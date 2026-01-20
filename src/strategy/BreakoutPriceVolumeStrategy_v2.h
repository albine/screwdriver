#ifndef BREAKOUT_PRICE_VOLUME_STRATEGY_V2_H
#define BREAKOUT_PRICE_VOLUME_STRATEGY_V2_H

#include "strategy_base.h"
#include "strategy_engine.h"  // for ControlMessage
#include "trade_signal.h"
#include "logger.h"
#include "utils/price_util.h"
#include "utils/time_util.h"
#include <atomic>
#include <deque>

#define LOG_MODULE MOD_STRATEGY

// ==========================================
// 突破价格档位挂单量监控策略 V2
//
// 与 V1 的区别：
// - 使用 MDOrderbookStruct.sellentries 获取突破价挂单量
// - 不再使用 FastOrderBook.get_volume_at_price()
// - on_order() 不再处理（时间无法对齐）
// - 仅在 on_orderbook_snapshot() 中更新挂单量缓存
//
// 策略逻辑：
// 1. 构造时传入突破价格，立即开始监控
// 2. 维护200ms滚动窗口，跟踪突破价档位的挂单量和主买成交量
// 3. 计算 n = 窗口内挂单量平均值
// 4. 计算 delta_n = 窗口内主买成交累计和
// 5. 当 delta_n >= n 时触发买入信号（仅记录日志）
// 6. 信号只触发一次
// ==========================================

class BreakoutPriceVolumeStrategy_v2 : public Strategy {
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

    // ==========================================
    // 快照数据缓存（V2 新增）
    // ==========================================
    uint64_t cached_volume_ = 0;           // 缓存的突破价档位挂单量
    bool volume_available_ = false;        // 是否已获取到挂单量
    int32_t last_snapshot_time_ = 0;       // 最后快照时间

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
    std::atomic<uint64_t> buy_trade_count_{0};      // 主买成交次数
    std::atomic<uint64_t> snapshot_count_{0};       // 快照接收次数
    std::atomic<uint64_t> price_found_count_{0};    // 找到突破价格的次数

public:
    // 构造函数：传入突破价格，策略默认启用
    explicit BreakoutPriceVolumeStrategy_v2(const std::string& strategy_name,
                                            const std::string& sym,
                                            uint32_t breakout_price) {
        this->name = strategy_name;
        this->symbol = sym;
        this->breakout_price_ = breakout_price;
        this->enabled_ = true;
        state_ = MonitoringState::MONITORING;
    }

