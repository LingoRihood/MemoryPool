#include "ThreadCache.h"
#include "CentralCache.h"
#include <cstdlib>

namespace MemoryPoolv2 {
    // 处理size==0的请求：至少分配一个对齐大小（如8字节）。
    // 大于MAX_BYTES的请求直接通过malloc分配。
    // 否则，尝试从线程缓存取内存：
    // 若线程缓存有可用内存，取出一个内存块返回。
    // 若没有，则调用fetchFromCentralCache从中心缓存批量获取内存。
    void* ThreadCache::allocate(size_t size) {
        // 处理0大小的分配请求
        if(size == 0) {
            // 至少分配一个对齐大小
            size = ALIGNMENT;
        }

        if(size > MAX_BYTES) {
            // 大对象直接从系统分配
            return malloc(size);
        }

        size_t alignedSize = SizeClass::roundUp(size);
        size_t index = SizeClass::getIndex(alignedSize);
        
        // 检查线程本地自由链表
        // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
        if(void* ptr = freeList_[index]) {
            // 更新对应自由链表的长度计数
            --freeListSize_[index];
            // freeList_[index] --> Block1 --> Block2 --> Block3 --> nullptr
            // 将freeList_[index]指向的内存块的下一个内存块地址（取决于内存块的实现）
            // 【trick】
            freeList_[index] = *reinterpret_cast<void**>(ptr);
            // freeList_[index] --> Block2 --> Block3 --> nullptr
            // 返回给用户: Block1
            return ptr;
        }

        // 如果线程本地自由链表为空，则从中心缓存获取一批内存
        return fetchFromCentralCache(index);
    }

    // 回收 用户释放的内存块。
    // 将释放的内存块插入到线程本地缓存中（即线程本地自由链表）。
    // 当线程缓存中的内存块超过阈值时，将多余的内存归还给中心缓存（CentralCache），以便平衡整体内存使用效率。
    void ThreadCache::deallocate(void* ptr, size_t size) {
        if(size > MAX_BYTES) {
            free(ptr);
            return;
        }

        size_t index = SizeClass::getIndex(size);

        // 插入到线程本地自由链表
        // freeList_[index] --> 原头节点0x2000 --> 0x3000 --> nullptr
        *reinterpret_cast<void**>(ptr) = freeList_[index];

        // freeList_[index] --> 新头节点(0x1000) --> 0x2000 --> 0x3000 --> nullptr
        freeList_[index] = ptr;

        // 更新对应自由链表的长度计数
        ++freeListSize_[index];

        // 判断是否需要将部分内存回收给中心缓存
        // 当线程缓存中的内存块过多时，会调用returnToCentralCache进行回收。
        // shouldReturnToCentralCache通常使用阈值判断，比如链表长度超过一定数量（例如256个）：
        // 如果达到阈值，就需要触发归还操作，以减少线程缓存占用过多的内存资源。
        // 若达到阈值，则将链表上的一部分内存批量归还中心缓存
        if(shouldReturnToCentralCache(index)) {
            returnToCentralCache(freeList_[index], size);
        }
    }

    // 判断是否需要将内存回收给中心缓存
    bool ThreadCache::shouldReturnToCentralCache(size_t index) {
        // 设定阈值，例如：当自由链表的大小超过一定数量时
        size_t threshold = 64;
        return (freeListSize_[index] > threshold);
    }

    // 线程缓存（本地链表）为空或不足时
    // ↓
    // 调用fetchFromCentralCache
    // ↓
    // 从中心缓存批量取内存块
    // ↓
    // 取出一个内存块返回，其余保存在本地
    void* ThreadCache::fetchFromCentralCache(size_t index) {
        size_t size = (index + 1) * ALIGNMENT; // 计算实际大小
        // 根据对象内存大小计算批量获取的数量
        size_t batchNum = getBatchNum(size);
        // 从中心缓存批量获取内存
        void* start = CentralCache::getInstance().fetchRange(index, batchNum);
        if(!start) {
            return nullptr; // 中心缓存没有可用内存
        }

        // 更新自由链表大小
        // 增加对应大小类的自由链表大小
        freeListSize_[index] += batchNum;

        // 取一个返回，其余放入线程本地自由链表
        void* result = start;
        if(batchNum > 1) {
            // 将start的下一个节点地址存入freeList_[index]
            freeList_[index] = *reinterpret_cast<void**>(start);
        }

        return result;
    }


