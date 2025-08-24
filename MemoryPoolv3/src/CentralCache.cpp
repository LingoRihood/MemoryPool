#include "CentralCache.h"
#include "PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace MemoryPoolv2 {
// const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

// 当线程缓存（ThreadCache）不足时，会调用此函数从中心缓存（CentralCache）批量获取内存。
// 如果中心缓存没有可用内存，则进一步从底层的页缓存（PageCache）获取大块内存并切分为小块。
void* CentralCache::fetchRange(size_t index, size_t batchNum) {
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if(index >= FREE_LIST_SIZE || batchNum == 0) {
        return nullptr; // 索引越界，无法获取内存
    }

    // 自旋锁保护
    // 线程A:   获取锁成功 → 临界区执行 → 释放锁
    // 线程B:   获取锁失败 → 忙等待自旋 → 锁释放 → 获取锁成功
    // 线程C:   获取锁失败 → 忙等待自旋 → 锁释放 → 获取锁成功
    // test_and_set() 是 std::atomic_flag 唯一提供的修改方法：
    // 原子地将标志位设置为 true（表示“已锁定”）
    // 如果test_and_set()返回true（表示锁被其他线程占有），则循环继续，进入自旋等待状态。
    // 如果返回false（表示锁未被占有），则成功获取锁，退出循环，进入临界区。

    // Acquire 语义的作用：
    // 确保后续的内存操作不会被重排到锁的获取之前。
    // 使得当前线程在获取锁之后，能够安全地看到之前线程释放锁前的所有内存写入操作的效果。
    // 释放线程:  [内存修改操作] → 锁释放 (release)
    // 获取线程:  锁获取 (acquire) → [读取释放线程的修改操作]
    // memory_order_acquire提供了一种内存屏障（Memory Barrier）：
    // 确保当前线程在获取锁（或原子变量）后，后续的内存读写操作一定不会被重排到锁获取之前。
    // 从而保障了线程看到的内存状态和预期是一致的
    while(locks_[index].test_and_set(std::memory_order_acquire)) {
        // 添加线程让步，避免忙等待，避免过度消耗CPU
        // yield() 函数的作用：
        // 提示操作系统主动让出当前线程的CPU时间片，给其他线程使用。
        // 避免“纯忙等待”导致CPU占用率飙高，降低系统整体吞吐量。
        // 线程等待获取锁 (spin):
        // 尝试获取锁 → 锁被占用 → 调用yield() → 暂停线程运行
        //                       └→ 调度器调度其他线程运行
        // yield 的使用并不会挂起线程，只是告知调度器：
        // 当前线程可以暂时让步，调度其他线程运行。
        // 避免“无效的CPU忙等待”状态。
        std::this_thread::yield();
    }

    void* result = nullptr;
    try {
        // 尝试从中心缓存获取内存块
        // 在读取时使用松散的内存顺序（relaxed）来优化性能。
        // 在写入时使用释放内存顺序（release）来确保内存操作的正确顺序和数据一致性。
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if(!result) {
            // 若中心缓存为空，从底层页缓存（PageCache）获取新的内存
            // size 就是单个内存块大小
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);

            if(!result) {
                locks_[index].clear(std::memory_order_release);
                // 若页缓存也无法提供内存，释放锁并返回nullptr表示失败。
                return nullptr;
            }

            // 将获取的内存块切分成小块
            // 从更底层的PageCache成功获取到一大块连续内存（一个Span）后。
            // 现在要做的事情是：
            // 将这一大块内存切割成多个小块。
            // 构建链表，存入CentralCache的自由链表中，以便后续分配。
            // 从切割出的内存块中，取出一个返回给调用者（通常是ThreadCache），其他的存入中心缓存。
            // 转换为char*类型，便于后续地址运算（字节级偏移）。
            char* start = static_cast<char*>(result);

            // 8 * 4096 = 32768 (32KB) / size
            // 计算总块数
            size_t totalBlocks = (SPAN_PAGES * PageCache::PAGE_SIZE) / size; 

            size_t allocBlocks = std::min(batchNum, totalBlocks); // 实际分配的块数
            
            // 构建返回给ThreadCache的内存块链表
            if(allocBlocks > 1) {
                // 确保至少有两个块才构建链表
                // 构建链表
                for(size_t i = 1; i < allocBlocks; ++i) {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr; // 最后一个块指向nullptr
            } 

            // 构建保留在CentralCache的链表
            if(totalBlocks > allocBlocks) {
                void* remainStart = start + allocBlocks * size;
                for(size_t i = allocBlocks + 1; i < totalBlocks; ++i) {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr; // 最后一个块指向nullptr
                centralFreeList_[index].store(remainStart, std::memory_order_release); // 更新中心缓存的自由链表头
            }
        } else {
            // 如果中心缓存有index对应大小的内存块
            // 从现有链表中获取指定数量的块
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            while(current && count < batchNum) {
                prev = current; // 保留前一个块
                current = *reinterpret_cast<void**>(current); // 获取下一个块
                count++;
            }
            // 当前centralFreeList_[index]链表上的内存块大于batchNum时需要用到 
            if(prev) {
                *reinterpret_cast<void**>(prev) = nullptr;
            }
            centralFreeList_[index].store(current, std::memory_order_release); // 更新中心缓存的自由链表头
        }
    } catch(...) {
        // 发生异常时确保释放锁
        locks_[index].clear(std::memory_order_release);
        throw; // 重新抛出异常
    }

    // 释放锁
    locks_[index].clear(std::memory_order_release);
    return result;
}

// CentralCache::returnRange 函数的作用是将一段内存（或内存块）归还给中央缓存（central cache）以便后续重用
// 将归还的内存块组织为自由链表并连接到中央缓存相应索引(index)的链表头部。
// 更新缓存相关计数，并判断是否触发延迟归还机制。
// void* start: 这是指向待归还内存块的起始位置的指针。
// size_t size: 这是待归还内存块的大小。
// size_t index: 这是一个索引，表示内存块的类型或大小，决定该块归还到哪个空闲链表。
void CentralCache::returnRange(void* start, size_t size, size_t index) {
    if(!start || index >= FREE_LIST_SIZE) {
        return;
    }

    // memory_order_acquire：保证当前线程在获取锁之后的操作，不会被重排到锁之前，即 当前线程能看到锁前线程的所有操作。当锁被获取时，线程会保证 其后面的内存操作 在获取锁之前 不会被重排。这意味着，锁后的所有内存访问（例如修改或读取内存）不会发生乱序，确保我们在锁后能够看到前面的内存操作
    // 通过 memory_order_acquire，我们确保：
    // 在当前线程获取锁后，它能看到之前线程对共享内存的修改。
    // 获取锁的操作是 同步的，也就是获取锁之前的所有操作都能被当前线程看到。这样可以避免 “脏读” 问题，保证当前线程获取锁后，能准确看到前一个持锁线程所做的内存修改。
    while(locks_[index].test_and_set(std::memory_order_acquire)) {
        // 添加线程让步，避免忙等待
        std::this_thread::yield();
    }

    try {
        // 1. 将归还的链表连接到中心缓存
        void* end = start;
        size_t count = 1;

        // 通过这段代码，我们遍历链表直到找到最后一个有效的内存块，并将其标记为链表的末尾
        while(*reinterpret_cast<void**>(end) != nullptr && count < size) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        // 使用 std::memory_order_relaxed 来进行读取操作，因为这里并不需要对内存操作进行同步，只需要读取当前空闲链表的头部
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        // 头插法（将原有链表接在归还链表后边）
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);
        // 原链表：centralFreeList_[index] -> [X] -> [Y] -> ...
        // 归还链表：[start] -> [...] -> [end]
        // 连接后：
        // centralFreeList_[index] -> [start] -> [...] -> [end] -> [X] -> [Y] -> ...

    } catch(...) {
        // 发生异常时确保释放锁
        locks_[index].clear(std::memory_order_release);
        throw; // 重新抛出异常
    }
    locks_[index].clear(std::memory_order_release);
}


void* CentralCache::fetchFromPageCache(size_t size) {
    // 1. 计算实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. 根据大小决定分配策略
    if(size <= SPAN_PAGES * PageCache::PAGE_SIZE) {
        // 小于等于32KB的请求，使用固定8页
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    } else {
        // 大于32KB的请求，按实际需求分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

}