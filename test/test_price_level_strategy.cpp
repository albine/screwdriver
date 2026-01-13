#include "PriceLevelVolumeStrategy.h"
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <iostream>

int main(int argc, char* argv[]) {
    // 1. 初始化日志系统
    hft::logger::LogConfig log_config;
    log_config.log_file = "price_level_strategy.log";
    log_config.console_output = true;
    hft::logger::init(log_config);

    std::cout << "===========================================\n";
    std::cout << "Price Level Volume Strategy Test\n";
    std::cout << "===========================================\n";

    // 2. 创建策略引擎
    StrategyEngine engine;

    // 3. 创建策略实例
    // 注意：由于回测适配器不支持tick数据，我们需要手动设置昨收价和开盘价
    // 600759.SH 的真实数据（从tick数据提取）
    uint32_t preclose_price = 35100;  // 3.51元
    uint32_t open_price = 34800;      // 3.48元（开盘低于昨收）

    PriceLevelVolumeStrategy strategy("PriceLevel_600759.SH", preclose_price, open_price);

    // 4. 注册策略
    std::string symbol = "600759.SH";
    engine.register_strategy(symbol, &strategy);

    std::cout << "Strategy configured:\n";
    std::cout << "  Preclose Price: " << preclose_price << " (" << (preclose_price / 10000.0) << "元)\n";
    std::cout << "  Open Price: " << open_price << " (" << (open_price / 10000.0) << "元)\n";

    // 5. 启动引擎
    std::cout << "Starting strategy engine...\n";
    strategy.on_start();  // 手动调用on_start（因为StrategyEngine.start()不会调用）
    engine.start();

    // 6. 创建回测适配器
    BacktestAdapter adapter(&engine, StrategyEngine::SHARD_COUNT);

    // 7. 加载市场数据
    std::cout << "Loading market data...\n";

    // 检查命令行参数
    std::string order_file;
    std::string transaction_file;

    if (argc >= 3) {
        order_file = argv[1];
        transaction_file = argv[2];
    } else {
        // 使用默认文件
        order_file = "test_data/MD_ORDER_StockType_600759.SH.csv";
        transaction_file = "test_data/MD_TRANSACTION_StockType_600759.SH.csv";
    }

    std::cout << "Order file: " << order_file << "\n";
    std::cout << "Transaction file: " << transaction_file << "\n";

    if (!adapter.load_order_file(order_file)) {
        std::cerr << "Failed to load order file: " << order_file << "\n";
        return 1;
    }

    if (!adapter.load_transaction_file(transaction_file)) {
        std::cerr << "Failed to load transaction file: " << transaction_file << "\n";
        return 1;
    }

    // 8. 开始回放
    std::cout << "Starting replay...\n";
    adapter.replay();

    // 9. 等待处理完成
    std::cout << "Waiting for processing to complete...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 10. 停止引擎
    std::cout << "Stopping strategy engine...\n";
    engine.stop();
    strategy.on_stop();  // 手动调用on_stop

    std::cout << "===========================================\n";
    std::cout << "Test completed. Check logs for results.\n";
    std::cout << "===========================================\n";

    // 11. 关闭日志系统
    hft::logger::shutdown();

    return 0;
}
