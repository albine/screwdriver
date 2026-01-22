#ifndef HENGGOU_STRATEGY_H
#define HENGGOU_STRATEGY_H

#include "strategy_base.h"
#include "market_data_structs.h"
#include "trade_signal.h"
#include "logger.h"
#include "breakout_detector.h"
#include "utils/time_util.h"
#include "utils/price_util.h"
#include <map>
#include <deque>
#include <string>

#define LOG_MODULE MOD_STRATEGY

/**
 * HenggouStrategy (横沟策略)
 *
 * 检测60秒内快速上涨后的横盘整理突破。
 *
 * 策略逻辑：
 * - Phase 1: 60秒滑动窗口检测涨速 > 3%
 * - Phase 2: 等待27秒整理期（无新高），且最高价 > 前收盘价
 * - Phase 3: 使用 BreakoutDetector 检测突破 highest_price
 *
 * 限制条件：
 * - 开盘10分钟内有效
 * - 整理期不超过3分钟，否则过期
 * - 涨幅上限 7%/17%
 */
class HenggouStrategy : public Strategy {
private:
    // Helper functions for data conversion
    static double to_price(int64_t price_int) {
        return price_int / 10000.0;
    }

    static uint32_t to_price_int(double price) {
        return static_cast<uint32_t>(price * 10000);
    }

    static std::string get_symbol(const MDStockStruct& stock) {
        return std::string(stock.htscsecurityid);
    }

    static bool is_market_open(int32_t mdtime) {
        int time_hhmm = mdtime / 100000;
        return (time_hhmm >= 930 && time_hhmm < 1130) ||  // Morning session
               (time_hhmm >= 1300 && time_hhmm < 1500);   // Afternoon session
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

    static int calculate_percentage_bp(double price, double base_price) {
        if (base_price <= 0.0) return 0;
        return static_cast<int>(((price - base_price) / base_price) * 10000);
    }

    // Constants
    // MDTime format: HHMMSSmmm
    // 1 second = 1000, 1 minute = 100000 (NOT 60000!)
    static constexpr int GAIN_THRESHOLD_BP = 300;                          // 3% 涨速阈值
    static constexpr int64_t GAIN_WINDOW_MS = 90000;                       // 90秒滑动窗口
    static constexpr int64_t CONSOLIDATION_HOLD_TIME_MS = 27000;           // 27秒整理期
    static constexpr int64_t CONSOLIDATION_UPPER_LIMIT_MS = 180000;        // 3分钟横沟超时
    static constexpr int64_t STRATEGY_DISABLE_TIME_MS = 1000000;           // 10分钟策略有效期

    static constexpr int THRESHOLD_60_00_CAP = 700;   // 7%
    static constexpr int THRESHOLD_30_68_CAP = 1700;  // 17%

    // Price record for sliding window
    struct PriceRecord {
        double price;
        int32_t mdtime;
    };

    // Strategy state per symbol
    struct HGState {
        // Phase tracking
        bool wait_for_henggou = false;     // Phase 1 完成标志
        bool consolidation_met = false;    // Phase 2 完成标志
        bool buy_signal_triggered = false;
        bool expired = false;              // 横沟超时标志

        // Price tracking
        double highest_price = 0.0;
        int32_t highest_timestamp_mdtime = 0;

        // 60秒滑动窗口
        std::deque<PriceRecord> price_window;

        // Thresholds
        int cap_threshold_bp = 700;  // 7% or 17%

        // Breakout detector
        BreakoutDetector breakout_detector;
        bool detector_armed = false;

        // Cached prices
        double prev_close = 0.0;
        double open_price = 0.0;
        uint32_t limit_up_price = 0;

        // Date tracking
        int32_t current_date = 0;
    };

    std::map<std::string, HGState> stock_states_;

public:
    explicit HenggouStrategy(const std::string& strategy_name,
                             const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;
    }

    virtual ~HenggouStrategy() = default;

    void on_start() override {}
    void on_stop() override {}

private:
    void OnMarketOpen(HGState& state, const MDStockStruct& stock, const std::string& symbol) {
        state.highest_price = to_price(stock.highpx);
        state.highest_timestamp_mdtime = stock.mdtime;
        state.wait_for_henggou = false;
        state.consolidation_met = false;
        state.buy_signal_triggered = false;
        state.expired = false;
        state.current_date = stock.mddate;
        state.detector_armed = false;
        state.breakout_detector.reset();
        state.breakout_detector.set_enabled(false);

        // Clear sliding window and add first record
        state.price_window.clear();
        double current_price = to_price(stock.lastpx);
        state.price_window.push_back({current_price, stock.mdtime});

        // Cache prices
        state.prev_close = to_price(stock.preclosepx);
        state.open_price = to_price(stock.openpx);
        state.limit_up_price = static_cast<uint32_t>(stock.maxpx);

        // Initialize cap threshold
        initializeThresholds(state, symbol);

        LOG_M_INFO("{} 开盘: prev_close={:.4f}, open={:.4f}",
                   symbol, state.prev_close, state.open_price);
    }

    void initializeThresholds(HGState& state, const std::string& symbol) const {
        if (symbol.size() < 2) return;

        std::string prefix = symbol.substr(0, 2);

        if (prefix == "60" || prefix == "00") {
            state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        } else if (prefix == "30" || prefix == "68") {
            state.cap_threshold_bp = THRESHOLD_30_68_CAP;
        } else {
            state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        }
    }

    // Update 60-second sliding window
    void updatePriceWindow(HGState& state, double current_price, int32_t current_mdtime) {
        // Remove records older than 60 seconds
        while (!state.price_window.empty()) {
            // 使用正确的时间差计算（处理跨分钟问题）
            int64_t time_diff = time_util::calculate_time_diff_ms(
                state.price_window.front().mdtime, current_mdtime);
            if (time_diff > GAIN_WINDOW_MS) {
                state.price_window.pop_front();
            } else {
                break;
            }
        }

        // Add current price
        state.price_window.push_back({current_price, current_mdtime});
    }

    // Get lowest price in sliding window
    PriceRecord getWindowLowest(const HGState& state) const {
        PriceRecord lowest = state.price_window.front();
        for (const auto& record : state.price_window) {
            if (record.price < lowest.price) {
                lowest = record;
            }
        }
        return lowest;
    }

public:
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;

        std::string symbol = get_symbol(stock);

        // Check if market is open
        if (!is_market_open(stock.mdtime)) {
            return;
        }

        auto& state = stock_states_[symbol];

        // Detect new trading day and initialize
        if (state.current_date != stock.mddate) {
            OnMarketOpen(state, stock, symbol);
        }

        // Skip if signal already triggered
        if (state.buy_signal_triggered) {
            return;
        }

        // Update limit up price
        if (stock.maxpx > 0) {
            state.limit_up_price = static_cast<uint32_t>(stock.maxpx);
        }

        // Only process within first 10 minutes of trading
        int64_t time_since_open = get_time_since_open_ms(stock.mdtime);
        if (time_since_open > STRATEGY_DISABLE_TIME_MS) {
            return;  // Window closed
        }

        // Check if expired (consolidation > 3 minutes)
        if (state.expired) {
            return;
        }

        // Convert prices
        double high_price = to_price(stock.highpx);
        double current_price = to_price(stock.lastpx);
        double prev_close = state.prev_close;

        // Check cap threshold (7%/17%)
        int highest_gain_bp = calculate_percentage_bp(high_price, prev_close);
        if (highest_gain_bp >= state.cap_threshold_bp) {
            LOG_M_DEBUG("{} 最高涨幅{}bp超过阈值{}bp，取消追踪",
                        symbol, highest_gain_bp, state.cap_threshold_bp);
            return;
        }

        // Update sliding window
        updatePriceWindow(state, current_price, stock.mdtime);

        // Phase 1: Check gain speed (current price vs window lowest > 3%)
        if (!state.wait_for_henggou) {
            checkGainSpeed(stock, state, symbol);
        }
        // Phase 2: Check consolidation period
        else if (!state.consolidation_met) {
            checkConsolidation(stock, state, symbol);
        }
        // Phase 3: Breakout detector is armed, handled in on_order/on_transaction

        // Update highest price (only after Phase 1 is complete)
        updateHighestPrice(stock, state, symbol);
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string symbol(order.htscsecurityid);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.buy_signal_triggered || !state.detector_armed || state.expired) return;

