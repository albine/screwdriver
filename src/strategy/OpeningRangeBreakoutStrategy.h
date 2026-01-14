#ifndef OPENING_RANGE_BREAKOUT_STRATEGY_H
#define OPENING_RANGE_BREAKOUT_STRATEGY_H

#include "strategy_engine.h"
#include "market_data_structs.h"
#include "logger.h"
#include <map>
#include <string>
#include <cstdio>
#include <cmath>

#define LOG_MODULE MOD_STRATEGY

/**
 * Opening Range Breakout Strategy - Adapted for current trading engine
 *
 * Gap Down Scenario: Buy when price recovers above previous close
 * Gap Up Scenario: Buy on breakout after consolidation period
 */
class OpeningRangeBreakoutStrategy : public Strategy {
private:
    // Helper functions for data conversion
    static double to_price(int64_t price_int) {
        return price_int / 10000.0;
    }

    static std::string get_symbol(const MDStockStruct& stock) {
        return std::string(stock.htscsecurityid);
    }

    static std::string format_mdtime(int32_t mdtime) {
        int hours = mdtime / 10000000;
        int minutes = (mdtime / 100000) % 100;
        int seconds = (mdtime / 1000) % 100;
        int millis = mdtime % 1000;

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                 hours, minutes, seconds, millis);
        return std::string(buffer);
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
    static constexpr int64_t THIRTY_SECONDS_MS = 30000000;  // 30 seconds in mdtime format
    static constexpr int64_t TEN_MINUTES_MS = 600000000;     // 10 minutes in mdtime format
    static constexpr int THRESHOLD_60_00_CAP = 700;          // 7% in basis points
    static constexpr int THRESHOLD_30_68_CAP = 1700;         // 17% in basis points

    struct ORBState {
        // Track highest price and consolidation
        double highest_price = 0.0;
        int64_t highest_timestamp_mdtime = 0;  // Store in mdtime format

        // Thresholds
        int cap_threshold_bp = 0;

        // Signal flags
        bool buy_signal_triggered = false;

        // Track market open for initialization
        int32_t current_date = 0;  // Track date to detect new trading day
    };

    std::map<std::string, ORBState> stock_states_;

public:
    explicit OpeningRangeBreakoutStrategy(const std::string& strategy_name) {
        this->name = strategy_name;
    }

    virtual ~OpeningRangeBreakoutStrategy() = default;

private:
    void OnMarketOpen(ORBState& state, const MDStockStruct& stock, const std::string& symbol) {
        state.highest_price = to_price(stock.highpx);
        state.highest_timestamp_mdtime = stock.mdtime;
        state.buy_signal_triggered = false;
        state.current_date = stock.mddate;

        initializeThresholds(state, symbol);

        LOG_M_INFO("Market open for {}: highest_price={:.4f}, date={}",
                   symbol, state.highest_price, state.current_date);
    }

public:
    void on_tick(const MDStockStruct& stock) override {
        // Extract symbol once
        std::string symbol = get_symbol(stock);

        // Check if market is open
        if (!is_market_open(stock.mdtime)) {
            return;
        }

        auto& stock_state = stock_states_[symbol];

        // Detect new trading day and initialize
        if (stock_state.current_date != stock.mddate) {
            OnMarketOpen(stock_state, stock, symbol);
        }

        // Skip if signal already triggered
        if (stock_state.buy_signal_triggered) {
            return;
        }

        // Only process within first 10 minutes of trading
        int64_t time_since_open = get_time_since_open_ms(stock.mdtime);
        if (time_since_open > TEN_MINUTES_MS) {
            return;  // Window closed
        }

        // Convert prices
        double high_price = to_price(stock.highpx);
        double prev_close = to_price(stock.preclosepx);
        double open_price = to_price(stock.openpx);

        // Calculate percentage gain from high to prev_close
        int highest_gain_bp = calculate_percentage_bp(high_price, prev_close);
        if (highest_gain_bp >= stock_state.cap_threshold_bp) {
            LOG_M_DEBUG("{} 最高涨幅超过阈值 {}bp (threshold={}bp). [弱转强新高] 取消.",
                        symbol, highest_gain_bp, stock_state.cap_threshold_bp);
            return;
        }

        // Process based on gap scenario
        if (open_price < prev_close) {
            processGapDownScenario(stock, stock_state, symbol);
        } else if (open_price >= prev_close) {
            processGapUpScenario(stock, stock_state, symbol);
        }

        // Update highest price if within 30 seconds of previous high
        int64_t consolidation_duration = stock.mdtime - stock_state.highest_timestamp_mdtime;

        if (consolidation_duration >= THIRTY_SECONDS_MS) {
            LOG_M_DEBUG("{} 距上一个新高已超过30秒，不再刷新新高", symbol);
        } else {
            if (high_price > stock_state.highest_price) {
                stock_state.highest_price = high_price;
                stock_state.highest_timestamp_mdtime = stock.mdtime;
            }
        }
    }

private:

