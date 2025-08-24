#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace MemoryPoolv2 {
// 这个函数的目的是根据请求的页数（numPages），为其分配一个内存块，返回其内存地址。
// 按页数申请
void* PageCache::allocateSpan(size_t numPages) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    // freeSpans_ 是一个以页数（numPages）为键、Span* 为值的映射容器。lower_bound 函数会查找第一个大于或等于 numPages 的键值。也就是说，it 会指向一个可以满足需求的 span（如果存在的话）
    auto it = freeSpans_.lower_bound(numPages);
    if(it != freeSpans_.end()) {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if(span->next) {
            freeSpans_[it->first] = span->next;
        } else {
            freeSpans_.erase(it);
        }

        // 如果span大于需要的numPages则进行分割
        // 当一个 span 中的页数多于请求的页数时，需要将 span 分成两个部分：一部分用于满足当前的内存请求，另一部分则被放回到空闲链表中
        if(span->numPages > numPages) {
            Span* newSpan = new Span;
            // newSpan->pageAddr 是超出部分的起始地址。通过将 span->pageAddr 向后偏移 numPages * PAGE_SIZE，我们得到超出部分的地址。也就是说，newSpan 的起始地址是原 span 地址加上已经分配的页数（numPages）
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 将超出部分放回空闲Span*列表头部
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages; // 更新原span的页数
        }

        // 记录span信息用于回收
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages);
    if(!memory) {
        return nullptr; // 系统分配失败
    }

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录span信息用于回收
    spanMap_[memory] = span;
    return memory;
}

// 这段代码是一个内存回收的函数，用于释放在 PageCache 中分配的内存块（span）。它的主要任务是将 ptr 指向的内存块（span）释放，并尝试将相邻的空闲内存块（span）合并成一个更大的空闲块，从而减少内存碎片。
void PageCache::deallocateSpan(void* ptr, size_t numPages) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对应的span，没找到代表不是PageCache分配的内存，直接返回
    auto it = spanMap_.find(ptr);
    if(it == spanMap_.end()) {
        return;
    }

    Span* span = it->second;

    // 尝试合并相邻的span
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);

    // 我们要释放一个内存块（span），需要看看它后面相邻的内存块（nextSpan）是不是空闲的。
    // | span（待释放）| nextSpan（空闲）| otherSpan（占用）|

    // 如果相邻的内存块（nextSpan）恰好是空闲的，我们就可以把两个内存块合并起来，这样就形成了一个更大的内存块，便于以后的再利用。

    // | span（空闲，扩大了）          | otherSpan（占用）|

    if(nextIt != spanMap_.end()) {
        Span* nextSpan = nextIt->second;

        // 1. 首先检查nextSpan是否在空闲链表中
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];

        // 检查是否是头节点
        if(nextList == nextSpan) {
            // 如果是头节点，直接把链表头指针指向下一个节点（nextSpan->next），这样nextSpan就从链表中移除了。
            // found置为true，表示成功找到并移除。
            nextList = nextSpan->next;
            found = true;
        } else if(nextList) {
            // 只有在链表非空时才遍历
            Span* prev = nextList;
            while(prev->next) {
                if(prev->next == nextSpan) {
                    // 将nextSpan从空闲链表中移除
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        // 2. 只有在找到nextSpan的情况下才进行合并
        if(found) {
            // 合并span
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            // 用于释放之前从PageCache中分配出去的内存span。
            // Span 是一个管理结构，它并不直接代表真正的内存块。
            // 真正被使用、分配和释放的内存区域由 span->pageAddr 指向，这个地址的内存由单独的机制（例如系统调用）分配和释放
            // Span 仅仅是记录或描述内存块的元数据
            // delete nextSpan 仅释放了管理结构Span自身的内存，而不是Span所描述的真正内存块（pages）
            delete nextSpan;
        }
    }

    // 将合并后的span通过头插法插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

// 它的目的是通过系统调用 mmap 向操作系统请求内存，并确保返回的内存块已经被清零。这个函数在内存池的实现中用于当无法从内部空闲内存池分配内存时，向操作系统请求更多的内存。
void* PageCache::systemAlloc(size_t numPages) {
    size_t size = numPages * PAGE_SIZE;

    // 使用mmap分配内存
    // mmap 是一个系统调用，用来映射文件或设备到内存地址空间，但在这里它用于请求匿名内存, 即与任何文件无关的内存区域。
    // nullptr：mmap 的第一个参数是期望映射的内存地址。这里传入 nullptr 表示让操作系统自动选择一个合适的地址。
    // PROT_READ | PROT_WRITE: 第三个参数指定了映射内存的访问权限，这里设置为 可读可写。
    // MAP_PRIVATE | MAP_ANONYMOUS: 
    // MAP_PRIVATE 表示创建的映射是私有的，不会影响到其他进程的内存。
    // MAP_ANONYMOUS 表示分配的内存不与任何文件关联，而是匿名内存，通常用于内存池等场景。
    // -1: 第五个参数表示没有文件描述符，表示内存映射不与任何文件关联，因为我们是申请匿名内存。
    // 0: 第六个参数通常表示文件的偏移量，但在匿名映射中它没有实际意义。
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // 如果 mmap 调用成功，ptr 会返回映射的内存地址。如果失败，ptr 将会是 MAP_FAILED，表示分配内存失败。
    if(ptr == MAP_FAILED) {
        return nullptr; // 分配失败
    }

    // 对齐到 8 字节
    // uintptr_t alignedPtr = reinterpret_cast<uintptr_t>(ptr);
    // alignedPtr = (alignedPtr + 7) & ~static_cast<uintptr_t>(7);  // 对齐到 8 字节
    // ptr = reinterpret_cast<void*>(alignedPtr);

    
    // 清零内存
    // memset 用于将分配到的内存初始化为零，确保返回的内存块不会包含任何旧数据，符合内存分配的安全标准。
    memset(ptr, 0, size);
    return ptr;
}

}