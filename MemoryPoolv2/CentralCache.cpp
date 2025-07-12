#include "CentralCache.h"
#include "PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace MemoryPoolv2 {
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

// 初始化中心缓存的数据结构（如空闲链表、自旋锁、延迟归还计数等）。
// 为后续内存分配和回收做好准备。
// 构造函数仅在程序启动时执行一次，因此是单线程执行环境，可以安全使用较宽松的内存序。
CentralCache::CentralCache() {
    for(auto& ptr: centralFreeList_) {
        // store() 是std::atomic类型的一个成员函数。它的作用是原子地给原子变量赋值。
        // 此时不存在其他线程同时访问这些变量。
        // 只有单一线程执行该初始化操作，无数据竞争。
        // 所以，可以安全地使用宽松顺序以获得更高的性能。
        ptr.store(nullptr, std::memory_order_relaxed);
    }

    for(auto& lock: locks_) {
        // 每个 atomic_flag 就是一个锁，具有两种状态：
        // 已设置 (set)：代表“锁定”状态。
        // 已清除 (clear)：代表“未锁定”状态。
        lock.clear();
    }

    // 初始化延迟归还相关的成员变量
    for(auto& count: delayCounts_) {
        count.store(0, std::memory_order_relaxed);
    }

    for(auto& time: lastReturnTimes_) {
        // steady_clock::now() 静态方法返回当前稳定时钟的时间点
        time = std::chrono::steady_clock::now();
    }

    // spanCount_用于记录当前在使用的span数量。
    // 初始化为0，表示启动时尚未使用任何span。
    spanCount_.store(0, std::memory_order_relaxed);
}

