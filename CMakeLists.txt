cmake_minimum_required(VERSION 3.10)
project(TradingEngine)

# 强制使用 C++17 (如果升级了GCC) 或 C++11 (如果用默认GCC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库 (CentOS 7 下必须链接 pthread)
find_package(Threads REQUIRED)

# 包含头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# 链接目录（必须在 add_executable 之前）
link_directories(${CMAKE_SOURCE_DIR}/fastfish/libs)

# ============ FetchContent 优化：禁用更新检查 ============
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)  # 已下载后不再检查更新

# ============ Quill 日志库 ============
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG v7.5.0
    GIT_SHALLOW TRUE      # 浅克隆，只下载最新版本
)
FetchContent_MakeAvailable(quill)

# ============ nlohmann/json ============
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")  # 禁用 json 测试
FetchContent_MakeAvailable(json)

# ============ ZeroMQ ============
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

add_executable(engine
    src/main.cpp
    src/FastOrderBook.cpp
    src/strategy_base.cpp
)
target_include_directories(engine PRIVATE
    ${CMAKE_SOURCE_DIR}/fastfish/include/mdc_gateway_client/include
    ${CMAKE_SOURCE_DIR}/fastfish/include/protobuf-3.1.0/include
    ${CMAKE_SOURCE_DIR}/fastfish/src/protobuf
    ${CMAKE_SOURCE_DIR}/src
    ${ZMQ_INCLUDE_DIRS}
)
# 重要：与 fastfish 库的 ABI 兼容性设置
target_compile_definitions(engine PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_link_libraries(engine
    Threads::Threads
    quill::quill
    nlohmann_json::nlohmann_json
    ${ZMQ_LIBRARIES}
    aeron_client_shared
    aeron_driver
    ACE
    ACE_SSL
    protobuf
    mdc_gateway_client
    ssl
    crypto
)
# 设置 rpath，让程序在运行时能找到 fastfish/libs 目录下的库
set_target_properties(engine PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/fastfish/libs"
    INSTALL_RPATH "${CMAKE_SOURCE_DIR}/fastfish/libs"
    LINK_FLAGS "-Wl,-rpath,${CMAKE_SOURCE_DIR}/fastfish/libs"
)