    virtual ~BreakoutPriceVolumeStrategy_v2() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        LOG_M_INFO("BreakoutPriceVolumeStrategy_v2 started: {} | symbol={} | breakout_price={} ({}元) | enabled={}",
                   name, symbol, breakout_price_, price_util::price_to_yuan(breakout_price_),
                   is_enabled() ? "true" : "false");
    }

    void on_stop() override {
        LOG_M_INFO("BreakoutPriceVolumeStrategy_v2 stopped: {} | ticks={} | orders={} | txns={} | snapshots={} | triggered={}",
                   name, tick_count_.load(), order_count_.load(), transaction_count_.load(),
                   snapshot_count_.load(), signal_triggered_ ? "yes" : "no");
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
            // 重置缓存状态
            cached_volume_ = 0;
            volume_available_ = false;
            last_snapshot_time_ = 0;
            set_enabled(true);

            LOG_M_INFO("========================================");
            LOG_M_INFO("BreakoutPriceVolumeStrategy_v2 ENABLED: {}", name);
            LOG_M_INFO("  - Breakout Price: {} ({}元)", breakout_price_, price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("  - Using OrderBook snapshot for volume (v2)");
            LOG_M_INFO("========================================");
        } else if (msg.type == ControlMessage::Type::DISABLE) {
            set_enabled(false);
            window_.clear();
            cached_volume_ = 0;
            volume_available_ = false;
            LOG_M_INFO("BreakoutPriceVolumeStrategy_v2 DISABLED: {}", name);
        }
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        tick_count_++;
        // 不需要检查任何条件，直接处于监控状态
    }

    // V2: on_order 不再处理（时间无法与 snapshot 对齐）
    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        order_count_++;
        // V2: 不再使用 order 事件更新窗口
        // 因为 order 和 snapshot 的时间无法对齐
    }

    // V2: 使用 OrderBook 快照获取挂单量
    void on_orderbook_snapshot(const MDOrderbookStruct& snapshot) override {
        if (!is_enabled()) return;
        snapshot_count_++;

        if (state_ == MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // 搜索卖盘档位中的突破价格
        for (int i = 0; i < snapshot.sellentries_count; ++i) {
            // MDEntryDetailStruct.price 是 int64_t，需要转换为 uint32_t 比较
            if (static_cast<uint32_t>(snapshot.sellentries[i].price) == breakout_price_) {
                // 找到突破价格，更新缓存
                // MDEntryDetailStruct.totalqty 是 int32_t
                cached_volume_ = static_cast<uint64_t>(snapshot.sellentries[i].totalqty);
                volume_available_ = true;
                last_snapshot_time_ = snapshot.mdtime;
                price_found_count_++;

                // 添加到窗口（快照事件没有成交量）
                add_to_window(snapshot.mdtime, cached_volume_, 0);

                // 检查触发条件
                check_trigger_condition();
                return;
            }
        }
        // 未找到突破价格，保持上一次的值
        // 不更新 volume_available_，继续等待
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        transaction_count_++;

        if (state_ == MonitoringState::SIGNAL_TRIGGERED) {
            return;
        }

        // V2: 如果还没有获取到挂单量，跳过处理
        if (!volume_available_) {
            return;
        }

        // 判断是否是主买成交
        uint64_t buy_trade_qty = 0;
        if (txn.tradebsflag == 1 &&  // 买方向（主买）
            static_cast<uint32_t>(txn.tradeprice) == breakout_price_) {
            buy_trade_qty = txn.tradeqty;  // 记录主买成交量
            buy_trade_count_++;
        }

        // V2: 使用缓存的挂单量（来自最近的快照）
        // 添加到窗口
        add_to_window(txn.mdtime, cached_volume_, buy_trade_qty);

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
                    "BUY SIGNAL (v2) | Time={} | Price={} | "
                    "n(avg_volume)={:.0f} | delta_n(buy_trades)={} | "
                    "current_volume={} | window_size={} | snapshots_received={}",
                    time_util::format_mdtime(latest.mdtime),
                    price_util::format_price_display(breakout_price_),
                    n,
                    delta_n,
                    latest.volume,
                    window_.size(),
                    snapshot_count_.load());

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED (v2) at {}",
                       time_util::format_mdtime(latest.mdtime));
            LOG_M_INFO("Price Level: {} ({}元)",
                       breakout_price_,
                       price_util::price_to_yuan(breakout_price_));
            LOG_M_INFO("n (Avg Volume in 200ms): {:.0f}", n);
            LOG_M_INFO("delta_n (Buy Trades in 200ms): {}", delta_n);
            LOG_M_INFO("Current Volume: {}", latest.volume);
            LOG_M_INFO("Window Size: {} snapshots", window_.size());
            LOG_M_INFO("Total Snapshots Received: {}", snapshot_count_.load());
            LOG_M_INFO("Price Found in Snapshots: {} times", price_found_count_.load());
            LOG_M_INFO("========================================");

            // 发送买入信号
            TradeSignal signal;
            signal.symbol = this->symbol;  // 使用基类的 symbol
            signal.side = TradeSignal::Side::BUY;
            signal.price = breakout_price_;
            signal.quantity = 100;  // 默认下单数量，可根据需求调整
            signal.trigger_time = latest.mdtime;
            signal.strategy_name = this->name;
            signal.strategy_type_id = strategy_type_id;
            place_order(signal);
        }
    }

};
#undef LOG_MODULE
#endif // BREAKOUT_PRICE_VOLUME_STRATEGY_V2_H
