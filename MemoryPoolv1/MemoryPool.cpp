#include "MemoryPool.h"

namespace MemoryPoolv1 {
MemoryPool::MemoryPool(size_t BlockSize)
    : BlockSize_(BlockSize)
    , SlotSize_(0)
    , firstBlock_(nullptr)
    , curSlot_(nullptr)
    , freeList_(nullptr)
    , lastSlot_(nullptr) 
    {}

MemoryPool::~MemoryPool() {
    // 把连续的block删除
    Slot* cur = firstBlock_;
    while(cur) {
        Slot* next = cur->next;
        // 等同于 free(reinterpret_cast<void*>(firstBlock_));
        // 转化为 void 指针，因为 void 类型不需要调用析构函数，只释放空间
        // 在 C++ 中，operator delete 需要的是一个 void* 类型的指针，而不是特定类型的指针。因此，我们需要将 cur 从 Slot* 类型转换为 void* 类型，这样才能传递给 operator delete。
        // operator delete 是一个低级别的内存释放操作符，通常与 operator new 配对使用。
        // delete 是一个更高级的操作符，通常与 new 配对使用。它不仅释放内存，还会调用对象的析构函数。
        operator delete(reinterpret_cast<void*>(cur));
        cur = next;
    }
}

void MemoryPool::init(size_t size) {
    assert(size > 0);
    SlotSize_ = size;
    firstBlock_ = nullptr;
    curSlot_ = nullptr;
    freeList_ = nullptr;
    lastSlot_ = nullptr;
}

void* MemoryPool::allocate() {
    // 优先使用空闲链表中的内存槽
    Slot* slot = popFreeList();
    if(slot != nullptr) {
        return slot;
    }

    Slot* temp;
    {
        std::lock_guard<std::mutex> lock(mutexForBlock_);
        if(curSlot_ >= lastSlot_) {
            // 当前内存块已无内存槽可用，开辟一块新的内存
            allocateNewBlock();
        }

        temp = curSlot_;
        // 这里不能直接 curSlot_ += SlotSize_ 因为curSlot_是Slot*类型，所以需要除以SlotSize_再加1
        curSlot_ += SlotSize_ / sizeof(Slot);
    }

    return temp;
}

void MemoryPool::deallocate(void* ptr) {
    if(!ptr) {
        return;
    }
    Slot* slot = reinterpret_cast<Slot*>(ptr);
    pushFreeList(slot);
}

void MemoryPool::allocateNewBlock() {
    //std::cout << "申请一块内存块，SlotSize: " << SlotSize_ << std::endl;
    // 头插法插入新的内存块
    void* newBlock = operator new(BlockSize_);
    reinterpret_cast<Slot*>(newBlock)->next = firstBlock_;
    firstBlock_ = reinterpret_cast<Slot*>(newBlock);

    char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot*);
    // 计算对齐需要填充内存的大小
    size_t paddingSize = padPointer(body, SlotSize_);
    curSlot_ = reinterpret_cast<Slot*>(body + paddingSize);

    // 超过该标记位置，则说明该内存块已无内存槽可用，需向系统申请新的内存块
    lastSlot_ = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(newBlock) + BlockSize_ - SlotSize_ + 1);
}

// 让指针对齐到槽大小的倍数位置
size_t MemoryPool::padPointer(char* p, size_t align) {
    // uintptr_t类型
    // 是 C++ 标准库（定义于<cstdint>头文件）提供的一个无符号整型类型。
    // 特点：专门设计用于存放指针值，以整数方式操作内存地址。
    // 优点：保证能安全容纳任何指针地址，无论是32位还是64位架构下，都能正确表示。
    
    // reinterpret_cast的特性：
    // 最底层的强制类型转换符，直接按二进制值转换，不考虑语义。
    // 把内存地址从指针形式（地址）转换成整数形式，以便进行数学计算。
    // align 是槽大小
    uintptr_t result = reinterpret_cast<uintptr_t>(p);
    return (align - (result % align)) % align;
}

