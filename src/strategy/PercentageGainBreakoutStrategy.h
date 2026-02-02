#ifndef PERCENTAGE_GAIN_BREAKOUT_STRATEGY_H
#define PERCENTAGE_GAIN_BREAKOUT_STRATEGY_H

#include "strategy_base.h"
#include "market_data_structs_aligned.h"
#include "trade_signal.h"
#include "logger.h"
#include "breakout_detector.h"
#include "utils/time_util.h"
#include "utils/price_util.h"
#include <map>
#include <string>

#define LOG_MODULE MOD_STRATEGY

/**
 * PercentageGainBreakoutStrategy (追4/6新高策略)
 *
 * 基于涨幅阈值的突破策略，使用盘口动力学检测器提高触发精度。
 *
 * 策略逻辑：
 * - Phase 1: 等待涨幅达到初始阈值 (60/00: 3%, 30/68: 5%)
 * - Phase 2: 等待 27 秒整理期（距离最高点超过 27 秒无新高）
 * - Phase 3: 使用 BreakoutDetector 检测突破（目标价 = max(4%/6%阈值价, 整理期最高价)）
 *
 * GapScenario 开盘跳空场景：
 * - NORMAL: 开盘涨幅 < 3%/5%，正常等待 Phase 1
 * - MODERATE_GAP: 开盘涨幅在 [3%/5%, 4%/6%) 区间，直接进入 Phase 2
 * - LARGE_GAP: 开盘涨幅 >= 4%/6%，直接进入 Phase 2，等待新高突破
 *
 * 涨幅上限：
 * - 60/00 开头股票: 7%
 * - 30/68 开头股票: 17%
 * - 超过上限后放弃该股票
 */
