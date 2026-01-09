cmake_minimum_required(VERSION 3.10)
project(TradingEngine)

# 强制使用 C++17 (如果升级了GCC) 或 C++11 (如果用默认GCC)
set(CMAKE_CXX_STANDARD 17) 
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库 (CentOS 7 下必须链接 pthread)
find_package(Threads REQUIRED)

# 包含 moodycamel 头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include) 

add_executable(engine main.cpp)
target_link_libraries(engine Threads::Threads)