// 当线程缓存（ThreadCache）不足时，会调用此函数从中心缓存（CentralCache）批量获取内存。
// 如果中心缓存没有可用内存，则进一步从底层的页缓存（PageCache）获取大块内存并切分为小块。
void* CentralCache::fetchRange(size_t index) {
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if(index >= FREE_LIST_SIZE) {
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

            // 计算实际分配的页数
            // 检查当前请求的内存块size是否小于或等于一个默认的最大分配单位（SPAN_PAGES页大小）。
            // 例如若SPAN_PAGES = 8, PAGE_SIZE = 4096字节，则阈值为：
            // 8 * 4096 = 32768字节 (32KB)
            // 因此条件可理解为： 请求大小 ≤ 32KB 吗？      
            // (a + b - 1)/b 这是经典的向上取整 (round up) 技巧，保证分配页数刚好足够容纳所需大小。      
            // (5000 + 4096 - 1) / 4096 = (9095 / 4096) = 2
            // 确定从页缓存分配的页数 (numPages)
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE)?
                                SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;  
            
            // 使用实际页数计算分配的内存块数量 (blockNum)
            // 用总内存除以单个块的大小，得到可以切割出的小块数
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            if(blockNum > 1) {
                // 确保至少有两个块才构建链表
                // [start] (0x1000) -> 存放0x1200
                // [start+500] (0x1200) -> 存放0x1400
                // [start+1000] (0x1400) -> 存放0x1600
                // ...
                // [start+(blockNum-1)*500] -> 存放nullptr (链表尾)

                // [start, start+size-1]，[start+size, start+2*size-1], … [start+(blockNum-1)*size, start+blockNum*size-1]
                for(size_t i = 1; i < blockNum; ++i) {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    // [start] -> [start+size] 
                    // 相当于**p -> *q
                    *reinterpret_cast<void**>(current) = next;
                }
                // 明确标记链表尾部，防止越界访问。
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr; // 最后一个块指向nullptr

                // 保存result的下一个节点
                void* next = *reinterpret_cast<void**>(result);

                // 将result与链表断开
                *reinterpret_cast<void**>(result) = nullptr;
                // [result(已分配出去)](nullptr) 
                // [next节点] → [next节点后续链表] → … → nullptr

                // 更新中心缓存的自由链表
                // CentralCache[index] → next节点 → … → nullptr
                centralFreeList_[index].store(next, std::memory_order_release);
                // 原链表：
                // [result] → [next] → [next+size] → …
                // 取出[result]后：
                // CentralCache自由链表头：[next] → [next+size] → … → nullptr

                // 为什么需要记录Span信息？
                // 中心缓存是管理小块内存的缓存，当空闲内存块全部释放后，中心缓存必须知道原始的连续大内存在哪里，才能归还给底层的PageCache，防止产生内存碎片。
                
                // 使用无锁方式记录span信息
                // 做记录是为了将中心缓存多余内存块归还给页缓存做准备。考虑点：
                // 1.CentralCache 管理的是小块内存，这些内存可能不连续
                // 2.PageCache 的 deallocateSpan 要求归还连续的内存
                size_t trackerIndex = spanCount_++;
                // 为了后续释放时，能把小块内存拼接回大块内存，归还给PageCache，避免碎片 
                if(trackerIndex < spanTrackers_.size()) {
                    spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    // 共分配了blockNum个内存块
                    spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    // 第一个块result已被分配出去，所以初始空闲块数为blockNum - 1
                    spanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release);
                }
            }
        } else {
            // 保存result的下一个节点
            // 假设链表: [A]->[B]->[C]->nullptr
            // 如果result指向节点A：
            //     next = B;
            void* next = *reinterpret_cast<void**>(result);

            // 将result与链表断开
            *reinterpret_cast<void**>(result) = nullptr;
            // 原链表：[A]->[B]->[C]
            // 操作后：result = A（分离出去）;
            //         剩余链表变为：[B]->[C]

            // 更新中心缓存的自由链表
            // centralFreeList_[index]原来指向A；
            // 现在更新为指向B；
            centralFreeList_[index].store(next, std::memory_order_release);

            // 更新span的空闲计数
            // 首先，根据当前被取出的内存块地址（result），通过getSpanTracker(result)获得对应的SpanTracker
            SpanTracker* tracker = getSpanTracker(result);
            if(tracker) {
                // 减少一个空闲块
                // fetch_sub 是 C++ 中 std::atomic 提供的原子操作。
                // 它的含义是以线程安全的方式，对变量进行减法操作，并返回原先的值。
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
            }
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

    // 如果 index = 0，那么 index + 1 = 1，表示第一个类型的内存块大小为 ALIGNMENT；如果 index = 1，则表示第二个类型的内存块大小为 2 * ALIGNMENT，依此类推。
    size_t blockSize = (index + 1) * ALIGNMENT;
    // 计算待归还内存区域所包含的内存块数量
    size_t blockCount = size / blockSize;

    while(locks_[index].test_and_set(std::memory_order_acquire)) {
        // 添加线程让步，避免忙等待
        std::this_thread::yield();
    }

    try {
        // 1. 将归还的链表连接到中心缓存
        void* end = start;
        size_t count = 1;

        // 通过这段代码，我们遍历链表直到找到最后一个有效的内存块，并将其标记为链表的末尾
        while(*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
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

        // 2. 更新延迟计数
        // 意味着每次调用此函数时，延迟计数自动增加1。
        // 返回的是增加前的旧值，因此需要+ 1得到增加后的当前值
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        // 3. 检查是否需要执行延迟归还
        if(shouldPerformDelayedReturn(index, currentCount, currentTime)) {
            performDelayedReturn(index);
        }
    } catch(...) {
        // 发生异常时确保释放锁
        locks_[index].clear(std::memory_order_release);
        throw; // 重新抛出异常
    }
    locks_[index].clear(std::memory_order_release);
}

// 决定是否应该执行延迟归还操作。延迟归还是内存管理中优化性能的一种方式，特别是在高并发场景下，可以减少频繁的内存归还操作
// 检查是否需要执行延迟归还
bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime) {
    // 基于计数和时间的双重检查
    if(currentCount >= MAX_DELAY_COUNT) {
        return true; // 达到最大延迟归还次数
    }
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL; // 超过延迟时间间隔
}

