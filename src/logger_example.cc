/**
 * @file logger_example.cc
 * @brief HFT Logger 使用示例 (基于 Quill)
 *
 * 编译:
 *   mkdir -p build && cd build
 *   cmake ..
 *   make logger_example
 *
 * 运行:
 *   ./build/logger_example
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

    LOG_DEBUG(hft::LogCategory::MARKET_DATA,
        "Received tick: {} price={} vol={}",
        stock.htscsecurityid, stock.lastpx, stock.totalvolumetrade);

    // 模拟处理延迟
    std::this_thread::sleep_for(std::chrono::microseconds(10));
}

// 模拟订单处理
void send_order(const char* symbol, int64_t price, int64_t qty, bool is_buy) {
    TRACK_LATENCY("send_order");

    LOG_INFO(hft::LogCategory::ORDER,
        "Sending {} order: {} @ {} x {}",
        is_buy ? "BUY" : "SELL", symbol, price, qty);

    // 模拟网络延迟
    std::this_thread::sleep_for(std::chrono::microseconds(50));

    LOG_INFO(hft::LogCategory::ORDER,
        "Order sent successfully: {}", symbol);
}

// 模拟风控检查
bool check_risk(const char* symbol, int64_t qty) {
    if (qty > 10000) {
        LOG_WARNING(hft::LogCategory::RISK,
            "Order quantity {} exceeds limit for {}", qty, symbol);
        return false;
    }
    return true;
}

// 模拟策略执行
void strategy_worker(int strategy_id) {
    LOG_INFO(hft::LogCategory::STRATEGY,
        "Strategy {} started", strategy_id);

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

    LOG_INFO(hft::LogCategory::STRATEGY,
        "Strategy {} finished", strategy_id);
}

// 演示不同日志级别
void demonstrate_log_levels() {
    std::cout << "\n=== 演示不同日志级别 ===\n" << std::endl;

    LOG_DEBUG(hft::LogCategory::SYSTEM, "This is a DEBUG message - detailed internal state");
    LOG_INFO(hft::LogCategory::SYSTEM, "This is an INFO message - normal operation");
    LOG_WARNING(hft::LogCategory::SYSTEM, "This is a WARNING message - non-fatal issue");
    LOG_ERROR(hft::LogCategory::SYSTEM, "This is an ERROR message - requires attention");
    LOG_CRITICAL(hft::LogCategory::SYSTEM, "This is a CRITICAL message - system failure");
}

// 演示格式化日志
void demonstrate_formatted_logging() {
    std::cout << "\n=== 演示格式化日志 (fmtlib 风格) ===\n" << std::endl;

    const char* symbol = "600000.SH";
    int64_t price = 1234567;  // 123.4567
    int64_t quantity = 1000;
    double latency_us = 45.678;

    // Quill 使用 fmtlib 风格的格式化
    LOG_INFO(hft::LogCategory::TRADE,
        "Trade executed: {} @ {} x {}, latency={:.2f}us",
        symbol, price, quantity, latency_us);

    LOG_DEBUG(hft::LogCategory::NETWORK,
        "Connection established: host={} port={}", "192.168.1.100", 8080);

    // 更多格式化示例
    LOG_INFO(hft::LogCategory::ORDER,
        "Order stats: success_rate={:.2%}, avg_latency={:.3f}ms",
        0.9876, 0.123);
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
        // 快速操作 - 会记录 DEBUG 级别
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    {
        TRACK_LATENCY("medium_operation");
        // 中等操作 - 会记录 INFO 级别
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    {
        TRACK_LATENCY("slow_operation");
        // 慢操作 - 会触发 WARNING
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    // 初始化 Quill 日志器
    hft::HftLogger::Config config;
    config.min_level = quill::LogLevel::Debug;
    config.enable_console = true;
    config.log_file_path = "trading.log";
    // 自定义格式 (可选)
    // config.log_pattern = "[%(time)] [%(log_level)] [%(logger)] %(message)";
    // config.timestamp_pattern = "%H:%M:%S.%Qns";

    hft::init_logger(config);

    LOG_INFO(hft::LogCategory::SYSTEM, "=== HFT Logger Example Started (Powered by Quill) ===");

    // 运行各种演示
    demonstrate_log_levels();
    demonstrate_formatted_logging();
    demonstrate_latency_tracking();
    demonstrate_multithreaded_logging();

    // 演示运行时级别调整
    std::cout << "\n=== 调整日志级别为 INFO ===\n" << std::endl;
    hft::HftLogger::instance().set_level(quill::LogLevel::Info);

    LOG_DEBUG(hft::LogCategory::SYSTEM, "This DEBUG message will NOT appear");
    LOG_INFO(hft::LogCategory::SYSTEM, "This INFO message WILL appear");

    LOG_INFO(hft::LogCategory::SYSTEM, "=== HFT Logger Example Completed ===");

    // 关闭日志器（确保所有消息都被写入）
    hft::shutdown_logger();

    std::cout << "\n日志已保存到 trading.log 文件" << std::endl;

    return 0;
}
