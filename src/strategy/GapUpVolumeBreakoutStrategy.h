#ifndef GAP_UP_VOLUME_BREAKOUT_STRATEGY_H
#define GAP_UP_VOLUME_BREAKOUT_STRATEGY_H

#include "strategy_engine.h"
#include "market_data_structs.h"
#include "logger.h"
#include <map>
#include <deque>
#include <string>
#include <cstdio>
#include <cmath>
#include <atomic>

#define LOG_MODULE MOD_STRATEGY

/**
 * Gap-Up Volume Breakout Strategy
 *
 * 结合 OpeningRangeBreakoutStrategy 和 PriceLevelVolumeStrategy:
 *
 * Phase 1 (Gap-Up Detection & Price Locking):
 * - 监控条件: open_price >= prev_close (高开)
 * - 跟踪最高价，30秒内更新，超过30秒锁定
 * - 锁定后的最高价作为突破价格档位
 *
 * Phase 2 (Volume-Based Trigger):
 * - 维护200ms滚动窗口
 * - 跟踪锁定价格档位的挂单量和主买成交量
 * - 计算 n = 窗口内挂单量平均值
 * - 计算 delta_n = 窗口内主买成交累计和
 * - 当 delta_n >= n 时触发买入信号（仅记录日志）
 * - 信号只触发一次
 */
class GapUpVolumeBreakoutStrategy : public Strategy {
private:
    // ==========================================
    // 状态机
    // ==========================================
    enum class State {
        IDLE,                  // 初始状态
        DETECTING_GAP_UP,      // 检测高开
        TRACKING_HIGH_PRICE,   // 跟踪最高价（30秒锁定期）
        MONITORING_VOLUME,     // 监控成交量
        SIGNAL_TRIGGERED       // 信号已触发
    };

    // ==========================================
    // 窗口快照（200ms滚动窗口）
    // ==========================================
    struct WindowSnapshot {
        int32_t mdtime;           // 时间戳（HHMMSSMMM格式）
        uint64_t volume;          // 突破价格档位的挂单量
        uint64_t buy_trade_qty;   // 主买成交量（单笔）

        WindowSnapshot(int32_t t, uint64_t v, uint64_t b)
            : mdtime(t), volume(v), buy_trade_qty(b) {}
    };

    // ==========================================
    // 单股状态
    // ==========================================
    struct StockState {
        State state = State::IDLE;

        // Phase 1: 价格跟踪（ORB风格）
        double open_price = 0.0;
        double prev_close = 0.0;
        double highest_price = 0.0;
        int64_t highest_timestamp_mdtime = 0;  // mdtime格式 (HHMMSSMMM * 1000)

        // Phase 2: 成交量监控（PLV风格）
        uint32_t locked_breakout_price = 0;    // 锁定的突破价格（price * 10000）
        std::deque<WindowSnapshot> window_;
        bool signal_triggered = false;

        // 交易日跟踪
        int32_t current_date = 0;
    };

    std::map<std::string, StockState> stock_states_;

    // ==========================================
    // 统计信息
    // ==========================================
    std::atomic<uint64_t> tick_count_{0};
    std::atomic<uint64_t> order_count_{0};
    std::atomic<uint64_t> transaction_count_{0};

    // ==========================================
    // 常量
    // ==========================================
    static constexpr int64_t THIRTY_SECONDS_MS = 30000000;  // 30秒 (mdtime格式)
    static constexpr int64_t TEN_MINUTES_MS = 600000000;    // 10分钟 (mdtime格式)

    // ==========================================
    // 日志器
    // ==========================================
    quill::Logger* logger_ = nullptr;

public:
    explicit GapUpVolumeBreakoutStrategy(const std::string& strategy_name) {
        this->name = strategy_name;
    }

