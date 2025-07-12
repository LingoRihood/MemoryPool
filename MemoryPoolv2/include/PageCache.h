#pragma once
#include "Common.h"
#include <map>
#include <mutex>

namespace MemoryPoolv2 {
class PageCache {
public:
    static const size_t PAGE_SIZE = 4096; // 每页大小为4KB

    // 单例模式，确保全局只有一个PageCache实例，统一管理页面缓存
    static PageCache& getInstance() {
        // 使用 C++11 的 static 特性实现线程安全的单例模式。
        // 保证内存池内存管理全局唯一实例，避免多个实例的冲突或内存浪费。
        static PageCache instance;
        return instance;
    }

    // 分配指定页数的span
    void* allocateSpan(size_t numPages);

    // 释放span
    void deallocateSpan(void* ptr, size_t numPages);

private:
    PageCache() = default;

    // 向系统申请内存
    void* systemAlloc(size_t numPages);
private:
    // Span表示一段连续的内存页，用于统一管理
    struct Span {
        // 页起始地址
        void* pageAddr;

        // 页数
        // numPages 表示 Span 占据多少个连续页；
        size_t numPages;

        // 链表指针
        Span* next;
    };

    // 按页数管理空闲span，不同页数对应不同Span链表
    // 以页数（numPages）为键，存储链表头（Span*）
    std::map<size_t, Span*> freeSpans_;

    // 页号到span的映射，用于回收
    // 以起始地址（pageAddr）为键，存储对应的 Span 信息。
    std::map<void*, Span*> spanMap_;
    std::mutex mutex_; // 保护freeSpans_和spanMap_的互斥锁
};
}