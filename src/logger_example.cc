/**
 * @file logger_example.cc
 * @brief HFT Logger 使用示例
 *
 * 编译: cmake --build build
 * 运行: ./build/logger_example
 */

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

#include "hft_logger.h"
#include "market_data_structs.h"

// 模拟行情处理函数
void process_market_data(const MDStockStruct& stock) {
    TRACK_LATENCY("process_market_data");

    LOG_DEBUGF(hft::LogCategory::MARKET_DATA,
        "Received tick: %s price=%ld vol=%ld",
        stock.htscsecurityid, stock.lastpx, stock.totalvolumetrade);

    // 模拟处理延迟
    std::this_thread::sleep_for(std::chrono::microseconds(10));
}

// 模拟订单处理
void send_order(const char* symbol, int64_t price, int64_t qty, bool is_buy) {
    TRACK_LATENCY("send_order");

    LOG_INFOF(hft::LogCategory::ORDER,
        "Sending %s order: %s @ %ld x %ld",
        is_buy ? "BUY" : "SELL", symbol, price, qty);

    // 模拟网络延迟
    std::this_thread::sleep_for(std::chrono::microseconds(50));

    LOG_INFOF(hft::LogCategory::ORDER,
        "Order sent successfully: %s", symbol);
}

// 模拟风控检查
bool check_risk(const char* symbol, int64_t qty) {
    if (qty > 10000) {
        LOG_WARNINGF(hft::LogCategory::RISK,
            "Order quantity %ld exceeds limit for %s", qty, symbol);
        return false;
    }
    return true;
}

// 模拟策略执行
void strategy_worker(int strategy_id) {
    LOG_INFOF(hft::LogCategory::STRATEGY,
        "Strategy %d started", strategy_id);

    for (int i = 0; i < 10; ++i) {
        // 模拟接收行情
        MDStockStruct stock{};
        std::snprintf(stock.htscsecurityid, sizeof(stock.htscsecurityid),
            "60000%d.SH", strategy_id);
        stock.lastpx = 1000000 + i * 100;
        stock.totalvolumetrade = 100000 + i;

        process_market_data(stock);

        // 模拟交易决策
        if (i % 3 == 0) {
            if (check_risk(stock.htscsecurityid, 500)) {
                send_order(stock.htscsecurityid, stock.lastpx, 500, true);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LOG_INFOF(hft::LogCategory::STRATEGY,
        "Strategy %d finished", strategy_id);
}

// 演示不同日志级别
void demonstrate_log_levels() {
    std::cout << "\n=== 演示不同日志级别 ===\n" << std::endl;

    LOG_DEBUG(hft::LogCategory::SYSTEM, "This is a DEBUG message - detailed internal state");
    LOG_INFO(hft::LogCategory::SYSTEM, "This is an INFO message - normal operation");
    LOG_WARNING(hft::LogCategory::SYSTEM, "This is a WARNING message - non-fatal issue");
    LOG_ERROR(hft::LogCategory::SYSTEM, "This is an ERROR message - requires attention");
}

// 演示格式化日志
void demonstrate_formatted_logging() {
    std::cout << "\n=== 演示格式化日志 ===\n" << std::endl;

    const char* symbol = "600000.SH";
    int64_t price = 1234567;  // 123.4567
    int64_t quantity = 1000;
    double latency_us = 45.678;

    LOG_INFOF(hft::LogCategory::TRADE,
        "Trade executed: %s @ %ld x %ld, latency=%.2fus",
        symbol, price, quantity, latency_us);

    LOG_DEBUGF(hft::LogCategory::NETWORK,
        "Connection established: host=%s port=%d", "192.168.1.100", 8080);
}

// 演示多线程日志
void demonstrate_multithreaded_logging() {
    std::cout << "\n=== 演示多线程日志 ===\n" << std::endl;

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(strategy_worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }
}

// 演示延迟追踪
void demonstrate_latency_tracking() {
    std::cout << "\n=== 演示延迟追踪 ===\n" << std::endl;

    {
        TRACK_LATENCY("fast_operation");
        // 快速操作
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    {
        TRACK_LATENCY("slow_operation");
        // 慢操作会触发 WARNING
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    // 初始化日志器
    hft::AsyncLogger::Config config;
    config.min_level = hft::LogLevel::DEBUG;
    config.enable_console = true;
    config.log_file_path = "trading.log";
    config.flush_interval_ms = 50;

    hft::AsyncLogger::instance().init(config);

    LOG_INFO(hft::LogCategory::SYSTEM, "=== HFT Logger Example Started ===");

    // 运行各种演示
    demonstrate_log_levels();
    demonstrate_formatted_logging();
    demonstrate_latency_tracking();
    demonstrate_multithreaded_logging();

    // 演示运行时级别调整
    std::cout << "\n=== 调整日志级别为 INFO ===\n" << std::endl;
    hft::AsyncLogger::instance().set_level(hft::LogLevel::INFO);

    LOG_DEBUG(hft::LogCategory::SYSTEM, "This DEBUG message will NOT appear");
    LOG_INFO(hft::LogCategory::SYSTEM, "This INFO message WILL appear");

    LOG_INFO(hft::LogCategory::SYSTEM, "=== HFT Logger Example Completed ===");

    // 关闭日志器（确保所有消息都被写入）
    hft::AsyncLogger::instance().shutdown();

    std::cout << "\n日志已保存到 trading.log 文件" << std::endl;

    return 0;
}