// 执行延迟归还
// 将之前缓存（积累）在中央空闲链表中的内存块进行检查和清理。
// 判断每个内存块对应的SpanTracker的空闲块数量，如果达到一定阈值，则将整个span归还到更高级别的内存管理器（如PageCache）。
// 批量归还操作后，重置相关的计数器和时间戳。
// 简单来说，就是批量清理和归还缓存内存，降低内存碎片和内存占用。
void CentralCache::performDelayedReturn(size_t index) {
    // 重置延迟计数
    delayCounts_[index].store(0, std::memory_order_relaxed);

    // 更新最后归还时间
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 统计每个span的空闲块数
    // 定义一个unordered_map容器用于临时统计每个SpanTracker对应的空闲块数量。
    // key为SpanTracker*，表示每个span；
    // value为size_t，表示统计出来的该span中空闲块的数量。
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;

    // currentBlock 是当前空闲链表的头部，它指向归还到中央空闲链表中的第一个内存块
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    // 调用getSpanTracker(currentBlock)来找到这个内存块所在的span。
    // 如果找到对应的SpanTracker，则在spanFreeCounts哈希表中，对应的计数+1。
    while(currentBlock) {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if(tracker) {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }
    // 更新每个span的空闲计数并检查是否可以归还
    for(const auto& [tracker, newFreeBlocks]: spanFreeCounts) {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
    }
}

// 负责更新内存块范围（Span）中空闲块的计数，并决定是否将整个 Span 归还给内存池
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index) {
    // 读取旧值用 relaxed：只需要读取值，不需要同步或互斥。
    // 存储新值用 release：需要确保其他线程可以看到最新的空闲块计数，以保证正确性。
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    // 如果更新后的空闲块数量 (newFreeCount) 等于 Span 总的内存块数量（tracker->blockCount），表明这个 Span 中的所有块都空闲了。
    // 此时，意味着整个 Span 不再使用，可以归还给更高级别的内存管理层（PageCache）
    // 如果所有块都空闲，归还span
    if(newFreeCount == tracker->blockCount.load(std::memory_order_relaxed)) {
        // 负责更新内存块范围（Span）中空闲块的计数，并决定是否将整个 Span 归还给内存池
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        // tracker->numPages.load(std::memory_order_relaxed)：原子读取当前 Span 的页数（numPages），表示 Span 占用的内存页数
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // 从自由链表中移除这些块
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr;
        void* prev = nullptr;
        void* current = head;

        while(current) {
            void* next = *reinterpret_cast<void**>(current);
            // 检查current是否在spanAddr范围内
            if(current >= spanAddr && current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
                if(prev) {
                    *reinterpret_cast<void**>(prev) = next; // 跳过当前块
                } else {
                    newHead = next; // 更新头部
                }
            } else {
                prev = current; // 保留前一个块
            }
            current = next; // 继续遍历
        }
        // 假设链表原状态：
        // [Block A (span1)] → [Block B (span2)] → [Block C (span1)] → nullptr

        // 此时归还span1：
        // - Block A 和 Block C 属于 span1，必须移除；
        // - 新链表变成：
        // [Block B (span2)] → nullptr
        centralFreeList_[index].store(newHead, std::memory_order_release);
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
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

SpanTracker* CentralCache::getSpanTracker(void* blockAddr) {
    // 遍历spanTrackers_数组，找到blockAddr所属的span
    for(size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); ++i) {
        void* spanAddr = spanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = spanTrackers_[i].numPages.load(std::memory_order_relaxed);

        if(blockAddr >= spanAddr && blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
            return &spanTrackers_[i];
        }
    }
    return nullptr; // 未找到对应的span
}

}