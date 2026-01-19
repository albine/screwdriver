#ifndef BREAKOUT_PRICE_VOLUME_STRATEGY_H
#define BREAKOUT_PRICE_VOLUME_STRATEGY_H

#include "strategy_base.h"
#include "strategy_engine.h"  // for ControlMessage
#include "trade_signal.h"
#include "logger.h"
#include "utils/price_util.h"
#include "utils/time_util.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <deque>

#define LOG_MODULE MOD_STRATEGY

// ==========================================
// 突破价格档位挂单量监控策略
//
// 策略逻辑：
// 1. 构造时传入突破价格，立即开始监控
// 2. 维护200ms滚动窗口，跟踪突破价档位的挂单量和主买成交量
// 3. 计算 n = 窗口内挂单量平均值
// 4. 计算 delta_n = 窗口内主买成交累计和
// 5. 当 delta_n >= n 时触发买入信号（仅记录日志）
// 6. 信号只触发一次
// ==========================================

class BreakoutPriceVolumeStrategy : public Strategy {
private:
    // ==========================================
    // 状态机
    // ==========================================
    enum class MonitoringState {
        MONITORING,        // 监控中
        SIGNAL_TRIGGERED   // 信号已触发
    };

    MonitoringState state_ = MonitoringState::MONITORING;
    bool signal_triggered_ = false;

    // ==========================================
    // 价格（乘以10000的整数格式）
    // ==========================================
    uint32_t breakout_price_ = 0;    // 突破价格，例如97900表示9.79元
    uint32_t limit_up_price_ = 0;    // 涨停价（从 MDStockStruct.maxpx 获取）

    // ==========================================
    // 滚动窗口（200ms）
    // ==========================================
    struct WindowSnapshot {
        int32_t mdtime;           // 时间戳（HHMMSSMMM格式）
        uint64_t volume;          // 突破价档位的挂单量
        uint64_t buy_trade_qty;   // 主买成交量（单笔）

        WindowSnapshot(int32_t t, uint64_t v, uint64_t b)
            : mdtime(t), volume(v), buy_trade_qty(b) {}
    };

    std::deque<WindowSnapshot> window_;

    // ==========================================
    // 统计信息
    // ==========================================
    std::atomic<uint64_t> tick_count_{0};
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    std::atomic<uint64_t> buy_trade_count_{0};  // 主买成交次数

public:
    // 构造函数：无需传入突破价格，策略默认禁用
    // 突破价格通过 ControlMessage 的 param 在 enable 时传入
    explicit BreakoutPriceVolumeStrategy(const std::string& strategy_name,
                                         const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;  // 使用基类的 symbol 成员
        this->enabled_ = false;  // 默认禁用，等待 enable 消息带入 target_price
        state_ = MonitoringState::MONITORING;
    }

