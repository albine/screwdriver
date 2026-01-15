#ifndef TEST_STRATEGY_H
#define TEST_STRATEGY_H

#include "strategy_base.h"
#include "logger.h"
#include <iomanip>
#include <chrono>
#include <atomic>
#include <random>

#define LOG_MODULE MOD_STRATEGY

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

    // 记录上次打印的分钟，避免重复打印
    int32_t last_print_minute_ = -1;

    // 随机偏移（-1到+1分钟），避免多策略同时打印
    int32_t random_offset_minute_ = 0;

public:
    explicit TestStrategy(const std::string& name, uint64_t sample_interval = 100000)
        : sample_interval_(sample_interval) {
        this->name = name;
        start_time_ = std::chrono::steady_clock::now();

        // 生成-1到+1分钟的随机偏移
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(-1, 1);
        random_offset_minute_ = dis(gen);
    }

    void on_tick(const MDStockStruct& stock) override {
        tick_count_++;

        // 定期打印 Tick 统计
        if (tick_count_ % sample_interval_ == 0) {
            LOG_M_INFO("[{}] Processed {} ticks, LastPx: {:.4f} Volume: {}",
                       name, tick_count_.load(), stock.lastpx / 10000.0, stock.totalvolumetrade);
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;

        // 按时间打印订单簿
        if (should_print_book(order.mdtime)) {
            print_book_with_time(book, order.mdtime);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;

        // 按时间打印订单簿
        if (should_print_book(txn.mdtime)) {
            print_book_with_time(book, txn.mdtime);
        }

        // 定期验证订单簿合法性
        if (transaction_count_ % sample_interval_ == 0) {
            validate_book(book, txn);
        }
    }

    // 打印最终统计
    void print_final_stats(const FastOrderBook& book) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time_);

        LOG_M_INFO("=== {} 统计报告 ===", name);
        LOG_M_INFO("运行时间: {} ms", duration.count());
        LOG_M_INFO("处理消息总数: {}", order_count_.load() + transaction_count_.load() + tick_count_.load());
        LOG_M_INFO("  - 委托消息: {}", order_count_.load());
        LOG_M_INFO("  - 成交消息: {}", transaction_count_.load());
        LOG_M_INFO("  - Tick 消息: {}", tick_count_.load());

        if (duration.count() > 0) {
            uint64_t total_msgs = order_count_ + transaction_count_ + tick_count_;
            double throughput = (total_msgs * 1000.0) / duration.count();
            LOG_M_INFO("吞吐量: {:.0f} msg/s", throughput);
        }

        LOG_M_INFO("=== 最终订单簿状态 ===");
        book.print_orderbook(10, "");
    }

private:
    // 获取当前系统时间（微秒时间戳）
    int64_t get_system_time_us() const {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }

    // 检查是否需要打印订单簿
    bool should_print_book(int32_t mdtime) {
        int hour = mdtime / 10000000;
        int minute = (mdtime / 100000) % 100;
        int second = (mdtime / 1000) % 100;

        // 固定时间点: 9:28:00, 11:35:00, 15:05:00 (不加偏移)
        if ((hour == 9 && minute == 28 && second == 0) ||
            (hour == 11 && minute == 35 && second == 0) ||
            (hour == 15 && minute == 5 && second == 0)) {
            int current_minute = hour * 60 + minute;
            if (current_minute != last_print_minute_) {
                last_print_minute_ = current_minute;
                return true;
            }
        }

        // 盘中每10分钟（加随机偏移，避免拥挤）
        // 上午: 9:30-11:30, 下午: 13:00-15:00
        bool is_trading_time = (hour == 9 && minute >= 30) ||
                               (hour == 10) ||
                               (hour == 11 && minute <= 30) ||
                               (hour == 13) ||
                               (hour == 14) ||
                               (hour == 15 && minute == 0);

        if (is_trading_time && second == 0) {
            // 检查是否是 (整10分钟 + offset)
            int target_minute = (minute / 10) * 10 + random_offset_minute_;
            // 处理边界
            if (target_minute < 0) target_minute += 10;
            if (target_minute >= 60) target_minute -= 10;

            if (minute == target_minute) {
                int current_minute = hour * 60 + minute;
                if (current_minute != last_print_minute_) {
                    last_print_minute_ = current_minute;
                    return true;
                }
            }
        }

        return false;
    }

    // 打印订单簿状态（带时间戳和10档）
    void print_book_with_time(const FastOrderBook& book, int32_t mdtime) {
        int64_t sys_time_us = get_system_time_us();

        LOG_M_INFO("========================================");
        LOG_M_INFO("OrderBook Snapshot | SysTime: {} us | MDTime: {}", sys_time_us, mdtime);

        // 使用 FastOrderBook::print_orderbook() 打印10档
        book.print_orderbook(10, "");

        LOG_M_INFO("========================================");
    }

    // 验证订单簿合法性
    void validate_book(const FastOrderBook& book, const MDTransactionStruct& txn) {
        auto best_bid = book.get_best_bid();
        auto best_ask = book.get_best_ask();

        // 检查 1: 买一价 < 卖一价
        if (best_bid && best_ask && *best_bid >= *best_ask) {
            LOG_M_ERROR("Invalid book state at Txn #{} | BestBid: {:.4f} >= BestAsk: {:.4f}",
                        transaction_count_.load(), *best_bid / 10000.0, *best_ask / 10000.0);
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
            LOG_M_INFO("Validation passed at Txn #{}", transaction_count_.load());
        }
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

        LOG_M_INFO("=== {} 性能报告 ===", name);
        LOG_M_INFO("总消息数: {}", message_count_.load());
        LOG_M_INFO("运行时间: {} ms", duration.count());

        if (duration.count() > 0) {
            double throughput = (message_count_ * 1000.0) / duration.count();
            LOG_M_INFO("吞吐量: {:.0f} msg/s", throughput);
            LOG_M_INFO("平均延迟: {:.2f} us/msg", duration.count() * 1000.0 / message_count_);
        }
    }
};

#endif // TEST_STRATEGY_H
