#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include "MemoryPool.h"

using namespace MemoryPoolv1;

// 测试用例
class P1 {
    int id_;
};

class P2 {
    int id_[5];
};

class P3
{
    int id_[10];
};

class P4
{
    int id_[20];
};

// 单轮次申请释放次数 线程数 轮次
void BenchmarkMemoryPool(size_t ntimes, size_t nworks, size_t rounds) {
    // 线程池
    std::vector<std::thread> vthread(nworks);
    // 统计所有线程在所有轮次中的总耗时（时钟计数）。
    size_t total_costtime = 0;
    // 创建 nworks 个线程
    for(size_t k = 0; k < nworks; ++k) {
        // 线程对象被构造后，立即启动线程并开始执行指定的任务内容。
        // 线程在创建的同时就已经开始运行。
        vthread[k] = std::thread([&]() {
            // 这里的代码是线程的具体工作内容
            for(size_t j = 0; j < rounds; ++j) {
                size_t begin1 = clock();
                for(size_t i = 0; i < ntimes; ++i) {
                    // 内存池对外接口
                    P1* p1 = newElement<P1>();
                    deleteElement<P1>(p1);
                    P2* p2 = newElement<P2>();
                    deleteElement<P2>(p2);
                    P3* p3 = newElement<P3>();
                    deleteElement<P3>(p3);
                    P4* p4 = newElement<P4>();
                    deleteElement<P4>(p4);
                }
                size_t end1 = clock();
                total_costtime += end1 - begin1;
            }
        });
    }
    for(auto &t: vthread) {
        t.join();
    }
    printf("%lu个线程并发执行%lu轮次，每轮次newElement&deleteElement %lu次，总计花费：%lu ms\n", nworks, rounds, ntimes, total_costtime);
}

void BenchmarkNew(size_t ntimes, size_t nworks, size_t rounds) {
    std::vector<std::thread> vthread(nworks);
    size_t total_costtime = 0;
    for(size_t i = 0; i < nworks; ++i) {
        vthread[i] = std::thread([&]() {
            for(size_t j = 0; j < rounds; ++j) {
                size_t begin1 = clock();
                for(size_t k = 0; k < ntimes; ++k) {
                    P1* p1 = new P1;
                    delete p1;
                    P2* p2 = new P2;
                    delete p2;
                    P3* p3 = new P3;
                    delete p3;
                    P4* p4 = new P4;
                    delete p4;                    
                }
                size_t end1 = clock();
                total_costtime += end1 - begin1;
            }
        });
    }
    for(auto& t: vthread) {
        t.join();
    }
    // %lu即“long unsigned”，通常用于输出size_t（无符号长整数类型） 
    printf("%lu个线程并发执行%lu轮次，每轮次new&delete %lu次，总计花费：%lu ms\n", nworks, rounds, ntimes, total_costtime);
}

int main() {
    // 使用内存池接口前一定要先调用该函数
    // static MemoryPool MemoryPool[MEMORY_POOL_NUM];
    // static MemoryPool MemoryPool[64];
    // 循环索引 (i)	内存槽大小（字节）
    // 0	        8 字节
    // 1	        16 字节
    // 2	        24 字节
    HashBucket::initMemoryPool();
    // 测试内存池
    BenchmarkMemoryPool(100, 1, 10);
    std::cout << "===========================================================================" << std::endl;
	std::cout << "===========================================================================" << std::endl;
    std::cout << "===========================================================================" << std::endl;
	std::cout << "===========================================================================" << std::endl;
    
    // 测试 new delete
    BenchmarkNew(100, 1, 10);
    return 0;
}