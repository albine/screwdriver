cmake_minimum_required(VERSION 3.16)
project(TradingEngine)

# 强制使用 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库
find_package(Threads REQUIRED)

# ============================================================================
# Quill - 高性能异步日志库
# https://github.com/odygrd/quill
# ============================================================================
include(FetchContent)

FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG v7.5.0
)

# Quill 配置选项
set(QUILL_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(quill)

# 包含项目头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# 主引擎可执行文件
add_executable(engine src/main.cc)
target_link_libraries(engine
    Threads::Threads
    quill::quill
)

# 日志示例程序
add_executable(logger_example src/logger_example.cc)
target_link_libraries(logger_example
    Threads::Threads
    quill::quill
)
