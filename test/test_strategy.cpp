#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// 引入测试策略
#include "src/TestStrategy.h"
#include "backtest_adapter.h"
#include "logger.h"

void print_usage(const char* prog_name) {
    std::cout << "用法: " << prog_name << " [选项]" << std::endl;
    std::cout << "\n选项:" << std::endl;
    std::cout << "  --mode <test|perf>     测试模式 (默认: test)" << std::endl;
    std::cout << "  --symbol <code>        股票代码 (默认: 002603.SZ)" << std::endl;
    std::cout << "  --order <file>         委托数据文件路径" << std::endl;
    std::cout << "  --transaction <file>   成交数据文件路径" << std::endl;
    std::cout << "  --sample <N>           采样间隔 (默认: 10000)" << std::endl;
    std::cout << "\n示例:" << std::endl;
    std::cout << "  " << prog_name << " --mode test --symbol 002603.SZ" << std::endl;
    std::cout << "  " << prog_name << " --mode perf --sample 100000" << std::endl;
}

int main(int argc, char* argv[]) {
    // 默认参数
    std::string mode = "test";
    std::string symbol = "002603.SZ";
    std::string order_file = "test_data/MD_ORDER_StockType_002603.SZ.csv";
    std::string txn_file = "test_data/MD_TRANSACTION_StockType_002603.SZ.csv";
    uint64_t sample_interval = 10000;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (arg == "--order" && i + 1 < argc) {
            order_file = argv[++i];
        } else if (arg == "--transaction" && i + 1 < argc) {
            txn_file = argv[++i];
        } else if (arg == "--sample" && i + 1 < argc) {
            sample_interval = std::stoull(argv[++i]);
        } else {
            std::cerr << "未知参数: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // 初始化日志系统
    hft::logger::LogConfig log_config;
    log_config.log_dir = "logs";
    log_config.log_file = "test_strategy.log";
    log_config.console_output = true;
    log_config.use_rdtsc = true;
    auto* logger = hft::logger::init(log_config);

    std::cout << "=== 策略测试程序 ===" << std::endl;
    std::cout << "模式: " << mode << std::endl;
    std::cout << "股票: " << symbol << std::endl;
    std::cout << "委托文件: " << order_file << std::endl;
    std::cout << "成交文件: " << txn_file << std::endl;
    std::cout << "采样间隔: " << sample_interval << std::endl;
    std::cout << std::endl;

    // 创建策略引擎
    StrategyEngine engine;

    // 根据模式创建不同的策略
    Strategy* strategy = nullptr;
    TestStrategy* test_strategy = nullptr;
    PerformanceStrategy* perf_strategy = nullptr;

    if (mode == "test") {
        test_strategy = new TestStrategy("TestStrategy", sample_interval);
        strategy = test_strategy;
        LOG_MODULE_INFO(logger, MOD_ENGINE, "使用测试策略 (TestStrategy)");
    } else if (mode == "perf") {
        perf_strategy = new PerformanceStrategy("PerfStrategy");
        strategy = perf_strategy;
        LOG_MODULE_INFO(logger, MOD_ENGINE, "使用性能测试策略 (PerformanceStrategy)");
    } else {
        std::cerr << "未知模式: " << mode << std::endl;
        hft::logger::shutdown();
        return 1;
    }

    // 注册策略
    engine.register_strategy(symbol, strategy);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "启动策略引擎...");
    engine.start();

    // 创建回测适配器
    BacktestAdapter adapter(&engine, StrategyEngine::SHARD_COUNT);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "加载历史数据...");
    if (!adapter.load_order_file(order_file)) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "加载委托文件失败: {}", order_file);
        engine.stop();
        delete strategy;
        hft::logger::shutdown();
        return 1;
    }

    if (!adapter.load_transaction_file(txn_file)) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "加载成交文件失败: {}", txn_file);
        engine.stop();
        delete strategy;
        hft::logger::shutdown();
        return 1;
    }

    LOG_MODULE_INFO(logger, MOD_ENGINE, "数据加载完成，总事件数: {}", adapter.event_count());

    // 记录开始时间
    auto start_time = std::chrono::steady_clock::now();

    LOG_MODULE_INFO(logger, MOD_ENGINE, "开始回放数据...");
    adapter.replay();

    // 等待队列排空
    std::cout << "\n等待处理完成..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 记录结束时间
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "回放完成，耗时: {} ms", duration.count());

    // 停止引擎
    LOG_MODULE_INFO(logger, MOD_ENGINE, "停止策略引擎...");
    engine.stop();

    // 打印统计信息
    std::cout << "\n" << std::string(50, '=') << std::endl;
    if (mode == "test" && test_strategy) {
        // 注意：这里无法获取 FastOrderBook，因为它在 worker 线程中
        // 可以考虑添加回调机制获取最终状态
        std::cout << "测试模式下，详细统计已在运行过程中输出" << std::endl;
    } else if (mode == "perf" && perf_strategy) {
        perf_strategy->print_stats();
    }
    std::cout << std::string(50, '=') << std::endl;

    // 清理
    delete strategy;
    hft::logger::shutdown();

    std::cout << "\n测试完成！" << std::endl;
    return 0;
}
