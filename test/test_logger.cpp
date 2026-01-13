/**
 * @file test_logger.cpp
 * @brief 日志模块测试程序
 */

#include "logger.h"
#include <thread>
#include <chrono>

int main() {
    // 初始化日志
    hft::logger::LogConfig config;
    config.log_dir = "logs";
    config.log_file = "test.log";
    config.console_output = true;
    config.use_rdtsc = true;

    auto* logger = hft::logger::init(config);

    // 测试各个日志等级
    LOG_DEBUG(logger, "This is a DEBUG message, value={}", 42);
    LOG_INFO(logger, "This is an INFO message");
    LOG_WARNING(logger, "This is a WARNING message");
    LOG_ERROR(logger, "This is an ERROR message");

    // 测试模块日志
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Engine starting...");
    LOG_MODULE_INFO(logger, MOD_ORDERBOOK, "OrderBook initialized, symbol={}", "000001.SZ");
    LOG_MODULE_WARNING(logger, MOD_STRATEGY, "Latency spike detected, latency_ns={}", 1500);
    LOG_MODULE_ERROR(logger, MOD_GATEWAY, "Connection failed, reason={}", "timeout");

    // ============ 测试业务日志 ============
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Initializing business logger...");

    // 初始化业务日志（输出到独立文件，同时输出到控制台方便测试）
    hft::logger::BizLogConfig biz_config;
    biz_config.log_dir = "logs";
    biz_config.log_file = "business.log";
    biz_config.console_output = true;  // 测试时输出到控制台
    auto* biz_logger = hft::logger::init_biz(biz_config);

    // 模拟业务流程
    LOG_BIZ(biz_logger, BIZ_SESS, "登录成功 account={} broker={}", "12345", "中信证券");
    LOG_BIZ(biz_logger, BIZ_ACCT, "资金查询 available={} frozen={}", 1000000.50, 50000.00);
    LOG_BIZ(biz_logger, BIZ_ORDR, "下单 id={} symbol={} side={} price={} qty={}",
            1001, "000001.SZ", "BUY", 10.50, 1000);
    LOG_BIZ(biz_logger, BIZ_FILL, "成交 id={} filled_qty={} filled_price={} status={}",
            1001, 500, 10.50, "PARTIAL");
    LOG_BIZ(biz_logger, BIZ_FILL, "成交 id={} filled_qty={} filled_price={} status={}",
            1001, 500, 10.50, "FILLED");
    LOG_BIZ(biz_logger, BIZ_ORDR, "下单 id={} symbol={} side={} price={} qty={}",
            1002, "600000.SH", "SELL", 8.88, 2000);
    LOG_BIZ(biz_logger, BIZ_RJCT, "拒绝 id={} reason={}", 1002, "insufficient_position");
    LOG_BIZ(biz_logger, BIZ_CNCL, "撤单 id={} canceled_qty={}", 1003, 500);
    LOG_BIZ(biz_logger, BIZ_POSN, "持仓变化 symbol={} qty={} avg_cost={}",
            "000001.SZ", 1000, 10.50);
    LOG_BIZ(biz_logger, BIZ_SESS, "登出 account={}", "12345");

    // 测试高频日志（模拟HFT场景）
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting HFT simulation...");
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10000; ++i) {
        LOG_DEBUG(logger, "Tick received, seq={} price={} volume={}", i, 100.50 + i * 0.01, 1000);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    LOG_MODULE_INFO(logger, MOD_ENGINE,
        "HFT simulation complete: {} messages in {} ns, avg={} ns/msg",
        10000, duration.count(), duration.count() / 10000);

    // 确保所有日志刷盘
    hft::logger::flush();

    // 等待后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Test complete!");

    // 关闭日志
    hft::logger::shutdown();

    return 0;
}
