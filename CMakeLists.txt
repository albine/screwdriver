cmake_minimum_required(VERSION 3.10)
project(TradingEngine)

# 生成 compile_commands.json 供 clangd 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 强制使用 C++17 (如果升级了GCC) 或 C++11 (如果用默认GCC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库 (CentOS 7 下必须链接 pthread)
find_package(Threads REQUIRED)

# 包含头文件路径
include_directories(${CMAKE_SOURCE_DIR}/include)

# 链接目录（必须在 add_executable 之前）
link_directories(${CMAKE_SOURCE_DIR}/fastfish/libs)

# ============ Quill 日志库 (本地) ============
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/quill)

# ============ nlohmann/json (本地) ============
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/json)

# ============ ZeroMQ ============
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

add_executable(engine
    src/main.cpp
    src/FastOrderBook.cpp
    src/strategy_base.cpp
    src/strategy_context.cpp
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

# ============ mmap_to_clickhouse 工具 ============
# 用于快速导出 mmap 二进制文件到 TSV 格式（供 ClickHouse 导入）
add_executable(mmap_to_clickhouse
    src/mmap_to_clickhouse.cpp
)
target_include_directories(mmap_to_clickhouse PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(mmap_to_clickhouse
    Threads::Threads
)
set_target_properties(mmap_to_clickhouse PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# ============ verify_orderbook 测试工具 ============
# 验证 FastOrderBook 重建的十档盘口与交易所快照是否一致
add_executable(verify_orderbook
    test/verify_orderbook.cpp
    src/FastOrderBook.cpp
)
target_include_directories(verify_orderbook PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(verify_orderbook
    Threads::Threads
    quill::quill
)
set_target_properties(verify_orderbook PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