    void initializeThresholds(ORBState& stock_state, const std::string& symbol) const {
        if (symbol.size() < 2) {
            return; // Default
        }
        
        std::string prefix = symbol.substr(0, 2);
        
        if (prefix == "60" || prefix == "00") {
            stock_state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        } else if (prefix == "30" || prefix == "68") {
            stock_state.cap_threshold_bp = THRESHOLD_30_68_CAP;
        } else {
            stock_state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        }
    }

    void processGapDownScenario(const MDStockStruct& stock,
                               ORBState& stock_state,
                               const std::string& symbol) {
        // Buy when price recovers above previous close
        double sell_price_01 = to_price(stock.sellpricequeue[0]);
        double prev_close = to_price(stock.preclosepx);
        double open_price = to_price(stock.openpx);
        double last_price = to_price(stock.lastpx);

        if (sell_price_01 > prev_close) {
            double order_price = last_price * 1.017;

            LOG_BIZ("SIGNAL",
                    "BUY | {} | Time={} | Price={:.4f} | Open={:.4f} | "
                    "PrevClose={:.4f} | Reason=弱转强绿开翻红",
                    symbol, format_mdtime(stock.mdtime), order_price,
                    open_price, prev_close);

            stock_state.buy_signal_triggered = true;
        }
    }
    
    void processGapUpScenario(const MDStockStruct& stock,
                             ORBState& stock_state,
                             const std::string& symbol) {
        // Must be at least 30 seconds after highest price
        int64_t consolidation_duration = stock.mdtime - stock_state.highest_timestamp_mdtime;
        double sell_price_01 = to_price(stock.sellpricequeue[0]);
        bool breaks_previous_high = sell_price_01 > stock_state.highest_price;

        if (breaks_previous_high && consolidation_duration >= THIRTY_SECONDS_MS) {
            double open_price = to_price(stock.openpx);
            double prev_close = to_price(stock.preclosepx);
            double last_price = to_price(stock.lastpx);
            double order_price = last_price * 1.017;

            int gap_up_bp = calculate_percentage_bp(open_price, prev_close);
            double consolidation_seconds = consolidation_duration / 1000000.0;  // Convert to seconds

            LOG_BIZ("SIGNAL",
                    "BUY | {} | Time={} | Price={:.4f} | Open={:.4f} | "
                    "PrevClose={:.4f} | PrevHigh={:.4f} | GapUp={:.2f}% | "
                    "Consolidation={:.1f}s | Reason=弱转强高开新高",
                    symbol, format_mdtime(stock.mdtime), order_price,
                    open_price, prev_close, stock_state.highest_price,
                    gap_up_bp / 100.0, consolidation_seconds);

            stock_state.buy_signal_triggered = true;
        }
    }

};

#endif // OPENING_RANGE_BREAKOUT_STRATEGY_H