class PercentageGainBreakoutStrategy : public Strategy {
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
        // 使用 time_util 正确计算时间差（MDTime 不能直接做减法）
        constexpr int32_t morning_open = 93000000;  // 09:30:00.000
        return time_util::calculate_time_diff_ms(morning_open, mdtime);
    }

    static int calculate_percentage_bp(double price, double base_price) {
        if (base_price <= 0.0) return 0;
        return static_cast<int>(((price - base_price) / base_price) * 10000);
    }

    // Constants
    // 时间常量（真实毫秒）
    static constexpr int64_t CONSOLIDATION_HOLD_TIME_MS = 27 * 1000;  // 27 秒
    static constexpr int64_t TEN_MINUTES_MS = 10 * 60 * 1000;         // 5 分钟

    // Thresholds in basis points
    static constexpr int THRESHOLD_60_00_INITIAL = 300;   // 3%
    static constexpr int THRESHOLD_30_68_INITIAL = 500;   // 5%
    static constexpr int THRESHOLD_60_00_BREAKOUT = 400;  // 4%
    static constexpr int THRESHOLD_30_68_BREAKOUT = 600;  // 6%
    static constexpr int THRESHOLD_60_00_CAP = 700;       // 7%
    static constexpr int THRESHOLD_30_68_CAP = 1700;      // 17%

    // Gap scenario types
    enum class GapScenario {
        NORMAL,        // 开盘涨幅 < initial_threshold (3%/5%)
        MODERATE_GAP,  // 开盘涨幅在 [initial, breakout) 区间
        LARGE_GAP      // 开盘涨幅 >= breakout_threshold (4%/6%)
    };

    // Strategy state per symbol
    struct PGBState {
        // Phase tracking
        bool initial_threshold_met = false;
        bool consolidation_period_met = false;
        bool buy_signal_triggered = false;

        // Price tracking
        double highest_price = 0.0;
        int64_t highest_timestamp_mdtime = 0;

        // Thresholds (basis points)
        int initial_threshold_bp = 300;   // 3% or 5%
        int breakout_threshold_bp = 400;  // 4% or 6%
        int cap_threshold_bp = 700;       // 7% or 17%

        // Gap scenario
        GapScenario gap_scenario = GapScenario::NORMAL;

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

    std::map<std::string, PGBState> stock_states_;

public:
    explicit PercentageGainBreakoutStrategy(const std::string& strategy_name,
                                            const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;
    }

    virtual ~PercentageGainBreakoutStrategy() = default;

    void on_start() override {}
    void on_stop() override {}

private:
    void OnMarketOpen(PGBState& state, const MDStockStruct& stock, const std::string& symbol) {
        state.highest_price = to_price(stock.highpx);
        state.highest_timestamp_mdtime = stock.mdtime;
        state.initial_threshold_met = false;
        state.consolidation_period_met = false;
        state.buy_signal_triggered = false;
        state.current_date = stock.mddate;
        state.detector_armed = false;
        state.breakout_detector.reset();
        state.breakout_detector.set_enabled(false);

        // Cache prices
        state.prev_close = to_price(stock.preclosepx);
        state.open_price = to_price(stock.openpx);
        state.limit_up_price = static_cast<uint32_t>(stock.maxpx);

        // Initialize thresholds based on stock code prefix
        initializeThresholds(state, symbol);

        // Determine gap scenario based on opening price
        int opening_gain_bp = calculate_percentage_bp(state.open_price, state.prev_close);
        state.gap_scenario = determineGapScenario(opening_gain_bp,
                                                   state.initial_threshold_bp,
                                                   state.breakout_threshold_bp);

        LOG_M_DEBUG("{} 开盘: prev_close={:.4f}, open={:.4f}, 开盘涨幅={:.2f}%, gap_scenario={}",
                   symbol, state.prev_close, state.open_price,
                   opening_gain_bp / 100.0, gapScenarioToString(state.gap_scenario));

        // Handle gap scenarios
        if (state.gap_scenario == GapScenario::MODERATE_GAP) {
            state.initial_threshold_met = true;
            LOG_M_DEBUG("{} 开盘已达3/5%阈值，等待顿点后追4/6", symbol);
        } else if (state.gap_scenario == GapScenario::LARGE_GAP) {
            state.initial_threshold_met = true;
            LOG_M_DEBUG("{} 开盘已超4/6%阈值，等待顿点后追新高", symbol);
        }
    }

    void initializeThresholds(PGBState& state, const std::string& symbol) const {
        if (symbol.size() < 2) return;

        std::string prefix = symbol.substr(0, 2);

        if (prefix == "60" || prefix == "00") {
            state.initial_threshold_bp = THRESHOLD_60_00_INITIAL;
            state.breakout_threshold_bp = THRESHOLD_60_00_BREAKOUT;
            state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        } else if (prefix == "30" || prefix == "68") {
            state.initial_threshold_bp = THRESHOLD_30_68_INITIAL;
            state.breakout_threshold_bp = THRESHOLD_30_68_BREAKOUT;
            state.cap_threshold_bp = THRESHOLD_30_68_CAP;
        } else {
            state.initial_threshold_bp = THRESHOLD_60_00_INITIAL;
            state.breakout_threshold_bp = THRESHOLD_60_00_BREAKOUT;
            state.cap_threshold_bp = THRESHOLD_60_00_CAP;
        }
    }

    GapScenario determineGapScenario(int opening_gain_bp,
                                     int initial_threshold_bp,
                                     int breakout_threshold_bp) const {
        if (opening_gain_bp < initial_threshold_bp) {
            return GapScenario::NORMAL;
        } else if (opening_gain_bp < breakout_threshold_bp) {
            return GapScenario::MODERATE_GAP;
        } else {
            return GapScenario::LARGE_GAP;
        }
    }

    const char* gapScenarioToString(GapScenario scenario) const {
        switch (scenario) {
            case GapScenario::NORMAL:       return "NORMAL";
            case GapScenario::MODERATE_GAP: return "MODERATE_GAP";
            case GapScenario::LARGE_GAP:    return "LARGE_GAP";
            default:                        return "UNKNOWN";
        }
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
        if (time_since_open > TEN_MINUTES_MS) {
            return;  // Window closed
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

        // Phase 1: Check if initial threshold is met
        if (!state.initial_threshold_met) {
            checkInitialThreshold(stock, state, symbol);
        }
        // Phase 2: Check consolidation period
        else if (!state.consolidation_period_met) {
            checkConsolidation(stock, state, symbol);
        }
        // Phase 3: Breakout detector is armed, use tick data as fallback trigger
        else if (state.detector_armed) {
            // 用 tick 数据保底检测突破
            if (state.breakout_detector.on_tick(stock)) {
                // onBreakoutTriggered(state, symbol, stock.mdtime, stock.local_recv_timestamp);
                LOG_M_INFO("[PGB] on_tick breakout detected, symbol={}, mdtime={}", symbol, stock.mdtime);
            }
        }

        // Update highest price if not yet in consolidation
        updateHighestPrice(stock, state, symbol);
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string symbol(order.htscsecurityid);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.buy_signal_triggered || !state.detector_armed) return;

        // Check 10-minute time limit in Phase 3
        int64_t time_since_open = get_time_since_open_ms(order.mdtime);
        if (time_since_open > TEN_MINUTES_MS) {
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_order(order, book)) {
            onBreakoutTriggered(state, symbol, order.mdtime, order.local_recv_timestamp);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string symbol(txn.htscsecurityid);
        auto it = stock_states_.find(symbol);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.buy_signal_triggered || !state.detector_armed) return;

        // Check 10-minute time limit in Phase 3
        int64_t time_since_open = get_time_since_open_ms(txn.mdtime);
        if (time_since_open > TEN_MINUTES_MS) {
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_transaction(txn, book)) {
            onBreakoutTriggered(state, symbol, txn.mdtime, txn.local_recv_timestamp);
        }
    }

private:
    // Phase 1: Check if initial threshold is met
    void checkInitialThreshold(const MDStockStruct& stock, PGBState& state, const std::string& symbol) {
        double current_price = to_price(stock.lastpx);
        int current_gain_bp = calculate_percentage_bp(current_price, state.prev_close);

        if (current_gain_bp >= state.initial_threshold_bp) {
            state.initial_threshold_met = true;
            LOG_M_DEBUG("{} Phase1完成: 涨幅{:.2f}%达到阈值{:.2f}%, mdtime={}",
                       symbol, current_gain_bp / 100.0, state.initial_threshold_bp / 100.0,
                       time_util::format_mdtime(stock.mdtime));
        }
    }

    // Phase 2: Check consolidation period (27 seconds since last high)
    void checkConsolidation(const MDStockStruct& stock, PGBState& state, const std::string& symbol) {
        // 使用 time_util 正确计算时间差（MDTime 不能直接做减法）
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_timestamp_mdtime, stock.mdtime);

        if (consolidation_duration >= CONSOLIDATION_HOLD_TIME_MS) {
            state.consolidation_period_met = true;

            // Calculate target price: max(breakout_threshold_price, highest_price)
            double breakout_threshold_price = state.prev_close * (1.0 + state.breakout_threshold_bp / 10000.0);
            double target_price_double = std::max(breakout_threshold_price, state.highest_price);
            uint32_t target_price = to_price_int(target_price_double);

            // Arm the breakout detector
            state.breakout_detector.set_target_price(target_price);
            state.breakout_detector.set_enabled(true);
            state.detector_armed = true;

            int64_t consolidation_seconds = consolidation_duration / 1000;
            LOG_M_DEBUG("{} Phase2完成: 顿点{}秒, 目标价={} ({}元), highest={:.4f}, highest_time={}, mdtime={}",
                       symbol, consolidation_seconds,
                       target_price, price_util::price_to_yuan(target_price),
                       state.highest_price, time_util::format_mdtime(state.highest_timestamp_mdtime),
                       time_util::format_mdtime(stock.mdtime));
        }
    }

    // Update highest price tracking
    void updateHighestPrice(const MDStockStruct& stock, PGBState& state, const std::string& symbol) {
        // Don't update after consolidation is met (highest price is locked)
        if (state.consolidation_period_met) {
            return;
        }

        double high_price = to_price(stock.highpx);
        if (high_price > state.highest_price) {
            state.highest_price = high_price;
            state.highest_timestamp_mdtime = stock.mdtime;
            LOG_M_DEBUG("{} 新高: {:.4f}, mdtime={}", symbol, high_price, time_util::format_mdtime(stock.mdtime));
        }
    }

    // Breakout triggered callback
    void onBreakoutTriggered(PGBState& state, const std::string& symbol, int32_t mdtime, int64_t local_recv_timestamp) {
        state.buy_signal_triggered = true;

        uint32_t target_price = state.breakout_detector.get_target_price();
        auto stats = state.breakout_detector.get_stats();

        // Calculate order price = min(target_price * 1.014, limit_up_price)
        uint32_t order_price = static_cast<uint32_t>(target_price * 1.014);
        if (state.limit_up_price > 0 && order_price > state.limit_up_price) {
            order_price = state.limit_up_price;
        }

        const char* scenario_str = (state.gap_scenario == GapScenario::LARGE_GAP)
                                   ? "追新高" : "追4/6";

        if (stats.current_volume == 0) {
            // Direct breakout (target_price < best_ask)
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | Reason={} (直接突破)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    state.open_price, state.prev_close, scenario_str);
        } else {
            // Order book dynamics triggered
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | n={:.0f} | delta_n={} | Reason={} (动力学触发)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    state.open_price, state.prev_close,
                    stats.avg_volume, stats.total_buy_qty, scenario_str);
        }

        LOG_M_DEBUG("========================================");
        LOG_M_DEBUG("SIGNAL TRIGGERED at {} for {}", time_util::format_mdtime(mdtime), symbol);
        LOG_M_DEBUG("Scenario: {}", scenario_str);
        LOG_M_DEBUG("Target Price: {} ({}元)", target_price, price_util::price_to_yuan(target_price));
        LOG_M_DEBUG("Order Price: {} ({}元)", order_price, price_util::price_to_yuan(order_price));
        if (stats.current_volume > 0) {
            LOG_M_DEBUG("n (Avg Volume): {:.0f}, delta_n (Buy Trades): {}", stats.avg_volume, stats.total_buy_qty);
        }
        LOG_M_DEBUG("========================================");

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
#endif // PERCENTAGE_GAIN_BREAKOUT_STRATEGY_H
