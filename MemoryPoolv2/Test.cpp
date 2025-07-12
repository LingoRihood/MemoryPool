#include "MemoryPool.h"
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cstring>
#include <random>
#include <algorithm>
#include <atomic>

using namespace MemoryPoolv2;

// 基础分配测试
void testBasicAllocation() {
    std::cout << "Running basic allocation test..." << std::endl;
    
    // 测试小内存分配
    void* ptr1 = MemoryPool::allocate(8);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 8);

    // 测试中等大小内存分配
    void* ptr2 = MemoryPool::allocate(1024);
    assert(ptr2 != nullptr);
    MemoryPool::deallocate(ptr2, 1024);

    // 测试大内存分配（超过MAX_BYTES）
    void* ptr3 = MemoryPool::allocate(1024 * 1024); // 1MB
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, 1024 * 1024);

    std::cout << "Basic allocation test passed!" << std::endl;
}

// 内存写入测试
void testMemoryWriting() {
    std::cout << "Running memory writing test..." << std::endl;

    // 分配并写入数据
    const size_t size = 128;
    char* ptr = static_cast<char*>(MemoryPool::allocate(size));
    assert(ptr != nullptr);

    // 写入数据
    for(size_t i = 0; i < size; ++i) {
        ptr[i] = static_cast<char>(i % 256); // 填充数据
    }

    // 验证数据
    for(size_t i = 0; i < size; ++i) {
        assert(ptr[i] == static_cast<char>(i % 256));
    }

    MemoryPool::deallocate(ptr, size);
    std::cout << "Memory writing test passed!" << std::endl;
}

// 多线程测试
// testMultiThreading() 用于测试自定义内存池 (MemoryPool) 在多线程并发环境下的正确性和线程安全性
void testMultiThreading() {
    std::cout << "Running multi-threading test..." << std::endl;

    const int NUM_THREADS = 4;
    // 每个线程执行1000次随机分配与释放操作。
    const int ALLOCS_PER_THREAD = 1000;
    // 原子标志，线程间共享，用于标记测试过程中是否出现错误。一旦出现错误，其他线程也可检测到并提前终止测试。
    std::atomic<bool> has_error{false};

    auto threadFunc = [&has_error]() {
        try {
            // allocations 用于存储当前线程已分配的内存指针和大小，以便后续释放。
            std::vector<std::pair<void*, size_t>> allocations;
            // reserve(ALLOCS_PER_THREAD) 表示预先为向量分配存储空间，以便可以存储至少ALLOCS_PER_THREAD个元素，而不需要中途不断扩容
            allocations.reserve(ALLOCS_PER_THREAD);

            for(int i = 0; i < ALLOCS_PER_THREAD && !has_error; ++i) {
                // 生成一个随机的内存分配大小
                // 每次随机生成一个介于8字节到2048字节之间的大小（8,16,24,...,2048）。
                // 这样分配的随机性更接近真实应用场景
                // rand() 是标准库函数，用于生成随机整数。
                // 生成范围：0 ~ RAND_MAX（RAND_MAX通常为32767或更大值）
                size_t size = (rand() % 256 + 1) * 8;
                void* ptr = MemoryPool::allocate(size);

                if(!ptr) {
                    std::cerr << "Allocation failed for size: " << size << std::endl;
                    has_error = true;
                    break;
                }

                allocations.push_back({ptr, size});

                // 随机决定（概率约50%）是否立即释放一个已分配的内存块。
                // 从已分配的内存列表中随机选一个内存块释放，并从列表中移除。
                // 模拟实际应用中内存的非规则分配和释放。
                if(rand() % 2 && !allocations.empty()) {
                    size_t index = rand() % allocations.size();
                    MemoryPool::deallocate(allocations[index].first, allocations[index].second);
                    allocations.erase(allocations.begin() + index);
                }
            }

            for(const auto& alloc: allocations) {
                MemoryPool::deallocate(alloc.first, alloc.second);
            }
        } catch(const std::exception& e) {
            std::cerr << "Thread exception: " << e.what() << std::endl;
            has_error = true;
        }
    };

    std::vector<std::thread> threads;
    for(int i = 0; i < NUM_THREADS; ++i) {
        // 相当于执行: threads.push_back(std::thread(threadFunc));
        threads.emplace_back(threadFunc);
    }

    for(auto& thread: threads) {
        // join()：等待线程完成，是“同步”操作。
        thread.join();
    }

    std::cout << "Multi-threading test passed!" << std::endl;
}

// 边界测试
void testEdgeCases() {
    std::cout << "Running edge cases test..." << std::endl;
    
    // 测试0大小分配
    void* ptr1 = MemoryPool::allocate(0);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 0);

    // 测试最小对齐大小
    void* ptr2 = MemoryPool::allocate(1);
    assert(ptr2 != nullptr);
    // 将 ptr2 指针强制转换为 uintptr_t，也就是 无符号整数类型（通常是 unsigned long）。
    // 作用：让我们可以对指针地址做位运算
    // 原理：只有当低几位都是 0 时，地址才能被 ALIGNMENT 整除。
    // 8字节对齐，地址末 3 位（二进制）必须是 000。
    assert((reinterpret_cast<uintptr_t>(ptr2) & (ALIGNMENT - 1)) == 0); // 确保对齐
    MemoryPool::deallocate(ptr2, 1);

    // 测试最大大小边界
    void* ptr3 = MemoryPool::allocate(MAX_BYTES);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, MAX_BYTES);

    // 测试超过最大大小的分配
    void* ptr4 = MemoryPool::allocate(MAX_BYTES + 1);
    assert(ptr4 != nullptr);
    MemoryPool::deallocate(ptr4, MAX_BYTES + 1);

    std::cout << "Edge cases test passed!" << std::endl;
}

// 压力测试
// testStress() 函数是一个内存池系统的 压力测试，旨在模拟多次内存分配和释放操作，并确保系统在大规模操作下的稳定性
void testStress() {
    std::cout << "Running stress test..." << std::endl;
    const int NUM_ITERATIONS = 10000;
    std::vector<std::pair<void*, size_t>> allocations;
    allocations.reserve(NUM_ITERATIONS);

    for(int i = 0; i < NUM_ITERATIONS; ++i) {
        size_t size = (rand() % 1024 + 1) * 8;
        void* ptr = MemoryPool::allocate(size);
        assert(ptr != nullptr);
        allocations.push_back({ptr, size});
    }

    // 随机顺序释放
    std::random_device rd;
    std::mt19937 g(rd());
    // 随机顺序释放：为了模拟更复杂的内存释放模式（避免顺序释放），使用了随机打乱释放顺序的方式
    std::shuffle(allocations.begin(), allocations.end(), g);
    for(const auto& alloc: allocations) {
        MemoryPool::deallocate(alloc.first, alloc.second);
    }

    std::cout << "Stress test passed!" << std::endl;
}

int main() {
    try {
        std::cout << "Starting memory pool tests..." << std::endl;
        testBasicAllocation();
        testMemoryWriting();
        testMultiThreading();

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}