        // Check expiration in Phase 3 (consolidation > 3 minutes from highest price time)
        int64_t time_since_highest = time_util::calculate_time_diff_ms(
            state.highest_timestamp_mdtime, order.mdtime);
        if (time_since_highest > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_order(order, book)) {
            onBreakoutTriggered(state, symbol, order.mdtime);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string symbol(txn.htscsecurityid);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.buy_signal_triggered || !state.detector_armed || state.expired) return;

        // Check expiration in Phase 3 (consolidation > 3 minutes from highest price time)
        int64_t time_since_highest = time_util::calculate_time_diff_ms(
            state.highest_timestamp_mdtime, txn.mdtime);
        if (time_since_highest > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_transaction(txn, book)) {
            onBreakoutTriggered(state, symbol, txn.mdtime);
        }
    }

private:
    // Phase 1: Check gain speed (current price vs window lowest > 3%)
    void checkGainSpeed(const MDStockStruct& stock, HGState& state, const std::string& symbol) {
        if (state.price_window.empty()) return;

        double current_price = to_price(stock.lastpx);
        PriceRecord lowest = getWindowLowest(state);
        int gain_bp = calculate_percentage_bp(current_price, lowest.price);

        LOG_M_DEBUG("[横沟] {} 60秒窗口最低价={:.4f} 现价={:.4f} 涨幅={}bp 窗口大小={}",
                    symbol, lowest.price, current_price, gain_bp, state.price_window.size());

        if (gain_bp > GAIN_THRESHOLD_BP) {
            state.wait_for_henggou = true;
            // Initialize highest price tracking when entering Phase 2
            state.highest_price = to_price(stock.highpx);
            state.highest_timestamp_mdtime = stock.mdtime;
            LOG_M_INFO("{} Phase1完成: 60秒内涨幅{}bp > {}bp，等待横沟",
                       symbol, gain_bp, GAIN_THRESHOLD_BP);
        }
    }

    // Phase 2: Check consolidation period (27 seconds since last high, highest > prev_close)
    void checkConsolidation(const MDStockStruct& stock, HGState& state, const std::string& symbol) {
        // 使用正确的时间差计算（处理跨分钟问题）
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_timestamp_mdtime, stock.mdtime);

        // Check expiration (consolidation > 3 minutes)
        if (consolidation_duration > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            LOG_M_INFO("{} 横沟超时({}秒 > 180秒)，策略取消", symbol, consolidation_duration / 1000);
            return;
        }

        // Must be red (highest > prev_close)
        bool is_red = state.highest_price > state.prev_close;
        if (!is_red) {
            LOG_M_DEBUG("{} 最高价{:.4f} <= 前收{:.4f}，不满足红盘条件",
                        symbol, state.highest_price, state.prev_close);
            return;
        }

        // Check if consolidation period is met (27 seconds)
        if (consolidation_duration >= CONSOLIDATION_HOLD_TIME_MS) {
            state.consolidation_met = true;

            // Arm the breakout detector with highest_price as target
            uint32_t target_price = to_price_int(state.highest_price);

            state.breakout_detector.set_target_price(target_price);
            state.breakout_detector.set_enabled(true);
            state.detector_armed = true;

            int64_t consolidation_seconds = consolidation_duration / 1000;
            LOG_M_INFO("{} Phase2完成: 顿点{}秒, 目标价={} ({}元), highest={:.4f}",
                       symbol, consolidation_seconds,
                       target_price, price_util::price_to_yuan(target_price),
                       state.highest_price);
        }
    }

    // Update highest price tracking (only in Phase 2, before consolidation is met)
    void updateHighestPrice(const MDStockStruct& stock, HGState& state, const std::string& symbol) {
        // Only track after Phase 1 is complete
        if (!state.wait_for_henggou) {
            return;
        }

        // Don't update after consolidation is met (highest price is locked)
        if (state.consolidation_met) {
            return;
        }

        // Check if still within consolidation window (27 seconds)
        // 使用正确的时间差计算（处理跨分钟问题）
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_timestamp_mdtime, stock.mdtime);
        if (consolidation_duration >= CONSOLIDATION_HOLD_TIME_MS) {
            // Consolidation period reached, don't update highest anymore
            return;
        }

        double high_price = to_price(stock.highpx);
        if (high_price > state.highest_price) {
            state.highest_price = high_price;
            state.highest_timestamp_mdtime = stock.mdtime;
            LOG_M_DEBUG("{} 新高: {:.4f}", symbol, high_price);
        }
    }

    // Breakout triggered callback
    void onBreakoutTriggered(HGState& state, const std::string& symbol, int32_t mdtime) {
        state.buy_signal_triggered = true;

        uint32_t target_price = state.breakout_detector.get_target_price();
        auto stats = state.breakout_detector.get_stats();

        // Calculate order price = min(target_price * 1.017, limit_up_price)
        uint32_t order_price = static_cast<uint32_t>(target_price * 1.017);
        if (state.limit_up_price > 0 && order_price > state.limit_up_price) {
            order_price = state.limit_up_price;
        }

        if (stats.current_volume == 0) {
            // Direct breakout (target_price < best_ask)
            LOG_BIZ("SIGNAL",
                    "BUY | {} | Time={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | Reason=横沟突破 (直接突破)",
                    symbol, time_util::format_mdtime(mdtime),
                    price_util::format_price_display(order_price),
                    state.open_price, state.prev_close);
        } else {
            // Order book dynamics triggered
            LOG_BIZ("SIGNAL",
                    "BUY | {} | Time={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | n={:.0f} | delta_n={} | Reason=横沟突破 (动力学触发)",
                    symbol, time_util::format_mdtime(mdtime),
                    price_util::format_price_display(order_price),
                    state.open_price, state.prev_close,
                    stats.avg_volume, stats.total_buy_qty);
        }

        LOG_M_INFO("========================================");
        LOG_M_INFO("SIGNAL TRIGGERED at {} for {}", time_util::format_mdtime(mdtime), symbol);
        LOG_M_INFO("Scenario: 横沟突破");
        LOG_M_INFO("Target Price: {} ({}元)", target_price, price_util::price_to_yuan(target_price));
        LOG_M_INFO("Order Price: {} ({}元)", order_price, price_util::price_to_yuan(order_price));
        if (stats.current_volume > 0) {
            LOG_M_INFO("n (Avg Volume): {:.0f}, delta_n (Buy Trades): {}", stats.avg_volume, stats.total_buy_qty);
        }
        LOG_M_INFO("========================================");

        // 发送买入信号
        TradeSignal signal;
        signal.symbol = symbol;
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
#endif // HENGGOU_STRATEGY_H
