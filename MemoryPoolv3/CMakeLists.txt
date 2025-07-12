# 设置CMake最低版本
cmake_minimum_required(VERSION 3.10)

# 项目名称
project(MemoryPoolv2)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 编译选项
add_compile_options(-Wall -O2)

# 查找pthread库
find_package(Threads REQUIRED)

# /project
#     ├── CMakeLists.txt
#     ├── src/
#     ├── include/
#     └── main.cpp
# 在这种情况下，${CMAKE_SOURCE_DIR} 的值将是 /project。
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
# SRC_DIR 是你自定义的变量，用来存储源代码文件所在的目录。
# 通过 set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)，你将 SRC_DIR 设置为 ${CMAKE_SOURCE_DIR}/src，即项目根目录下的 src 文件夹路径。如果 CMAKE_SOURCE_DIR 是 /project，那么 SRC_DIR 就会是 /project/src。
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)

# 获取所有.cpp文件
file(GLOB SOURCES "${SRC_DIR}/*.cpp")

# 添加头文件目录
include_directories(${INC_DIR})

# 创建单元测试可执行文件
add_executable(unit_test 
    ${SOURCES}
    Test.cpp
)

# 创建性能测试可执行文件
add_executable(perf_test
    ${SOURCES}
    PerformanceTest.cpp
)

# 链接pthread库
target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)

# 添加测试命令
add_custom_target(test
    COMMAND ./unit_test
    DEPENDS unit_test
)

add_custom_target(perf
    COMMAND ./perf_test
    DEPENDS perf_test
)