    virtual ~GapUpVolumeBreakoutStrategy() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        logger_ = hft::logger::get_logger();
        LOG_M_INFO("========================================");
        LOG_M_INFO("GapUpVolumeBreakoutStrategy started: {}", name);
        LOG_M_INFO("Phase 1: Gap-up detection with 30s price lock");
        LOG_M_INFO("Phase 2: Volume monitoring (200ms window, delta_n >= n)");
        LOG_M_INFO("========================================");
    }

    void on_stop() override {
        LOG_M_INFO("========================================");
        LOG_M_INFO("GapUpVolumeBreakoutStrategy stopped: {}", name);
        LOG_M_INFO("Statistics:");
        LOG_M_INFO("  - Ticks: {}", tick_count_.load());
        LOG_M_INFO("  - Orders: {}", order_count_.load());
        LOG_M_INFO("  - Transactions: {}", transaction_count_.load());
        LOG_M_INFO("========================================");
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        tick_count_++;

        std::string symbol = get_symbol(stock);

        // 检查交易时段
        if (!is_market_open(stock.mdtime)) {
            return;
        }

        auto& state = stock_states_[symbol];

        // 检测新交易日
        if (state.current_date != stock.mddate) {
            on_market_open(state, stock, symbol);
        }

        // 如果信号已触发，跳过
        if (state.signal_triggered) {
            return;
        }

        // 只在开盘10分钟内处理
        int64_t time_since_open = get_time_since_open_ms(stock.mdtime);
        if (time_since_open > TEN_MINUTES_MS) {
            return;
        }

        // Phase 1: Gap-Up Detection & Price Tracking
        if (state.state == State::IDLE || state.state == State::DETECTING_GAP_UP) {
            double open_px = to_price(stock.openpx);
            double prev_close_px = to_price(stock.preclosepx);

            if (open_px > 0 && prev_close_px > 0) {
                state.open_price = open_px;
                state.prev_close = prev_close_px;

                if (open_px >= prev_close_px) {
                    // Gap-up detected
                    state.state = State::TRACKING_HIGH_PRICE;
                    state.highest_price = to_price(stock.highpx);
                    state.highest_timestamp_mdtime = stock.mdtime;

                    LOG_M_INFO("{} Gap-up detected: open={:.4f} >= prev_close={:.4f}, highest={:.4f}",
                               symbol, open_px, prev_close_px, state.highest_price);
                } else {
                    // Not gap-up, don't monitor this stock today
                    state.state = State::SIGNAL_TRIGGERED;  // Skip to end state
                    return;
                }
            }
        }

        // Phase 1: Track highest price and check for lock
        if (state.state == State::TRACKING_HIGH_PRICE) {
            double high_price = to_price(stock.highpx);
            int64_t consolidation_duration = stock.mdtime - state.highest_timestamp_mdtime;

            if (consolidation_duration >= THIRTY_SECONDS_MS) {
                // Lock the breakout price
                state.locked_breakout_price = static_cast<uint32_t>(state.highest_price * 10000);
                state.state = State::MONITORING_VOLUME;

                LOG_M_INFO("{} Breakout price LOCKED at {:.4f} ({}), starting volume monitoring",
                           symbol, state.highest_price, state.locked_breakout_price);
            } else {
                // Still within 30-second window, update if new high
                if (high_price > state.highest_price) {
                    state.highest_price = high_price;
                    state.highest_timestamp_mdtime = stock.mdtime;
                    LOG_M_DEBUG("{} New high: {:.4f}", symbol, high_price);
                }
            }
        }

        // Phase 2: Monitor volume at locked breakout price
        if (state.state == State::MONITORING_VOLUME) {
            // Query current volume at locked breakout price
            uint64_t current_volume = get_volume_at_locked_price(stock, state);

            // Add to rolling window (no buy trade in tick event)
            add_to_window(state, stock.mdtime, current_volume, 0);

            // Check trigger condition
            check_trigger_condition(state, symbol, stock.mdtime);
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        order_count_++;

        std::string symbol = get_symbol_from_order(order);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) {
            return;
        }

        auto& state = it->second;

        // Only process if monitoring volume
        if (state.state != State::MONITORING_VOLUME) {
            return;
        }

        // Query current volume at locked breakout price
        uint64_t current_volume = book.get_volume_at_price(state.locked_breakout_price);

        // Add to window (order events have no buy trade)
        add_to_window(state, order.mdtime, current_volume, 0);

        // Check trigger condition
        check_trigger_condition(state, symbol, order.mdtime);
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        transaction_count_++;

        std::string symbol = get_symbol_from_transaction(txn);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) {
            return;
        }

        auto& state = it->second;

        // Only process if monitoring volume
        if (state.state != State::MONITORING_VOLUME) {
            return;
        }

        // Query current volume at locked breakout price
        uint64_t current_volume = book.get_volume_at_price(state.locked_breakout_price);

        // Check if this is an active buy trade at the breakout price
        uint64_t buy_trade_qty = 0;
        if (txn.tradebsflag == 1 &&  // Buy direction (主买)
            static_cast<uint32_t>(txn.tradeprice) == state.locked_breakout_price) {
            buy_trade_qty = txn.tradeqty;
        }

        // Add to window
        add_to_window(state, txn.mdtime, current_volume, buy_trade_qty);

        // Check trigger condition
        check_trigger_condition(state, symbol, txn.mdtime);
    }

