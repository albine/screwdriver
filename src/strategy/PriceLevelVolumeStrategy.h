#ifndef PRICE_LEVEL_VOLUME_STRATEGY_H
#define PRICE_LEVEL_VOLUME_STRATEGY_H

#include "strategy_base.h"
#include "logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cmath>

#define LOG_MODULE MOD_STRATEGY

// ==========================================
// 价格档位挂单量监控策略
//
// 策略逻辑：
// 1. 当开盘价 < 昨收价时，开始监控
// 2. 维护200ms滚动窗口，跟踪昨收价档位的挂单量和主买成交量
// 3. 计算 n = 窗口内挂单量平均值
// 4. 计算 delta_n = 窗口内主买成交累计和
// 5. 当 delta_n >= n 时触发买入信号（仅记录日志）
// 6. 信号只触发一次
// ==========================================

class PriceLevelVolumeStrategy : public Strategy {
private:
    // ==========================================
    // 状态机
    // ==========================================
    enum class MonitoringState {
        IDLE,              // 初始状态，等待第一个Tick
        WAITING_FOR_TICK,  // 已记录昨收价，等待开盘价数据
        MONITORING,        // 监控中
        SIGNAL_TRIGGERED   // 信号已触发
    };

    MonitoringState state_ = MonitoringState::IDLE;
    bool signal_triggered_ = false;

    // ==========================================
    // 价格（乘以10000的整数格式）
    // ==========================================
    uint32_t preclose_price_ = 0;    // 昨收价，例如97900表示9.79元
    uint32_t open_price_ = 0;        // 开盘价

    // ==========================================
    // 滚动窗口（200ms）
    // ==========================================
    struct WindowSnapshot {
        int32_t mdtime;           // 时间戳（HHMMSSMMM格式）
        uint64_t volume;          // 昨收价档位的挂单量
        uint64_t buy_trade_qty;   // 主买成交量（单笔）

        WindowSnapshot(int32_t t, uint64_t v, uint64_t b)
            : mdtime(t), volume(v), buy_trade_qty(b) {}
    };

    std::deque<WindowSnapshot> window_;

    // ==========================================
    // 日志器
    // ==========================================
    quill::Logger* logger_ = nullptr;

    // ==========================================
    // 统计信息
    // ==========================================
    std::atomic<uint64_t> tick_count_{0};
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    std::atomic<uint64_t> buy_trade_count_{0};  // 主买成交次数

public:
    explicit PriceLevelVolumeStrategy(const std::string& strategy_name,
                                       uint32_t preclose_price = 0,
                                       uint32_t open_price = 0) {
        this->name = strategy_name;
        // Note: std::deque does not have reserve(), unlike std::vector

        // 如果提供了昨收价和开盘价，直接使用（用于回测）
        if (preclose_price > 0 && open_price > 0 && open_price < preclose_price) {
            preclose_price_ = preclose_price;
            open_price_ = open_price;
            state_ = MonitoringState::MONITORING;
        }
    }

    virtual ~PriceLevelVolumeStrategy() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        logger_ = hft::logger::get_logger();

        LOG_M_INFO("========================================");
        LOG_M_INFO("PriceLevelVolumeStrategy started: {}", name);
        LOG_M_INFO("Configuration:");
        LOG_M_INFO("  - Window Size: 200ms rolling");
        LOG_M_INFO("  - Monitoring: Preclose price level");
        LOG_M_INFO("  - n = Avg volume in window");
        LOG_M_INFO("  - delta_n = Sum of buy trades in window");
        LOG_M_INFO("  - Trigger: delta_n >= n");

        if (state_ == MonitoringState::MONITORING) {
            LOG_M_INFO("  - Preclose Price: {} ({}元)", preclose_price_, price_to_yuan(preclose_price_));
            LOG_M_INFO("  - Open Price: {} ({}元)", open_price_, price_to_yuan(open_price_));
            LOG_M_INFO("  - State: MONITORING (initialized from parameters)");
        }

