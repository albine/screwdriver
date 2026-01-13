#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

// 引入核心头文件
#include "strategy_engine.h"
#include "backtest_adapter.h"
#include "logger.h"  // 使用 quill 日志库

// ==========================================
// 简单测试策略
// ==========================================
class SimpleTestStrategy : public Strategy {
private:
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    std::chrono::steady_clock::time_point start_time_;
    uint64_t sample_interval_;
    uint64_t expected_order_count_;  // 预期的总订单数（用于判断是否到最后）

    // 保存最终盘口状态
    uint64_t last_applseqnum_{0};
    std::vector<std::pair<uint32_t, uint64_t>> final_bids_;
    std::vector<std::pair<uint32_t, uint64_t>> final_asks_;

public:
    SimpleTestStrategy(const std::string& n, uint64_t interval = 10000, uint64_t expected = 0)
        : sample_interval_(interval), expected_order_count_(expected) {
        name = n;
        start_time_ = std::chrono::steady_clock::now();
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;

        // 定期打印状态并显示十档盘口
        if (order_count_ % sample_interval_ == 0) {
            print_book_snapshot(order.applseqnum, book);
        }

        // 保存最后的盘口状态
        last_applseqnum_ = order.applseqnum;
        final_bids_ = book.get_bid_levels(10);
        final_asks_ = book.get_ask_levels(10);
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;

        // 保存最后的盘口状态（成交也会改变盘口）
        last_applseqnum_ = txn.applseqnum;
        final_bids_ = book.get_bid_levels(10);
        final_asks_ = book.get_ask_levels(10);

        // 定期验证
        if (transaction_count_ % (sample_interval_ * 10) == 0) {
            auto best_bid = book.get_best_bid();
            auto best_ask = book.get_best_ask();

            if (best_bid && best_ask && *best_bid >= *best_ask) {
                std::cerr << "❌ ERROR: Invalid book at Txn #" << transaction_count_
                          << " BestBid: " << (*best_bid / 10000.0)
                          << " >= BestAsk: " << (*best_ask / 10000.0) << std::endl;
            } else {
                std::cout << "✅ Validation passed at Txn #" << transaction_count_ << std::endl;
            }
        }
    }

    void print_stats() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_);

        std::cout << "\n=== " << name << " 统计报告 ===" << std::endl;
        std::cout << "委托消息: " << order_count_ << std::endl;
        std::cout << "成交消息: " << transaction_count_ << std::endl;
        std::cout << "总消息数: " << (order_count_ + transaction_count_) << std::endl;
        std::cout << "运行时间: " << duration.count() << " ms" << std::endl;

        if (duration.count() > 0) {
            uint64_t total = order_count_ + transaction_count_;
            double throughput = (total * 1000.0) / duration.count();
            std::cout << "吞吐量: " << std::fixed << std::setprecision(0)
                      << throughput << " msg/s" << std::endl;
        }
        std::cout << std::endl;
    }

    void set_expected_order_count(uint64_t count) {
        expected_order_count_ = count;
    }

    // 打印收盘后的最终十档盘口
    void print_final_book() {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "收盘后最终订单簿 (ApplSeqNum: " << last_applseqnum_ << ")" << std::endl;
        std::cout << std::string(60, '=') << std::endl;

        // 打印卖盘（倒序，价格从高到低）
        std::cout << "\n--- 卖盘 (Ask) ---" << std::endl;
        if (final_asks_.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            for (auto it = final_asks_.rbegin(); it != final_asks_.rend(); ++it) {
                std::cout << "  卖" << std::setw(2) << std::distance(final_asks_.rbegin(), it) + 1
                          << ": " << std::fixed << std::setprecision(4)
                          << (it->first / 10000.0) << " 元  "
                          << std::setw(10) << it->second << std::endl;
            }
        }

        std::cout << std::string(40, '-') << std::endl;

        // 打印买盘
        std::cout << "--- 买盘 (Bid) ---" << std::endl;
        if (final_bids_.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            int level = 1;
            for (const auto& [price, volume] : final_bids_) {
                std::cout << "  买" << std::setw(2) << level++
                          << ": " << std::fixed << std::setprecision(4)
                          << (price / 10000.0) << " 元  "
                          << std::setw(10) << volume << std::endl;
            }
        }

        // 计算并显示买卖价差
        if (!final_bids_.empty() && !final_asks_.empty()) {
            double spread = (final_asks_[0].first - final_bids_[0].first) / 10000.0;
            double spread_bps = (spread / (final_bids_[0].first / 10000.0)) * 10000;
            std::cout << "\n价差: " << std::fixed << std::setprecision(4)
                      << spread << " 元 (" << std::setprecision(2)
                      << spread_bps << " bps)" << std::endl;

            // 检查合法性
            if (final_bids_[0].first >= final_asks_[0].first) {
                std::cout << "⚠️  WARNING: 买一价 >= 卖一价！订单簿状态异常！" << std::endl;
            }
        }

        std::cout << std::string(60, '=') << std::endl << std::endl;
    }

