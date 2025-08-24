#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>

//   +------------------+        +----------------+        +--------------+
//   | ThreadCache 1..N |  <---> | CentralCache   |  <---> |  PageCache   |
//   +------------------+        +----------------+        +--------------+
//          一级缓存                  二级缓存                 底层缓存
//        线程本地缓存             多线程共享缓存             系统页缓存


// 减少线程间竞争：
// 每个线程先访问自己的ThreadCache。
// ThreadCache内存不足或超量时，向CentralCache批量获取或归还内存，减少频繁锁竞争
namespace MemoryPoolv2 {
// 使用无锁的span信息存储
// struct SpanTracker {
//     // 内存span的起始地址
//     std::atomic<void*> spanAddr{nullptr};
//     // span跨越的内存页数
//     std::atomic<size_t> numPages{0};
//     // 此span内总内存块数
//     std::atomic<size_t> blockCount{0};
//     // 用于追踪spn中还有多少块是空闲的，如果所有块都空闲，则归还span给PageCache
//     // 当前span内可用的内存块数
//     // 当freeCount == blockCount时，表明整个span空闲，可以归还给PageCache
//     std::atomic<size_t> freeCount{0};
// };

// 中心缓存的作用 是管理多个线程缓存间的内存调度，减少线程间的竞争。
class CentralCache {
public:
    // 中心缓存使用单例模式，保证只有一个实例
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    // 从中心缓存对应索引的自由链表中批量取出内存块给线程缓存。
    // 如果中心缓存不足，则调用更底层(PageCache)的接口获取更多内存。
    void* fetchRange(size_t index, size_t batchNum);

    // 线程缓存批量归还内存块给中心缓存。
    void returnRange(void* start, size_t size, size_t index);

private:
    // 初始化成员变量，包括自由链表、锁、自旋标志等
    // 相互是还所有原子指针为nullptr
    CentralCache(){
        for(auto& ptr: centralFreeList_) {
            ptr.store(nullptr, std::memory_order_relaxed);
        }
        // 初始化所有锁
        for(auto& lock: locks_) {
            lock.clear(std::memory_order_relaxed);
        }
    }
    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size);

    // 获取span信息
    // 根据给定的内存块地址快速找到对应的SpanTracker。
    // 一般通过一定的地址映射机制实现快速定位。
    // SpanTracker* getSpanTracker(void* blockAddr);

    // 更新span的空闲计数并检查是否可以归还
    // 每次回收或分配内存块时，更新span中剩余的空闲块数量。
    // 当span所有块都空闲时，触发归还span给底层页缓存。
    // void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

private:
    // 中心缓存的自由链表
    // 存储着从PageCache申请的内存块集合，提供给线程缓存快速批量获取。
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 用于同步的自旋锁
    // std::atomic_flag本质上是最简单、最轻量级的原子类型，它提供了线程安全的原子操作。
    // 当多个线程同时访问同一链表时，用于确保并发安全。
    // 性能远高于传统锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    // 使用数组存储span信息，避免map的开销
    // std::array<SpanTracker, 1024> spanTrackers_;
    // spanCount_记录当前使用了多少个span。
    // std::atomic<size_t> spanCount_{0};

    // 延迟归还相关的成员变量
    // 作用是实现延迟归还策略，减少频繁调用底层PageCache接口
    // 最大延迟计数
    // static const size_t MAX_DELAY_COUNT = 48;
    // 每个大小类的延迟计数
    // std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;
    // 上次归还时间
    // std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;
    // // 延迟间隔
    // static const std::chrono::milliseconds DELAY_INTERVAL;

    // 延迟归还策略的实现
    // bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
    // 实际执行归还给底层页缓存。
    // void performDelayedReturn(size_t index);
};
}