        LOG_M_INFO("========================================");
    }

    void on_stop() override {
        LOG_M_INFO("========================================");
        LOG_M_INFO("PriceLevelVolumeStrategy stopped: {}", name);
        LOG_M_INFO("Statistics:");
        LOG_M_INFO("  - Ticks: {}", tick_count_.load());
        LOG_M_INFO("  - Orders: {}", order_count_.load());
        LOG_M_INFO("  - Transactions: {}", transaction_count_.load());
        LOG_M_INFO("  - Buy Trades (at preclose): {}", buy_trade_count_.load());
        LOG_M_INFO("  - Signal Triggered: {}", signal_triggered_);
        LOG_M_INFO("========================================");
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        tick_count_++;

        if (state_ == MonitoringState::IDLE) {
            // 记录昨收价（注意：价格是乘以10000的整数）
            preclose_price_ = static_cast<uint32_t>(stock.preclosepx);

            LOG_M_INFO("Initialized preclose_price={} ({}元)",
                       preclose_price_,
                       price_to_yuan(preclose_price_));

            state_ = MonitoringState::WAITING_FOR_TICK;
        }

        if (state_ == MonitoringState::WAITING_FOR_TICK) {
            // 检查是否满足 openpx < preclosepx
            if (stock.openpx > 0 && stock.openpx < stock.preclosepx) {
                open_price_ = static_cast<uint32_t>(stock.openpx);

                LOG_M_INFO("Monitoring condition MET: openpx={} ({}元) < preclosepx={} ({}元)",
                           open_price_,
                           price_to_yuan(open_price_),
                           preclose_price_,
                           price_to_yuan(preclose_price_));

                state_ = MonitoringState::MONITORING;
            }
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;

        if (state_ != MonitoringState::MONITORING && state_ != MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // 调试：打印特定委托后的十档盘口
        // if (order.orderindex == 707223) {
        //     char context[256];
        //     std::snprintf(context, sizeof(context),
        //                   "DEBUG: After processing OrderIndex=%llu\nOrder Info: Price=%lld Qty=%lld BSFlag=%d Type=%d",
        //                   order.orderindex, order.orderprice, order.orderqty, order.orderbsflag, order.ordertype);
        //     book.print_orderbook(logger_, 10, std::string(context));
        // }

        // 查询昨收价档位的当前挂单量
        // 注意：FastOrderBook已经在Strategy回调前更新了订单簿
        uint64_t current_volume = book.get_volume_at_price(preclose_price_);

        // 添加到窗口（委托事件没有成交量）
        add_to_window(order.mdtime, current_volume, 0);

        // 检查触发条件
        if (state_ == MonitoringState::MONITORING) {
            check_trigger_condition();
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;

        if (state_ != MonitoringState::MONITORING && state_ != MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // 调试：打印特定委托后的十档盘口
        if (txn.tradeindex == 439514) {
            book.print_orderbook(10, "tradeindex=439514");
        }

        // 查询昨收价档位的当前挂单量
        uint64_t current_volume = book.get_volume_at_price(preclose_price_);

        // 判断是否是主买成交
        uint64_t buy_trade_qty = 0;
        if (txn.tradebsflag == 1 &&  // 买方向（主买）
            static_cast<uint32_t>(txn.tradeprice) == preclose_price_) {
            buy_trade_qty = txn.tradeqty;  // 记录主买成交量
            buy_trade_count_++;
        }

        // 添加到窗口
        add_to_window(txn.mdtime, current_volume, buy_trade_qty);

        // 检查触发条件
        if (state_ == MonitoringState::MONITORING) {
            check_trigger_condition();
        }
    }

private:
    // ==========================================
    // 窗口管理
    // ==========================================
    void add_to_window(int32_t mdtime, uint64_t volume, uint64_t buy_trade_qty) {
        // 添加新数据点
        window_.emplace_back(mdtime, volume, buy_trade_qty);

        // 移除超过200ms的旧数据
        while (!window_.empty()) {
            if (is_within_200ms(window_.front().mdtime, mdtime)) {
                break;  // 还在窗口内
            }
            window_.pop_front();
        }
    }

    // ==========================================
    // 触发条件检测
    // ==========================================
    void check_trigger_condition() {
        if (signal_triggered_ || window_.empty()) {
            return;
        }

        // 计算n：窗口内挂单量的平均值
        uint64_t total_volume = 0;
        for (const auto& snapshot : window_) {
            total_volume += snapshot.volume;
        }
        double n = static_cast<double>(total_volume) / window_.size();

        // 边界保护：如果n=0（昨收价档位无挂单），不触发
        if (n < 1.0) {
            return;
        }

        // 计算delta_n：窗口内主买成交量的累计和
        uint64_t delta_n = 0;
        for (const auto& snapshot : window_) {
            delta_n += snapshot.buy_trade_qty;
        }

        // 检查触发条件：delta_n >= n
        if (delta_n >= static_cast<uint64_t>(n)) {
            signal_triggered_ = true;
            state_ = MonitoringState::SIGNAL_TRIGGERED;

            // 获取当前时间和挂单量（窗口最后一个数据点）
            const auto& latest = window_.back();

            // 输出详细日志
            LOG_BIZ("SIGNAL",
                    "BUY SIGNAL | Time={} | Price={} | "
                    "n(avg_volume)={:.0f} | delta_n(buy_trades)={} | "
                    "current_volume={} | window_size={}",
                    format_mdtime(latest.mdtime),
                    format_price_display(preclose_price_),
                    n,
                    delta_n,
                    latest.volume,
                    window_.size());

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED at {}",
                       format_mdtime(latest.mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       preclose_price_,
                       price_to_yuan(preclose_price_));
            LOG_M_INFO("n (Avg Volume in 200ms): {:.0f}", n);
            LOG_M_INFO("delta_n (Buy Trades in 200ms): {}", delta_n);
            LOG_M_INFO("Current Volume: {}", latest.volume);
            LOG_M_INFO("Window Size: {} snapshots", window_.size());
            LOG_M_INFO("========================================");
        }
    }

    // ==========================================
    // 时间计算
    // ==========================================
    int64_t calculate_time_diff_ms(int32_t time1, int32_t time2) const {
        // MDTime格式: HHMMSSMMM (9位数字)
        // 例如: 093015500 = 09:30:15.500

        // 提取时分秒毫秒
        int32_t h1 = time1 / 10000000;
        int32_t m1 = (time1 / 100000) % 100;
        int32_t s1 = (time1 / 1000) % 100;
        int32_t ms1 = time1 % 1000;

        int32_t h2 = time2 / 10000000;
        int32_t m2 = (time2 / 100000) % 100;
        int32_t s2 = (time2 / 1000) % 100;
        int32_t ms2 = time2 % 1000;

        // 转换为总毫秒数
        int64_t total1 = (h1 * 3600000LL) + (m1 * 60000LL) + (s1 * 1000LL) + ms1;
        int64_t total2 = (h2 * 3600000LL) + (m2 * 60000LL) + (s2 * 1000LL) + ms2;

        return total2 - total1;
    }

    bool is_within_200ms(int32_t time1, int32_t time2) const {
        int64_t diff = calculate_time_diff_ms(time1, time2);
        return (diff >= 0 && diff <= 200);
    }

    // ==========================================
    // 格式化输出
    // ==========================================

    // 格式化价格显示（带人民币单位）
    std::string format_price_display(uint32_t price) const {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u(%.2f元)", price, price / 10000.0);
        return std::string(buf);
    }

    // 仅转换为元（用于数值计算显示）
    double price_to_yuan(uint32_t price) const {
        return price / 10000.0;
    }

    std::string format_mdtime(int32_t mdtime) const {
        int32_t h = mdtime / 10000000;
        int32_t m = (mdtime / 100000) % 100;
        int32_t s = (mdtime / 1000) % 100;
        int32_t ms = mdtime % 1000;

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, ms);
        return std::string(buf);
    }
};
#undef LOG_MODULE
#endif // PRICE_LEVEL_VOLUME_STRATEGY_H
