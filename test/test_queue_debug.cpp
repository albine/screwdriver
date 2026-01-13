#include <iostream>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include "backtest_adapter.h"
#include "logger.h"

// 追踪入队次数
std::atomic<uint64_t> enqueue_order_count{0};
std::atomic<uint64_t> enqueue_txn_count{0};
std::unordered_set<uint64_t> enqueued_orders;
std::unordered_set<uint64_t> enqueued_txns;
std::mutex track_mutex;

// 自定义StrategyEngine用于调试
class DebugEngine {
public:
    void on_market_order(const MDOrderStruct& order) {
        enqueue_order_count++;

        std::lock_guard<std::mutex> lock(track_mutex);
        if (enqueued_orders.count(order.orderno)) {
            std::cout << "⚠️  重复入队ORDER: " << order.orderno << "\n";
        }
        enqueued_orders.insert(order.orderno);
    }

    void on_market_transaction(const MDTransactionStruct& txn) {
        enqueue_txn_count++;

        std::lock_guard<std::mutex> lock(track_mutex);
        if (enqueued_txns.count(txn.tradeindex)) {
            std::cout << "⚠️  重复入队TXN: " << txn.tradeindex << "\n";
        }
        enqueued_txns.insert(txn.tradeindex);
    }

    void start() {}
    void stop() {}
};

int main() {
    hft::logger::LogConfig log_config;
    log_config.console_output = false;
    hft::logger::init(log_config);

    std::string order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.txt";
    std::string txn_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt";

    std::cout << "=== 队列入队调试测试 ===\n\n";

    DebugEngine engine;
    BacktestAdapter adapter(&engine, 1);

    if (!adapter.load_order_file(order_file) || !adapter.load_transaction_file(txn_file)) {
        std::cerr << "加载数据失败\n";
        return 1;
    }

    std::cout << "开始回放...\n";
    adapter.replay();

    std::cout << "\n=== 统计结果 ===\n";
    std::cout << "入队ORDER次数: " << enqueue_order_count << "\n";
    std::cout << "唯一ORDER数: " << enqueued_orders.size() << "\n";
    std::cout << "入队TXN次数: " << enqueue_txn_count << "\n";
    std::cout << "唯一TXN数: " << enqueued_txns.size() << "\n";

    hft::logger::shutdown();
    return 0;
}
