#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <set>
#include <unordered_map>
#include <mutex>

// 引入核心头文件
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "logger.h"

// ==========================================
// 调试策略 - 追踪订单处理
// ==========================================
class DebugStrategy : public Strategy {
private:
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    std::set<uint64_t> seen_order_nos_;
    std::set<uint64_t> seen_txn_indices_;
    std::unordered_map<const FastOrderBook*, uint64_t> book_instances_;
    std::set<std::thread::id> thread_ids_;
    std::unordered_map<std::thread::id, uint64_t> thread_msg_counts_;
    const FastOrderBook* last_book_{nullptr};
    std::mutex debug_mutex_;
    std::thread::id first_thread_id_;
    bool first_thread_set_{false};

public:
    DebugStrategy(const std::string& n) {
        name = n;
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;
        auto current_thread = std::this_thread::get_id();

        // 检查是否重复
        std::lock_guard<std::mutex> lock(debug_mutex_);
        if (seen_order_nos_.count(order.orderno)) {
            // 不打印重复警告，数据本身就有重复（撤单）
        }
        seen_order_nos_.insert(order.orderno);

        // 追踪OrderBook实例和线程ID
        book_instances_[&book]++;
        thread_ids_.insert(current_thread);
        thread_msg_counts_[current_thread]++;
        last_book_ = &book;

        // 记录第一个处理线程
        if (!first_thread_set_) {
            first_thread_id_ = current_thread;
            first_thread_set_ = true;
        }

        // 检测线程切换
        if (current_thread != first_thread_id_) {
            std::cout << "❌ 线程切换！ORDER #" << order_count_
                      << " 被不同线程处理！\n";
        }

        // 在关键区间更频繁地监控
        bool should_print = (order_count_ % 10000 == 0) ||
                           (order_count_ >= 24000 && order_count_ <= 26000 && order_count_ % 100 == 0);

        if (should_print) {
            auto bids = book.get_bid_levels(1);
            uint64_t bid1_vol = bids.empty() ? 0 : bids[0].second;

            std::cout << "#" << order_count_ << " 买一量: " << bid1_vol
                      << " [OrderNO=" << order.orderno
                      << " Type=" << (int)order.ordertype << "]";

            // 检测异常增长
            static uint64_t last_vol = 0;
            if (bid1_vol > last_vol * 2 && last_vol > 1000) {
                std::cout << " ⚠️  异常增长！上次: " << last_vol;
            }
            last_vol = bid1_vol;

            std::cout << std::endl;
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;
        auto current_thread = std::this_thread::get_id();

        // 检查是否重复
        std::lock_guard<std::mutex> lock(debug_mutex_);
        if (seen_txn_indices_.count(txn.tradeindex)) {
            std::cout << "⚠️  重复TXN: tradeindex=" << txn.tradeindex
                      << " ApplSeqNum=" << txn.applseqnum << std::endl;
        }
        seen_txn_indices_.insert(txn.tradeindex);

        // 追踪线程
        thread_ids_.insert(current_thread);
        thread_msg_counts_[current_thread]++;

        // 检测线程切换
        if (first_thread_set_ && current_thread != first_thread_id_) {
            std::cout << "❌ 线程切换！TXN #" << transaction_count_
                      << " 被不同线程处理！\n";
        }
    }

    void print_stats() {
        std::cout << "\n=== Debug统计 ===\n";
        std::cout << "总ORDER调用次数: " << order_count_ << "\n";
        std::cout << "唯一ORDER数量: " << seen_order_nos_.size() << "\n";
        std::cout << "总TXN调用次数: " << transaction_count_ << "\n";
        std::cout << "唯一TXN数量: " << seen_txn_indices_.size() << "\n";
        std::cout << "\n=== OrderBook实例统计 ===\n";
        std::cout << "OrderBook实例数: " << book_instances_.size() << "\n";
        std::cout << "处理线程数: " << thread_ids_.size() << "\n";

        for (const auto& [book_ptr, count] : book_instances_) {
            std::cout << "  实例 " << book_ptr << ": " << count << " 次调用\n";
        }

        std::cout << "\n=== 线程处理统计 ===\n";
        for (const auto& [tid, count] : thread_msg_counts_) {
            std::cout << "  线程 " << tid << ": " << count << " 条消息\n";
        }

        if (order_count_ > seen_order_nos_.size()) {
            std::cout << "\n✅ ORDER数据包含撤单记录（正常）\n";
            std::cout << "   撤单数量: " << (order_count_ - seen_order_nos_.size()) << "\n";
        }

        if (transaction_count_ > seen_txn_indices_.size()) {
            std::cout << "❌ 检测到TXN重复处理！\n";
            std::cout << "   重复次数: " << (transaction_count_ - seen_txn_indices_.size()) << "\n";
        }

        // 打印最终OrderBook状态
        if (last_book_) {
            std::cout << "\n=== 最终OrderBook状态 ===\n";
            auto bids = last_book_->get_bid_levels(10);
            auto asks = last_book_->get_ask_levels(10);

            std::cout << "卖盘档位数: " << asks.size() << "\n";
            std::cout << "买盘档位数: " << bids.size() << "\n";

            if (!bids.empty()) {
                std::cout << "\n买一: " << (bids[0].first / 10000.0)
                          << " 元, 量: " << bids[0].second << "\n";
            }

            if (!asks.empty()) {
                std::cout << "卖一: " << (asks[0].first / 10000.0)
                          << " 元, 量: " << asks[0].second << "\n";
            }
        }

        std::cout << std::endl;
    }
};

int main() {
    // 初始化日志系统
    hft::logger::LogConfig log_config;
    log_config.console_output = false;
    hft::logger::init(log_config);

    std::string symbol = "603122.SH";
    std::string order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.txt";
    std::string txn_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt";

    std::cout << "=== 调试测试程序 ===\n";
    std::cout << "股票: " << symbol << "\n\n";

    // 创建策略引擎
    StrategyEngine engine;
    DebugStrategy strategy("Debug");

    // 注册策略
    engine.register_strategy(symbol, &strategy);

    std::cout << "启动策略引擎...\n";
    engine.start();

    // 创建回测适配器（单线程回放）
    BacktestAdapter adapter(&engine, 1);

    std::cout << "加载历史数据...\n";
    if (!adapter.load_order_file(order_file)) {
        std::cerr << "❌ 加载委托文件失败\n";
        engine.stop();
        return 1;
    }

    if (!adapter.load_transaction_file(txn_file)) {
        std::cerr << "❌ 加载成交文件失败\n";
        engine.stop();
        return 1;
    }

    std::cout << "数据加载完成，总事件数: " << adapter.event_count() << "\n\n";

    std::cout << "开始回放数据...\n";
    adapter.replay();

    std::cout << "等待处理完成...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 在engine.stop()之前打印统计（避免OrderBook被销毁）
    strategy.print_stats();

    std::cout << "\n=== StrategyEngine入队统计 ===\n";
    std::cout << "入队ORDER次数: " << engine.enqueue_order_count_ << "\n";
    std::cout << "入队TXN次数: " << engine.enqueue_txn_count_ << "\n";

    std::cout << "\n停止策略引擎...\n";
    engine.stop();

    hft::logger::shutdown();

    return 0;
}
