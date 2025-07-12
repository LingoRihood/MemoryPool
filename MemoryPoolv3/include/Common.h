#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace MemoryPoolv2 {
// 对齐数和大小定义
constexpr size_t ALIGNMENT = 8;
// 256KB
constexpr size_t MAX_BYTES = 256 * 1024;
// ALIGNMENT等于指针void*的大小
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; 

// 内存块头部信息
struct BlockHeader {
    // 内存块大小
    size_t size;
    // 使用标志
    bool inUse;
    // 指向下一个内存块
    BlockHeader* next;
};

// 大小类管理
class SizeClass {
public:
    // static不绑定到类的具体对象
    // 静态成员函数不属于任何实例对象，而是属于类本身。调用静态成员函数时无需创建类的对象，直接通过类名调用。
    // 确保所有分配的内存大小都是8字节的整数倍，便于内存对齐。
    static size_t roundUp(size_t bytes) {
        // 假设 ALIGNMENT = 8，用户请求了15字节：
        // 15 + 8 - 1 = 22
        // 22的二进制: 00010110
        // ~(8-1) = ~(7) = 11111000
        // 00010110 & 11111000 = 00010000，结果是16，刚好是8的整数倍且大于等于15。
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }
    
    // 目的：计算对应空闲链表的索引号，以快速定位空闲链表数组的位置。
    static size_t getIndex(size_t bytes) {
        // 确保bytes至少为ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        // 请求8字节： (8+7)/8 - 1 = 1 - 1 = 0

        // 8字节请求会映射到数组的第0个位置。

        // 请求16字节： (16+7)/8 - 1 = 2 - 1 = 1

        // 16字节请求映射到数组第1个位置。

        // 以此类推，每个大小请求迅速找到对应的空闲链表。
        // 向上取整后-1
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};
} // namespace MemoryPoolv2