private:
    // ==========================================
    // Phase 1 Helper Functions (from ORB)
    // ==========================================
    void on_market_open(StockState& state, const MDStockStruct& stock, const std::string& symbol) {
        // Reset state for new trading day
        state.state = State::IDLE;
        state.open_price = 0.0;
        state.prev_close = 0.0;
        state.highest_price = 0.0;
        state.highest_timestamp_mdtime = 0;
        state.locked_breakout_price = 0;
        state.window_.clear();
        state.signal_triggered = false;
        state.current_date = stock.mddate;

        LOG_M_INFO("{} New trading day: {}", symbol, state.current_date);
    }

    static bool is_market_open(int32_t mdtime) {
        int time_hhmm = mdtime / 100000;
        return (time_hhmm >= 930 && time_hhmm < 1130) ||   // Morning session
               (time_hhmm >= 1300 && time_hhmm < 1500);    // Afternoon session
    }

    static int64_t get_time_since_open_ms(int32_t mdtime) {
        int64_t time_ms = mdtime;
        int64_t morning_open = 93000000;  // 09:30:00.000

        if (time_ms >= morning_open && time_ms < 113000000) {
            return time_ms - morning_open;
        } else if (time_ms >= 130000000) {
            // Afternoon: add morning duration + afternoon elapsed
            return (113000000 - morning_open) + (time_ms - 130000000);
        }
        return 0;
    }

    static double to_price(int64_t price_int) {
        return price_int / 10000.0;
    }

    static std::string get_symbol(const MDStockStruct& stock) {
        return std::string(stock.htscsecurityid);
    }

    static std::string get_symbol_from_order(const MDOrderStruct& order) {
        return std::string(order.htscsecurityid);
    }

    static std::string get_symbol_from_transaction(const MDTransactionStruct& txn) {
        return std::string(txn.htscsecurityid);
    }

    // ==========================================
    // Phase 2 Helper Functions (from PLV)
    // ==========================================
    uint64_t get_volume_at_locked_price(const MDStockStruct& stock, const StockState& state) {
        // For tick events, we need to manually calculate volume from sellpricequeue/sellqtyqueue
        // However, this is approximate. For accurate volume, rely on order/transaction events.
        // Return 0 for tick events as we primarily rely on order/transaction callbacks
        return 0;
    }

    void add_to_window(StockState& state, int32_t mdtime, uint64_t volume, uint64_t buy_trade_qty) {
        // Add new data point
        state.window_.emplace_back(mdtime, volume, buy_trade_qty);

        // Remove data older than 200ms
        while (!state.window_.empty()) {
            if (is_within_200ms(state.window_.front().mdtime, mdtime)) {
                break;  // Still within window
            }
            state.window_.pop_front();
        }
    }

    void check_trigger_condition(StockState& state, const std::string& symbol, int32_t mdtime) {
        if (state.signal_triggered || state.window_.empty()) {
            return;
        }

        // Calculate n: average volume in window
        uint64_t total_volume = 0;
        for (const auto& snapshot : state.window_) {
            total_volume += snapshot.volume;
        }
        double n = static_cast<double>(total_volume) / state.window_.size();

        // Safety check: if n=0 (no orders at breakout price), don't trigger
        if (n < 1.0) {
            return;
        }

        // Calculate delta_n: cumulative sum of buy trades in window
        uint64_t delta_n = 0;
        for (const auto& snapshot : state.window_) {
            delta_n += snapshot.buy_trade_qty;
        }

        // Check trigger condition: delta_n >= n
        if (delta_n >= static_cast<uint64_t>(n)) {
            state.signal_triggered = true;
            state.state = State::SIGNAL_TRIGGERED;

            // Log buy signal
            LOG_BIZ("SIGNAL",
                    "BUY SIGNAL | {} | Time={} | "
                    "Breakout_Price={:.4f}({}) | "
                    "n(avg_volume)={:.0f} | delta_n(buy_trades)={} | "
                    "window_size={} | open={:.4f} | prev_close={:.4f}",
                    symbol,
                    format_mdtime(mdtime),
                    state.locked_breakout_price / 10000.0,
                    state.locked_breakout_price,
                    n,
                    delta_n,
                    state.window_.size(),
                    state.open_price,
                    state.prev_close);

            LOG_M_INFO("========================================");
            LOG_M_INFO("SIGNAL TRIGGERED | {}", symbol);
            LOG_M_INFO("Time: {}", format_mdtime(mdtime));
            LOG_M_INFO("Breakout Price: {:.4f} ({})",
                       state.locked_breakout_price / 10000.0,
                       state.locked_breakout_price);
            LOG_M_INFO("Open: {:.4f} | Prev Close: {:.4f}", state.open_price, state.prev_close);
            LOG_M_INFO("n (Avg Volume in 200ms): {:.0f}", n);
            LOG_M_INFO("delta_n (Buy Trades in 200ms): {}", delta_n);
            LOG_M_INFO("Window Size: {} snapshots", state.window_.size());
            LOG_M_INFO("========================================");
        }
    }

    bool is_within_200ms(int32_t time1, int32_t time2) const {
        int64_t diff = calculate_time_diff_ms(time1, time2);
        return (diff >= 0 && diff <= 200);
    }

    int64_t calculate_time_diff_ms(int32_t time1, int32_t time2) const {
        // MDTime format: HHMMSSMMM (9 digits)
        // Example: 093015500 = 09:30:15.500

        // Extract hours, minutes, seconds, milliseconds
        int32_t h1 = time1 / 10000000;
        int32_t m1 = (time1 / 100000) % 100;
        int32_t s1 = (time1 / 1000) % 100;
        int32_t ms1 = time1 % 1000;

        int32_t h2 = time2 / 10000000;
        int32_t m2 = (time2 / 100000) % 100;
        int32_t s2 = (time2 / 1000) % 100;
        int32_t ms2 = time2 % 1000;

        // Convert to total milliseconds
        int64_t total1 = (h1 * 3600000LL) + (m1 * 60000LL) + (s1 * 1000LL) + ms1;
        int64_t total2 = (h2 * 3600000LL) + (m2 * 60000LL) + (s2 * 1000LL) + ms2;

        return total2 - total1;
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
#endif // GAP_UP_VOLUME_BREAKOUT_STRATEGY_H