private:
    // 打印订单簿快照（包含十档盘口）
    void print_book_snapshot(uint64_t seq_num, const FastOrderBook& book) {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "订单簿快照 #" << order_count_ << " (ApplSeqNum: " << seq_num << ")" << std::endl;
        std::cout << std::string(60, '=') << std::endl;

        auto bids = book.get_bid_levels(10);
        auto asks = book.get_ask_levels(10);

        // 打印卖盘（倒序，价格从高到低）
        std::cout << "\n--- 卖盘 (Ask) ---" << std::endl;
        if (asks.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
                std::cout << "  卖" << std::setw(2) << std::distance(asks.rbegin(), it) + 1
                          << ": " << std::fixed << std::setprecision(4)
                          << (it->first / 10000.0) << " 元  "
                          << std::setw(10) << it->second << std::endl;
            }
        }

        std::cout << std::string(40, '-') << std::endl;

        // 打印买盘
        std::cout << "--- 买盘 (Bid) ---" << std::endl;
        if (bids.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            int level = 1;
            for (const auto& [price, volume] : bids) {
                std::cout << "  买" << std::setw(2) << level++
                          << ": " << std::fixed << std::setprecision(4)
                          << (price / 10000.0) << " 元  "
                          << std::setw(10) << volume << std::endl;
            }
        }

        // 计算并显示买卖价差
        if (!bids.empty() && !asks.empty()) {
            double spread = (asks[0].first - bids[0].first) / 10000.0;
            double spread_bps = (spread / (bids[0].first / 10000.0)) * 10000;
            std::cout << "\n价差: " << std::fixed << std::setprecision(4)
                      << spread << " 元 (" << std::setprecision(2)
                      << spread_bps << " bps)" << std::endl;

            // 检查合法性
            if (bids[0].first >= asks[0].first) {
                std::cout << "⚠️  WARNING: 买一价 >= 卖一价！订单簿状态异常！" << std::endl;
            }
        }

        std::cout << std::string(60, '=') << std::endl << std::endl;
    }
};

int main(int argc, char* argv[]) {
    // 初始化日志系统
    hft::logger::LogConfig log_config;
    log_config.console_output = false;  // 禁用控制台输出，避免干扰测试结果
    hft::logger::init(log_config);

    // 默认参数
    std::string symbol = "603122.SH";
    std::string order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.txt";
    std::string txn_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt";
    uint64_t sample_interval = 10000;

    // 简单参数解析
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "用法: " << argv[0] << " [--sample N]" << std::endl;
            std::cout << "  --sample N    采样间隔 (默认: 10000)" << std::endl;
            return 0;
        } else if (arg == "--sample" && i + 1 < argc) {
            sample_interval = std::stoull(argv[++i]);
        }
    }

    std::cout << "=== 策略测试程序 ===" << std::endl;
    std::cout << "股票: " << symbol << std::endl;
    std::cout << "委托文件: " << order_file << std::endl;
    std::cout << "成交文件: " << txn_file << std::endl;
    std::cout << "采样间隔: " << sample_interval << std::endl;
    std::cout << std::endl;

    // 创建策略引擎
    StrategyEngine engine;
    SimpleTestStrategy strategy("SimpleTest", sample_interval);

    // 注册策略
    engine.register_strategy(symbol, &strategy);

    std::cout << "启动策略引擎..." << std::endl;
    engine.start();

    // 创建回测适配器（使用单线程回放以保证顺序）
    BacktestAdapter adapter(&engine, 1);

    std::cout << "加载历史数据..." << std::endl;
    if (!adapter.load_order_file(order_file)) {
        std::cerr << "❌ 加载委托文件失败: " << order_file << std::endl;
        engine.stop();
        return 1;
    }

    if (!adapter.load_transaction_file(txn_file)) {
        std::cerr << "❌ 加载成交文件失败: " << txn_file << std::endl;
        engine.stop();
        return 1;
    }

    std::cout << "数据加载完成，总事件数: " << adapter.event_count() << std::endl;

    // 记录开始时间
    auto start_time = std::chrono::steady_clock::now();

    std::cout << "开始回放数据..." << std::endl;
    adapter.replay();

    // 等待队列排空
    std::cout << "\n等待处理完成..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 记录结束时间
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "回放完成，耗时: " << duration.count() << " ms" << std::endl;

    // 停止引擎
    std::cout << "停止策略引擎..." << std::endl;
    engine.stop();

    // 打印统计信息
    std::cout << "\n" << std::string(50, '=') << std::endl;
    strategy.print_stats();
    std::cout << std::string(50, '=') << std::endl;

    // 打印收盘后的最终十档盘口
    strategy.print_final_book();

    std::cout << "\n✅ 测试完成！" << std::endl;

    // 关闭日志系统
    hft::logger::shutdown();

    return 0;
}
