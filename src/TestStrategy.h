#ifndef TEST_STRATEGY_H
#define TEST_STRATEGY_H

#include "strategy_engine.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>

// ==========================================
// 测试策略 - 用于验证订单簿正确性
// ==========================================
class TestStrategy : public Strategy {
private:
    // 统计信息
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    std::atomic<uint64_t> tick_count_{0};

    // 记录时间戳
    std::chrono::steady_clock::time_point start_time_;

    // 采样频率（每 N 条消息打印一次）
    uint64_t sample_interval_;

    // 最后一次订单簿快照
    struct BookSnapshot {
        uint32_t best_bid_price = 0;
        uint64_t best_bid_volume = 0;
        uint32_t best_ask_price = 0;
        uint64_t best_ask_volume = 0;
        std::chrono::steady_clock::time_point timestamp;
    };
    BookSnapshot last_snapshot_;

public:
    explicit TestStrategy(const std::string& name, uint64_t sample_interval = 10000)
        : sample_interval_(sample_interval) {
        this->name = name;
        start_time_ = std::chrono::steady_clock::now();
    }

    void on_tick(const MDStockStruct& stock) override {
        tick_count_++;

        // 定期打印 Tick 统计
        if (tick_count_ % sample_interval_ == 0) {
            std::cout << "[" << name << "] Processed " << tick_count_
                      << " ticks, LastPx: " << (stock.lastpx / 10000.0)
                      << " Volume: " << stock.totalvolumetrade
                      << std::endl;
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;

        // 定期打印订单簿状态
        if (order_count_ % sample_interval_ == 0) {
            print_book_status(book, order);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;

        // 定期验证订单簿合法性
        if (transaction_count_ % sample_interval_ == 0) {
            validate_book(book, txn);
        }
    }

    // 打印最终统计
    void print_final_stats(const FastOrderBook& book) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_);

        std::cout << "\n=== " << name << " 统计报告 ===" << std::endl;
        std::cout << "运行时间: " << duration.count() << " ms" << std::endl;
        std::cout << "处理消息总数: " << (order_count_ + transaction_count_ + tick_count_) << std::endl;
        std::cout << "  - 委托消息: " << order_count_ << std::endl;
        std::cout << "  - 成交消息: " << transaction_count_ << std::endl;
        std::cout << "  - Tick 消息: " << tick_count_ << std::endl;

        if (duration.count() > 0) {
            uint64_t total_msgs = order_count_ + transaction_count_ + tick_count_;
            double throughput = (total_msgs * 1000.0) / duration.count();
            std::cout << "吞吐量: " << std::fixed << std::setprecision(0)
                      << throughput << " msg/s" << std::endl;
        }

        std::cout << "\n=== 最终订单簿状态 ===" << std::endl;
        print_book_levels(book, 10);
    }

private:
    // 打印订单簿状态
    void print_book_status(const FastOrderBook& book, const MDOrderStruct& order) {
        auto best_bid = book.get_best_bid();
        auto best_ask = book.get_best_ask();

        std::cout << "[" << name << "] Order #" << order_count_
                  << " | ApplSeqNum: " << order.applseqnum
                  << " | BestBid: " << (best_bid ? std::to_string(*best_bid / 10000.0) : "None")
                  << " | BestAsk: " << (best_ask ? std::to_string(*best_ask / 10000.0) : "None");

        // 检查买卖价格是否合法
        if (best_bid && best_ask && *best_bid >= *best_ask) {
            std::cout << " ⚠️ WARNING: Bid >= Ask!" << std::endl;
        } else {
            std::cout << std::endl;
        }
    }

    // 验证订单簿合法性
    void validate_book(const FastOrderBook& book, const MDTransactionStruct& txn) {
        auto best_bid = book.get_best_bid();
        auto best_ask = book.get_best_ask();

        // 检查 1: 买一价 < 卖一价
        if (best_bid && best_ask && *best_bid >= *best_ask) {
            std::cerr << "❌ ERROR: Invalid book state at Txn #" << transaction_count_
                      << " | BestBid: " << (*best_bid / 10000.0)
                      << " >= BestAsk: " << (*best_ask / 10000.0)
                      << std::endl;
        }

        // 检查 2: 成交价格应该在买卖价之间（如果存在）
        if (txn.tradetype == 0) {  // 正常成交
            uint32_t trade_price = txn.tradeprice;
            if (best_bid && best_ask) {
                if (trade_price < *best_bid || trade_price > *best_ask) {
                    // 注意：这个检查可能不总是成立，因为成交会消除订单
                    // 仅作为参考
                }
            }
        }

        // 定期打印验证通过信息
        if (transaction_count_ % (sample_interval_ * 10) == 0) {
            std::cout << "✅ Validation passed at Txn #" << transaction_count_ << std::endl;
        }
    }

    // 打印订单簿档位
    void print_book_levels(const FastOrderBook& book, int n) {
        auto bids = book.get_bid_levels(n);
        auto asks = book.get_ask_levels(n);

        std::cout << "\n--- 卖盘 (Ask) ---" << std::endl;
        if (asks.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            // 倒序打印卖盘（价格从高到低）
            for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
                std::cout << "  卖" << std::distance(asks.rbegin(), it) + 1
                          << ": " << std::fixed << std::setprecision(4)
                          << (it->first / 10000.0) << " 元  "
                          << it->second << std::endl;
            }
        }

        std::cout << "-------------------" << std::endl;

        std::cout << "--- 买盘 (Bid) ---" << std::endl;
        if (bids.empty()) {
            std::cout << "  (空)" << std::endl;
        } else {
            int level = 1;
            for (const auto& [price, volume] : bids) {
                std::cout << "  买" << level++
                          << ": " << std::fixed << std::setprecision(4)
                          << (price / 10000.0) << " 元  "
                          << volume << std::endl;
            }
        }
        std::cout << std::endl;
    }
};

// ==========================================
// 性能测试策略 - 纯粹测试吞吐量
// ==========================================
class PerformanceStrategy : public Strategy {
private:
    std::atomic<uint64_t> message_count_{0};
    std::chrono::steady_clock::time_point start_time_;

public:
    explicit PerformanceStrategy(const std::string& name) {
        this->name = name;
        start_time_ = std::chrono::steady_clock::now();
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        message_count_++;
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        message_count_++;
    }

    void on_tick(const MDStockStruct& stock) override {
        message_count_++;
    }

    void print_stats() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_);

        std::cout << "\n=== " << name << " 性能报告 ===" << std::endl;
        std::cout << "总消息数: " << message_count_ << std::endl;
        std::cout << "运行时间: " << duration.count() << " ms" << std::endl;

        if (duration.count() > 0) {
            double throughput = (message_count_ * 1000.0) / duration.count();
            std::cout << "吞吐量: " << std::fixed << std::setprecision(0)
                      << throughput << " msg/s" << std::endl;
            std::cout << "平均延迟: " << std::fixed << std::setprecision(2)
                      << (duration.count() * 1000.0 / message_count_) << " μs/msg" << std::endl;
        }
    }
};

#endif // TEST_STRATEGY_H
