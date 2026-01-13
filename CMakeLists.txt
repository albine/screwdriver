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
target_include_directories(engine PRIVATE
    ${CMAKE_SOURCE_DIR}/fastfish/include/mdc_gateway_client/include
    ${CMAKE_SOURCE_DIR}/fastfish/include/protobuf-3.1.0/include
    ${CMAKE_SOURCE_DIR}/fastfish/src/protobuf
    ${CMAKE_SOURCE_DIR}/src
)
# 重要：与 fastfish 库的 ABI 兼容性设置
target_compile_definitions(engine PRIVATE _GLIBCXX_USE_CXX11_ABI=0 USE_QUILL_LOGGER)
target_link_libraries(engine
    Threads::Threads
    quill::quill
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

# 日志模块测试程序
add_executable(test_logger test/test_logger.cpp)
target_link_libraries(test_logger
    Threads::Threads
    quill::quill
)

# FastOrderBook历史数据回放测试
add_executable(test_fastorderbook
    test/test_fastorderbook.cpp
    src/FastOrderBook.cpp
)
target_include_directories(test_fastorderbook PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_fastorderbook
    Threads::Threads
    quill::quill
)

# 策略测试程序（暂时注释掉，因为依赖的源文件已删除）
# add_executable(test_strategy_simple
#     test/test_strategy_simple.cpp
#     src/FastOrderBook.cpp
#     src/Order.cpp
#     src/Trade.cpp
#     src/PriceLevel.cpp
#     src/OrderBook.cpp
# )
# target_include_directories(test_strategy_simple PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(test_strategy_simple
#     Threads::Threads
#     quill::quill
# )
# target_compile_definitions(test_strategy_simple PRIVATE USE_QUILL_LOGGER)

# 调试测试程序（暂时注释掉，因为依赖的源文件已删除）
# add_executable(test_strategy_debug
#     test/test_strategy_debug.cpp
#     src/FastOrderBook.cpp
#     src/Order.cpp
#     src/Trade.cpp
#     src/PriceLevel.cpp
#     src/OrderBook.cpp
# )
# target_include_directories(test_strategy_debug PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(test_strategy_debug
#     Threads::Threads
#     quill::quill
# )
# target_compile_definitions(test_strategy_debug PRIVATE USE_QUILL_LOGGER)

# 消息顺序追踪测试（暂时注释掉，因为依赖的源文件已删除）
# add_executable(test_transaction_order
#     test/test_transaction_order.cpp
#     src/FastOrderBook.cpp
#     src/Order.cpp
#     src/Trade.cpp
#     src/PriceLevel.cpp
#     src/OrderBook.cpp
# )
# target_include_directories(test_transaction_order PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(test_transaction_order
#     Threads::Threads
#     quill::quill
# )
# target_compile_definitions(test_transaction_order PRIVATE USE_QUILL_LOGGER)

# 数据顺序检查测试（无队列）
add_executable(test_data_order
    test/test_data_order.cpp
)
target_include_directories(test_data_order PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_data_order
    Threads::Threads
)

# 价格档位挂单量监控策略测试
add_executable(test_price_level_strategy
    test/test_price_level_strategy.cpp
    src/FastOrderBook.cpp
)
target_include_directories(test_price_level_strategy PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_price_level_strategy
    Threads::Threads
    quill::quill
)
target_compile_definitions(test_price_level_strategy PRIVATE USE_QUILL_LOGGER)