cmake_minimum_required(VERSION 3.10)
project(TradingEngine)

# 强制使用 C++17 (如果升级了GCC) 或 C++11 (如果用默认GCC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库 (CentOS 7 下必须链接 pthread)
find_package(Threads REQUIRED)

# 包含头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============ Quill 日志库 (FetchContent) ============
include(FetchContent)
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG v7.5.0
)
FetchContent_MakeAvailable(quill)

add_executable(engine
    src/main.cpp
    src/FastOrderBook.cpp
)
target_link_libraries(engine
    Threads::Threads
    quill::quill
)

# 日志模块测试程序
add_executable(test_logger test_logger.cpp)
target_link_libraries(test_logger
    Threads::Threads
    quill::quill
)

# FastOrderBook历史数据回放测试
add_executable(test_fastorderbook
    test_fastorderbook.cpp
    src/FastOrderBook.cpp
)
target_include_directories(test_fastorderbook PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_fastorderbook
    Threads::Threads
    quill::quill
)