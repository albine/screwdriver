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
#include <thread>
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
    uint32_t open_price_ = 0;        // 开盘价
    uint32_t prev_close_ = 0;        // 昨收价

    // ==========================================
    // 统计信息
    // ==========================================
    std::atomic<uint64_t> tick_count_{0};
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};
    int64_t local_recv_timestamp_ = 0;  // 本地接收时间戳

public:
    // 构造函数：传入突破价格，策略默认启用
    explicit BreakoutPriceVolumeStrategy(const std::string& strategy_name,
                                         const std::string& sym,
                                         uint32_t breakout_price) {
        this->name = strategy_name;
        this->symbol = sym;
        this->breakout_price_ = breakout_price;
        this->enabled_ = true;

        // 初始化检测器
        detector_.set_target_price(breakout_price_);
        detector_.set_callback([this](uint32_t price, int32_t mdtime) {
            on_breakout_triggered(price, mdtime);
        });
        detector_.set_enabled(true);
    }

    virtual ~BreakoutPriceVolumeStrategy() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        LOG_M_DEBUG("BreakoutPriceVolumeStrategy started: {} | symbol={} | breakout_price={} ({}元) | enabled={}",
                   name, symbol, breakout_price_, price_util::price_to_yuan(breakout_price_),
                   is_enabled() ? "true" : "false");

    }

    void on_stop() override {
        LOG_M_DEBUG("BreakoutPriceVolumeStrategy stopped: {} | ticks={} | orders={} | txns={} | triggered={}",
                   name, tick_count_.load(), order_count_.load(), transaction_count_.load(),
                   detector_.is_triggered() ? "yes" : "no");
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

            LOG_M_DEBUG("========================================");
            LOG_M_DEBUG("BreakoutPriceVolumeStrategy ENABLED: {}", name);
            LOG_M_DEBUG("  - Breakout Price: {} ({}元)", breakout_price_, price_util::price_to_yuan(breakout_price_));
            LOG_M_DEBUG("========================================");
        } else if (msg.type == ControlMessage::Type::DISABLE) {
            detector_.set_enabled(false);
            set_enabled(false);
            LOG_M_DEBUG("BreakoutPriceVolumeStrategy DISABLED: {}", name);
        }
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        tick_count_++;

        // 更新价格信息（从 MDStockStruct 获取）
        if (stock.maxpx > 0) {
            limit_up_price_ = static_cast<uint32_t>(stock.maxpx);
        }
        if (stock.openpx > 0 && open_price_ == 0) {
            open_price_ = static_cast<uint32_t>(stock.openpx);
        }
        if (stock.preclosepx > 0 && prev_close_ == 0) {
            prev_close_ = static_cast<uint32_t>(stock.preclosepx);
        }
        local_recv_timestamp_ = stock.local_recv_timestamp;
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
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | Reason=突破价位监控 (直接突破)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp_),
                    price_util::format_price_display(price),
                    price_util::price_to_yuan(open_price_),
                    price_util::price_to_yuan(prev_close_));

            LOG_M_DEBUG("========================================");
            LOG_M_DEBUG("SIGNAL TRIGGERED (BREAKOUT) at {}",
                       time_util::format_mdtime(mdtime));
            LOG_M_DEBUG("Price Level: {} ({}元)",
                       price,
                       price_util::price_to_yuan(price));
            LOG_M_DEBUG("Reason: breakout_price < best_ask (已突破)");
            LOG_M_DEBUG("========================================");
        } else {
            // 情况2：delta_n >= n
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | n={:.0f} | delta_n={} | Reason=突破价位监控 (动力学触发)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp_),
                    price_util::format_price_display(price),
                    price_util::price_to_yuan(open_price_),
                    price_util::price_to_yuan(prev_close_),
                    stats.avg_volume, stats.total_buy_qty);

            LOG_M_DEBUG("========================================");
            LOG_M_DEBUG("SIGNAL TRIGGERED at {}",
                       time_util::format_mdtime(mdtime));
            LOG_M_DEBUG("Price Level: {} ({}元)",
                       price,
                       price_util::price_to_yuan(price));
            LOG_M_DEBUG("n (Avg Volume in 200ms): {:.0f}", stats.avg_volume);
            LOG_M_DEBUG("delta_n (Buy Trades in 200ms): {}", stats.total_buy_qty);
            LOG_M_DEBUG("Current Volume: {}", stats.current_volume);
            LOG_M_DEBUG("Window Size: {} snapshots", stats.window_size);
            LOG_M_DEBUG("========================================");
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
        signal.strategy_type_id = strategy_type_id;
        place_order(signal);
    }
};
#undef LOG_MODULE
#endif // BREAKOUT_PRICE_VOLUME_STRATEGY_H
