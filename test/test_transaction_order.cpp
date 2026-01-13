#include <iostream>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <thread>
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "logger.h"

// 追踪ORDER和TRANSACTION的处理顺序
std::unordered_map<uint64_t, uint64_t> order_sequence;  // orderno -> 处理序号
std::unordered_map<uint64_t, uint64_t> txn_order_refs;  // tradeindex -> orderno引用
std::atomic<uint64_t> msg_sequence{0};
std::mutex tracking_mutex;

class OrderTrackingStrategy : public Strategy {
private:
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> txn_count_{0};
    std::atomic<uint64_t> txn_missing_order_{0};  // 成交找不到订单

public:
    OrderTrackingStrategy(const std::string& n) {
        name = n;
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;
        uint64_t seq = msg_sequence.fetch_add(1);

        std::lock_guard<std::mutex> lock(tracking_mutex);
        order_sequence[order.orderno] = seq;
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        txn_count_++;
        uint64_t seq = msg_sequence.fetch_add(1);

        std::lock_guard<std::mutex> lock(tracking_mutex);

        // 检查成交对应的订单是否已经被处理过
        TradeBSFlag bsflag = static_cast<TradeBSFlag>(txn.tradebsflag);
        uint64_t buy_order = txn.tradebuyno;
        uint64_t sell_order = txn.tradesellno;

        bool buy_exists = order_sequence.count(buy_order) > 0;
        bool sell_exists = order_sequence.count(sell_order) > 0;

        // 上海市场：买方主动则只需要卖单存在，卖方主动则只需要买单存在
        bool is_shanghai = true;  // 603122.SH是上海股票

        if (is_shanghai) {
            if (bsflag == TradeBSFlag::Buy) {
                // 买方主动，需要卖单存在
                if (!sell_exists) {
                    txn_missing_order_++;
                    if (txn_missing_order_ <= 10) {  // 只打印前10个
                        std::cout << "❌ TXN #" << txn_count_ << " (seq=" << seq
                                  << ") 找不到卖单 " << sell_order << "\n";
                    }
                }
            } else if (bsflag == TradeBSFlag::Sell) {
                // 卖方主动，需要买单存在
                if (!buy_exists) {
                    txn_missing_order_++;
                    if (txn_missing_order_ <= 10) {
                        std::cout << "❌ TXN #" << txn_count_ << " (seq=" << seq
                                  << ") 找不到买单 " << buy_order << "\n";
                    }
                }
            }
        }
    }

    void print_stats() {
        std::cout << "\n=== 顺序追踪统计 ===\n";
        std::cout << "ORDER消息数: " << order_count_ << "\n";
        std::cout << "TXN消息数: " << txn_count_ << "\n";
        std::cout << "TXN找不到订单次数: " << txn_missing_order_ << "\n";

        if (txn_missing_order_ > 0) {
            std::cout << "\n❌ 检测到消息乱序！成交在订单之前处理了 "
                      << txn_missing_order_ << " 次\n";
        } else {
            std::cout << "\n✅ 所有成交都能找到对应订单\n";
        }
    }
};

int main() {
    hft::logger::LogConfig log_config;
    log_config.console_output = false;
    hft::logger::init(log_config);

    std::string symbol = "603122.SH";
    std::string order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.txt";
    std::string txn_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt";

    std::cout << "=== 消息顺序追踪测试 ===\n";
    std::cout << "股票: " << symbol << "\n\n";

    StrategyEngine engine;
    OrderTrackingStrategy strategy("OrderTracker");

    engine.register_strategy(symbol, &strategy);
    engine.start();

    BacktestAdapter adapter(&engine, 1);

    if (!adapter.load_order_file(order_file) || !adapter.load_transaction_file(txn_file)) {
        std::cerr << "加载数据失败\n";
        engine.stop();
        return 1;
    }

    std::cout << "总事件数: " << adapter.event_count() << "\n\n";
    std::cout << "开始回放...\n";

    adapter.replay();

    std::cout << "等待处理完成...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    strategy.print_stats();

    engine.stop();
    hft::logger::shutdown();

    return 0;
}