    // 当线程本地缓存(ThreadCache)中的自由链表长度超过一定阈值时：
    // 保留一部分内存在线程本地缓存，以便快速满足后续请求。
    // 将多余的内存批量归还给中心缓存(CentralCache) ，避免线程缓存占用过多的内存。
    void ThreadCache::returnToCentralCache(void* start, size_t size) {
        // 根据大小计算对应的索引
        size_t index = SizeClass::getIndex(size);

        // 获取对齐后的实际块大小
        size_t alignedSize = SizeClass::roundUp(size);

        // 当前自由链表上的内存块数量 (batchNum)
        size_t batchNum = freeListSize_[index];
        // 如果只有一个块，则不归还
        if(batchNum <= 1) {
            return;
        }

        // 保留一部分在ThreadCache中（比如保留1/4）
        size_t keepNum = std::max(batchNum / 4, size_t(1));
        size_t returnNum = batchNum - keepNum;

        // 将内存块串成链表
        char* current = static_cast<char*>(start);

        // 使用对齐后的大小计算分割点
        char* splitNode = current;
        // 链表头(0x1000) -> 0x2000 -> 0x3000 -> 0x4000 -> nullptr
        // batchNum = 4, keepNum = 2, returnNum = 2

        // 遍历过程:
        // i = 0, splitNode: 0x1000 -> *reinterpret_cast<void**>(0x1000)=0x2000
        // (splitNode现在为0x2000, 保留2个节点即为0x1000和0x2000)

        // 原链表: 0x1000 -> 0x2000 -> 0x3000 -> 0x4000 -> nullptr
        // 保留:   0x1000 -> 0x2000 -> nullptr (本地缓存)
        // 归还:   0x3000 -> 0x4000 -> nullptr (中心缓存)

        for(size_t i = 0; i < keepNum - 1; ++i) {
            // splitNode = 0x1000
            // 内存状态：
            // [0x1000]: 存放下一个节点地址 (0x2000)
            // [0x2000]: 存放下一个节点地址 (0x3000)
            // [0x3000]: nullptr (链表尾)
            // reinterpret_cast<void**>(splitNode)
            // splitNode是0x1000，转为void**类型的指针。
            // 意味着从0x1000地址处读取的是一个void*指针。
            // 解引用*reinterpret_cast<void**>(splitNode)
            // 从内存位置0x1000读取一个指针，内容为0x2000。
            // 再用reinterpret_cast<char*>将0x2000转回char*类型。
            splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
            if(splitNode == nullptr) {
                // 如果链表提前结束，更新实际的返回数量
                returnNum = batchNum - (i + 1);
                break;
            }
        }
        if(splitNode != nullptr) {
            // 将要返回的部分和要保留的部分断开
            void* nextNode = *reinterpret_cast<void**>(splitNode);
            *reinterpret_cast<void**>(splitNode) = nullptr; // 断开链表

            // 更新ThreadCache的空闲链表
            freeList_[index] = start;

            // 更新自由链表大小
            freeListSize_[index] = keepNum;

            // 将剩余的内存块返回给中心缓存
            if(returnNum > 0 && nextNode != nullptr) {
                CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
            }
        }
    }

// 计算批量获取内存块的数量
size_t ThreadCache::getBatchNum(size_t size) {
    // 基准：每次批量获取不超过4KB内存
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 4KB

    // 根据对象大小设置合理的基准批量数
    size_t baseNum;
    if(size <= 32) {
        baseNum = 64;       // 64 * 32 = 2KB
    } else if(size <= 64) {
        baseNum = 32;       // 32 * 64 = 2KB
    } else if(size <= 128) {
        baseNum = 16;       // 16 * 128 = 2KB
    } else if(size <= 256) {
        baseNum = 8;        // 8 * 256 = 2KB
    } else if(size <= 512) {
        baseNum = 4;        // 4 * 512 = 2KB
    } else if(size <= 1024) {
        baseNum = 2;        // 2 * 1024 = 2KB
    } else {
        baseNum = 1;        // 大于1024直接获取一个
    }

    // 计算最大批量数
    size_t maxNum = std::max(size_t(1), MAX_BATCH_SIZE / size);

    // 取最小值，但确保至少返回1
    return std::max(size_t(1), std::min(baseNum, maxNum));
}
}