// 实现无锁入队操作
bool MemoryPool::pushFreeList(Slot* slot) {
    while(true) {
        // 获取当前头节点
        // load() 方法用于从一个原子变量读取当前值（线程安全的读取）。
        // 只保证安全读，但暂不要求内存同步其他数据状态。
        // 因为后续真正关键的同步是在CAS中。
        // oldHead
        //   |
        //   v
        // freeList_ -> SlotA -> SlotB -> nullptr
        Slot* oldHead = freeList_.load(std::memory_order_relaxed);

        // 将新节点的 next 指向当前头节点
        // 相当于 slot->next = oldHead; // 普通链表
        // slot -> oldHead -> SlotA -> SlotB -> nullptr
        slot->next.store(oldHead, std::memory_order_relaxed);

        // 尝试将新节点设置为头节点
        // 比较当前链表头（freeList_）是否等于之前读到的oldHead
        // std::memory_order_release表示若CAS成功，之前的所有操作（尤其是slot->next.store）对其他线程变为可见（内存同步完成）。
        // 相当于发布一个“同步成功”的信号。
        // memory_order_relaxed（失败情况）：
        // 若失败，暂不要求严格的内存同步，因为失败后立即重试。
        if(freeList_.compare_exchange_weak(oldHead, slot, std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}

// 实现无锁出队操作
Slot* MemoryPool::popFreeList() {
    while(true) {
        // 生产者线程用release发布数据；
        // 消费者线程用acquire获取最新发布的数据。

        // 用memory_order_acquire确保了线程间正确的数据同步
        Slot* oldHead = freeList_.load(std::memory_order_acquire);
        if(oldHead == nullptr) {
            // 队列为空
            return nullptr;
        }

        // 在访问 newHead 之前再次验证 oldHead 的有效性
        Slot* newHead = nullptr;
        try {
            newHead = oldHead->next.load(std::memory_order_relaxed);
        } catch(...) {
            // 如果返回失败，则continue重新尝试申请内存
            continue;
        }

        // 尝试更新头结点
        // std::memory_order_acquire：
        // 确保当前线程在执行此操作之后的所有读取操作不会被重排到此操作之前。这是为了确保你读取到的原子变量的值是正确且同步的。

        // std::memory_order_release：
        // 确保当前线程在执行此操作之前的所有写入操作不会被重排到此操作之后。这用于确保你对某个共享资源的修改对于其他线程是可见的。

        // std::memory_order_relaxed：
        // 不保证任何同步，指令可以自由重排，仅保证原子操作的顺序。
        // 原子性地尝试将 freeList_ 从 oldHead 更新为 newHead
        // bool compare_exchange_weak(T& expected, T desired, 
        //                    memory_order success_order,
        //                    memory_order failure_order);
        // 比较原子变量当前值与expected是否相等：
        // 如果相等：
        // 原子地将当前值修改为desired（期望值）。
        // 返回true表示CAS成功。
        // success_order	CAS成功时的内存顺序	memory_order_acquire
        // failure_order	CAS失败时的内存顺序	memory_order_relaxed
        // std::memory_order_acquire：表示如果CAS操作成功，那么所有随后的内存操作（如对freeList_的读取操作）不会被重排到CAS之前。确保你之后读取freeList_时是正确的。
        // 这就意味着，在CAS操作成功之后，你能获取到newHead的值，并且它之前的所有操作（如读取freeList_的值）都不会被重排。
        if(freeList_.compare_exchange_weak(oldHead, newHead, std::memory_order_acquire, std::memory_order_relaxed)) {
            return oldHead;
        }
        // 失败：说明另一个线程可能已经修改了 freeList_
        // CAS 失败则重试
    }
}

void HashBucket::initMemoryPool() {
    for(int i = 0; i < MEMORY_POOL_NUM; ++i) {
        getMemoryPool(i).init((i + 1) * SLOT_BASE_SIZE);
    }
}

// 单例模式
MemoryPool& HashBucket::getMemoryPool(int index) {
    // static 关键字确保该数组仅第一次调用时初始化一次，整个程序生命周期中只存在一份。
    // 也就是说，所有对getMemoryPool函数的调用，都会访问同一个内存池数组。
    static MemoryPool MemoryPool[MEMORY_POOL_NUM];
    return MemoryPool[index];
}

}