#pragma once
#include "Common.h"

//           +------------+     allocate
// 线程A --> | ThreadCache| ---> 用户请求内存
//           +------------+
//              ↑     |
//              |     v
//           中心缓存（CentralCache）
//              ↑     |
//              |     v
//           +------------+     allocate
// 线程B --> | ThreadCache| ---> 用户请求内存
//           +------------+


namespace MemoryPoolv2 {
// 线程本地缓存
class ThreadCache {
public:
    static ThreadCache* getInstance() {
        // 线程本地存储意味着每个线程都有一份独立的变量副本，这些副本彼此互不干扰。
        static thread_local ThreadCache instance;
        return &instance;
    }

    // 负责为用户提供指定大小（size）的内存块。
    // 首先尝试从线程本地缓存（自由链表数组）中获取。
    // 若本地缓存不足，则通过fetchFromCentralCache从中心缓存获取新的内存块。
    void* allocate(size_t size);

    // 将用户释放的内存放回线程本地缓存。
    // 若线程本地缓存超过一定阈值，则将多余内存通过returnToCentralCache归还给中心缓存
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache() = default;

    // 从中心缓存获取内存
    // 当线程本地缓存不足以满足请求时调用
    // 作用是从中心缓存（Central Cache）中请求内存，并填充到本地缓存。
    // 中心缓存通常为多线程共享，需要同步访问。
    void* fetchFromCentralCache(size_t index);

    // 归还内存到中心缓存
    // 将多余的本地缓存归还给中心缓存。
    // 防止单个线程持有过多内存，降低整体内存占用。
    void returnToCentralCache(void* start, size_t size);

    // 计算批量获取内存块的数量
    size_t getBatchNum(size_t size);

    // 判断当前链表中的内存块数量是否超过阈值。
    // 当超过阈值时，触发归还内存给中心缓存，以避免内存浪费
    bool shouldReturnToCentralCache(size_t index);

private:
    // 每个线程的自由链表数组
    std::array<void*, FREE_LIST_SIZE> freeList_;
    // 自由链表大小统计   
    std::array<size_t, FREE_LIST_SIZE> freeListSize_;
};

}   // namespace MemoryPoolv2