#ifndef BREAKOUT_PRICE_VOLUME_STRATEGY_H
#define BREAKOUT_PRICE_VOLUME_STRATEGY_H

#include "strategy_base.h"
#include "strategy_engine.h"  // for ControlMessage
#include "trade_signal.h"
#include "logger.h"
#include "breakout_detector.h"
#include "utils/price_util.h"
#include "utils/time_util.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>

#define LOG_MODULE MOD_STRATEGY

// ==========================================
// 突破价格档位挂单量监控策略
//
// 策略逻辑：
// 1. 构造时传入突破价格，立即开始监控
// 2. 使用 BreakoutDetector 维护200ms滚动窗口
// 3. 当 delta_n >= n 或 target_price < best_ask 时触发买入信号
// 4. 信号只触发一次
// ==========================================

class BreakoutPriceVolumeStrategy : public Strategy {
private:
    // ==========================================
    // 突破检测器
    // ==========================================
    BreakoutDetector detector_;

    // ==========================================
    // 价格（乘以10000的整数格式）
    // ==========================================
    uint32_t breakout_price_ = 0;    // 突破价格，例如97900表示9.79元
    uint32_t limit_up_price_ = 0;    // 涨停价（从 MDStockStruct.maxpx 获取）

    // ==========================================
    // 统计信息
    // ==========================================
    std::atomic<uint64_t> tick_count_{0};
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};

public:
    // 构造函数：无需传入突破价格，策略默认禁用
    // 突破价格通过 ControlMessage 的 param 在 enable 时传入
    explicit BreakoutPriceVolumeStrategy(const std::string& strategy_name,
                                         const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;  // 使用基类的 symbol 成员
        this->enabled_ = false;  // 默认禁用，等待 enable 消息带入 target_price
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
        if (detector_.is_triggered()) {
            LOG_M_WARNING("Strategy {} already triggered, ignoring {} message",
                          name, msg.type == ControlMessage::Type::ENABLE ? "ENABLE" : "DISABLE");
            return;
        }

        if (msg.type == ControlMessage::Type::ENABLE) {
            // 从 param 获取 target_price
            breakout_price_ = msg.param;

            // 设置检测器
            detector_.set_target_price(breakout_price_);
            detector_.set_callback([this](uint32_t price, int32_t mdtime) {
                on_breakout_triggered(price, mdtime);
            });
            detector_.set_enabled(true);
            set_enabled(true);

            LOG_M_INFO("========================================");
            LOG_M_INFO("BreakoutPriceVolumeStrategy ENABLED: {}", name);
            LOG_M_INFO("  - Breakout Price: {} ({}元)", breakout_price_, price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("========================================");
        } else if (msg.type == ControlMessage::Type::DISABLE) {
            detector_.set_enabled(false);
            set_enabled(false);
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

        // 委托给检测器处理
        detector_.on_order(order, book);
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        transaction_count_++;

        // 委托给检测器处理
        detector_.on_transaction(txn, book);
    }

private:
    // ==========================================
    // 突破触发回调
    // ==========================================
    void on_breakout_triggered(uint32_t price, int32_t mdtime) {
        // 获取统计信息用于日志
        auto stats = detector_.get_stats();

        if (stats.current_volume == 0) {
            // 情况1：价格已突破（target_price < best_ask）
            LOG_BIZ("SIGNAL",
                    "BUY SIGNAL (BREAKOUT) | Time={} | Price={} | "
                    "current_volume=0 (price below best_ask)",
                    time_util::format_mdtime(mdtime),
                    price_util::format_price_display(price));

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED (BREAKOUT) at {}",
                       time_util::format_mdtime(mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       price,
                       price_util::price_to_yuan(price));
            LOG_M_INFO("Reason: breakout_price < best_ask (已突破)");
            LOG_M_INFO("========================================");
        } else {
            // 情况2：delta_n >= n
            LOG_BIZ("SIGNAL",
                    "BUY SIGNAL | Time={} | Price={} | "
                    "n(avg_volume)={:.0f} | delta_n(buy_trades)={} | "
                    "current_volume={} | window_size={}",
                    time_util::format_mdtime(mdtime),
                    price_util::format_price_display(price),
                    stats.avg_volume,
                    stats.total_buy_qty,
                    stats.current_volume,
                    stats.window_size);

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED at {}",
                       time_util::format_mdtime(mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       price,
                       price_util::price_to_yuan(price));
            LOG_M_INFO("n (Avg Volume in 200ms): {:.0f}", stats.avg_volume);
            LOG_M_INFO("delta_n (Buy Trades in 200ms): {}", stats.total_buy_qty);
            LOG_M_INFO("Current Volume: {}", stats.current_volume);
            LOG_M_INFO("Window Size: {} snapshots", stats.window_size);
            LOG_M_INFO("========================================");
        }

        // 发送买入信号
        // 下单价格 = min(breakout_price_ * 1.017, 涨停价)
        uint32_t order_price = static_cast<uint32_t>(price * 1.017);
        if (limit_up_price_ > 0 && order_price > limit_up_price_) {
            order_price = limit_up_price_;
        }

        TradeSignal signal;
        signal.symbol = this->symbol;
        signal.side = TradeSignal::Side::BUY;
        signal.price = order_price;
        signal.quantity = 100;
        signal.trigger_time = mdtime;
        signal.strategy_name = this->name;
        place_order(signal);
    }
};
#undef LOG_MODULE
#endif // BREAKOUT_PRICE_VOLUME_STRATEGY_H