    virtual ~BreakoutPriceVolumeStrategy() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        // 空实现，日志宏 LOG_M_* 已内置 logger
    }

    void on_stop() override {
        // 空实现，日志已移至引擎层汇总
    }

    // 覆盖控制消息处理：enable 时从 param 获取 target_price
    void on_control_message(const ControlMessage& msg) override {
        // 用 unique_id 匹配：检查策略类型 ID 是否一致
        uint8_t msg_strategy_id = msg.unique_id & 0xFF;
        if (strategy_type_id != msg_strategy_id) {
            return;  // 不是目标策略
        }

        // 如果信号已触发，忽略 enable/disable 消息
        if (state_ == MonitoringState::SIGNAL_TRIGGERED) {
            LOG_M_WARNING("Strategy {} already triggered, ignoring {} message",
                          name, msg.type == ControlMessage::Type::ENABLE ? "ENABLE" : "DISABLE");
            return;
        }

        if (msg.type == ControlMessage::Type::ENABLE) {
            // 从 param 获取 target_price
            breakout_price_ = msg.param;
            set_enabled(true);

            LOG_M_INFO("========================================");
            LOG_M_INFO("BreakoutPriceVolumeStrategy ENABLED: {}", name);
            LOG_M_INFO("  - Breakout Price: {} ({}元)", breakout_price_, price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("========================================");
        } else if (msg.type == ControlMessage::Type::DISABLE) {
            set_enabled(false);
            window_.clear();
            LOG_M_INFO("BreakoutPriceVolumeStrategy DISABLED: {}", name);
        }
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        tick_count_++;

        // 更新涨停价（从 MDStockStruct 获取）
        if (stock.maxpx > 0) {
            limit_up_price_ = static_cast<uint32_t>(stock.maxpx);
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        order_count_++;

        if (state_ == MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // 查询突破价档位的当前挂单量
        // 如果突破价已经小于卖一价，说明已被突破，current_volume 设为 0
        uint64_t current_volume = 0;
        auto best_ask = book.get_best_ask();
        if (best_ask.has_value() && breakout_price_ >= best_ask.value()) {
            current_volume = book.get_volume_at_price(breakout_price_);
        }

        // 添加到窗口（委托事件没有成交量）
        add_to_window(order.mdtime, current_volume, 0);

        // 检查触发条件
        check_trigger_condition();
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        transaction_count_++;

        if (state_ == MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // 查询突破价档位的当前挂单量
        // 如果突破价已经小于卖一价，说明已被突破，current_volume 设为 0
        uint64_t current_volume = 0;
        auto best_ask = book.get_best_ask();
        if (best_ask.has_value() && breakout_price_ >= best_ask.value()) {
            current_volume = book.get_volume_at_price(breakout_price_);
        }

        // 判断是否是主买成交
        uint64_t buy_trade_qty = 0;
        if (txn.tradebsflag == 1 &&  // 买方向（主买）
            static_cast<uint32_t>(txn.tradeprice) == breakout_price_) {
            buy_trade_qty = txn.tradeqty;  // 记录主买成交量
            buy_trade_count_++;
        }

        // 添加到窗口
        add_to_window(txn.mdtime, current_volume, buy_trade_qty);

        // 检查触发条件
        check_trigger_condition();
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
            if (time_util::is_within_ms(window_.front().mdtime, mdtime, 200)) {
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

        // 获取最新数据点
        const auto& latest = window_.back();

        // 如果 current_volume = 0（突破价已小于卖一价），直接触发
        if (latest.volume == 0) {
            signal_triggered_ = true;
            state_ = MonitoringState::SIGNAL_TRIGGERED;

            LOG_BIZ("SIGNAL",
                    "BUY SIGNAL (BREAKOUT) | Time={} | Price={} | "
                    "current_volume=0 (price below best_ask)",
                    time_util::format_mdtime(latest.mdtime),
                    price_util::format_price_display(breakout_price_));

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED (BREAKOUT) at {}",
                       time_util::format_mdtime(latest.mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       breakout_price_,
                       price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("Reason: breakout_price < best_ask (已突破)");
            LOG_M_INFO("========================================");

            // 发送买入信号
            // 下单价格 = min(breakout_price_ * 1.017, 涨停价)
            uint32_t order_price = static_cast<uint32_t>(breakout_price_ * 1.017);
            if (limit_up_price_ > 0 && order_price > limit_up_price_) {
                order_price = limit_up_price_;
            }

            TradeSignal signal;
            signal.symbol = this->symbol;
            signal.side = TradeSignal::Side::BUY;
            signal.price = order_price;
            signal.quantity = 100;
            signal.trigger_time = latest.mdtime;
            signal.strategy_name = this->name;
            place_order(signal);
            return;
        }

        // 计算n：窗口内挂单量的平均值
        uint64_t total_volume = 0;
        for (const auto& snapshot : window_) {
            total_volume += snapshot.volume;
        }
        double n = static_cast<double>(total_volume) / window_.size();

        // 边界保护：如果n=0（突破价档位无挂单），不触发
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
                    time_util::format_mdtime(latest.mdtime),
                    price_util::format_price_display(breakout_price_),
                    n,
                    delta_n,
                    latest.volume,
                    window_.size());

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED at {}",
                       time_util::format_mdtime(latest.mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       breakout_price_,
                       price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("n (Avg Volume in 200ms): {:.0f}", n);
            LOG_M_INFO("delta_n (Buy Trades in 200ms): {}", delta_n);
            LOG_M_INFO("Current Volume: {}", latest.volume);
            LOG_M_INFO("Window Size: {} snapshots", window_.size());
            LOG_M_INFO("========================================");

            // 发送买入信号
            // 下单价格 = min(breakout_price_ * 1.017, 涨停价)
            uint32_t order_price = static_cast<uint32_t>(breakout_price_ * 1.017);
            if (limit_up_price_ > 0 && order_price > limit_up_price_) {
                order_price = limit_up_price_;
            }

            TradeSignal signal;
            signal.symbol = this->symbol;  // 使用基类的 symbol
            signal.side = TradeSignal::Side::BUY;
            signal.price = order_price;
            signal.quantity = 100;  // 默认下单数量，可根据需求调整
            signal.trigger_time = latest.mdtime;
            signal.strategy_name = this->name;
            place_order(signal);
        }
    }

};
#undef LOG_MODULE
#endif // BREAKOUT_PRICE_VOLUME_STRATEGY_H
