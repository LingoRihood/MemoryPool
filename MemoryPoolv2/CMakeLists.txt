cmake_minimum_required(VERSION 3.10)

# 项目名称
project(MemoryPoolv2)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 明确指定项目源文件（推荐）
add_executable(MemoryPoolv2
    Test.cpp
    MemoryPool.h
    Common.h
    PageCache.h
    PageCache.cpp
    ThreadCache.h
    ThreadCache.cpp
    CentralCache.h
    CentralCache.cpp
)

# 包含目录（如有需要明确指定）
target_include_directories(MemoryPoolv2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 推荐开启的额外编译选项
target_compile_options(MemoryPoolv2 PRIVATE -Wall -Wextra -O2)

# Add compile options
target_compile_options(${PROJECT_NAME} PRIVATE -g -pthread)

# Link libraries
target_link_libraries(${PROJECT_NAME} pthread)