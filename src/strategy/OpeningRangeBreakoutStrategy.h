#ifndef OPENING_RANGE_BREAKOUT_STRATEGY_H
#define OPENING_RANGE_BREAKOUT_STRATEGY_H

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
 * Opening Range Breakout Strategy - Adapted for current trading engine
 *
 * 策略逻辑：
 * - Gap Down Scenario: 低开后，价格回升突破前收盘价时买入
 * - Gap Up Scenario: 高开后，经过整理期突破时买入
 *
 * 突破检测：
 * - 使用 BreakoutDetector 进行盘口动力学检测
 * - 200ms 滚动窗口，delta_n >= n 时提前触发
 *
 * 参数配置：
 * - Opening range period: 开盘后观察期（默认 30 秒）
 * - Breakout threshold: 突破确认阈值
 *
 * 风险控制：
 * - 仅在开盘时段执行（9:30-11:30, 13:00-15:00）
 * - 单日单次交易限制
 */
class OpeningRangeBreakoutStrategy : public Strategy {
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

    // 集合竞价结束后（09:25 之后）
    static bool is_after_auction_end(int32_t mdtime) {
        int time_hhmm = mdtime / 100000;
        return time_hhmm >= 925;
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
    static constexpr int64_t THIRTY_SECONDS_MS = 30 * 1000;       // 30 秒
    static constexpr int64_t TEN_MINUTES_MS = 10 * 60 * 1000;     // 10 分钟
    static constexpr int THRESHOLD_60_00_CAP = 700;          // 7% in basis points
    static constexpr int THRESHOLD_30_68_CAP = 1700;         // 17% in basis points

    // 场景类型
    enum class BreakoutScenario {
        NONE,
        GAP_DOWN_RECOVERY,    // 绿开翻红
        GAP_UP_NEW_HIGH       // 高开新高
    };

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

        // 突破检测器
        BreakoutDetector breakout_detector;
        bool detector_armed = false;  // 检测器是否已设置目标价
        BreakoutScenario armed_scenario = BreakoutScenario::NONE;
        bool scenario_determined = false;  // 场景是否已判断（只判断一次）
        bool highest_initialized = false;  // highest_price 是否在开盘后初始化

        // 价格缓存（用于突破回调）
        double prev_close = 0.0;
        double open_price = 0.0;
        uint32_t limit_up_price = 0;
    };

    std::map<std::string, ORBState> stock_states_;

public:
    explicit OpeningRangeBreakoutStrategy(const std::string& strategy_name,
                                          const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;
    }

    virtual ~OpeningRangeBreakoutStrategy() = default;

private:
    // 新交易日第一个 tick 时调用，初始化状态
    void OnFirstTick(ORBState& state, const MDStockStruct& stock, const std::string& symbol) {
        state.buy_signal_triggered = false;
        state.current_date = stock.mddate;
        state.detector_armed = false;
        state.armed_scenario = BreakoutScenario::NONE;
        state.scenario_determined = false;
        state.highest_initialized = false;
        state.breakout_detector.reset();
        state.breakout_detector.set_enabled(false);

        // 缓存价格
        state.prev_close = to_price(stock.preclosepx);

        // 更新涨停价
        if (stock.maxpx > 0) {
            state.limit_up_price = static_cast<uint32_t>(stock.maxpx);
        }

        initializeThresholds(state, symbol);

        LOG_M_DEBUG("OnFirstTick {}: prev_close={:.4f}, limit_up={}, date={}, mdtime={}",
                   symbol, state.prev_close,
                   price_util::price_to_yuan(state.limit_up_price),
                   state.current_date, time_util::format_mdtime(stock.mdtime));
    }

    // 集合竞价结束后（09:25）调用一次，判断场景并设置检测器
    void OnOpeningCallEnd(ORBState& state, const MDStockStruct& stock, const std::string& symbol) {
        // 集合竞价结束后才能获取正确的开盘价
        double open_price = to_price(stock.openpx);

        // 【修复】如果 openpx 还是 0，等待下一个有效的 tick
        // 某些股票在 09:25:01 时 openpx 可能尚未填充
        if (open_price <= 0.0) {
            LOG_M_DEBUG("{} openpx=0 at {}, 等待有效开盘价",
                       symbol, time_util::format_mdtime(stock.mdtime));
            return;  // 不设置 scenario_determined，下一帧重试
        }

        state.scenario_determined = true;
        state.open_price = open_price;

        double prev_close = state.prev_close;

        if (open_price < prev_close) {
            // 绿开翻红场景：立即设置检测器
            state.armed_scenario = BreakoutScenario::GAP_DOWN_RECOVERY;

            uint32_t target_price = to_price_int(prev_close);
            state.breakout_detector.set_target_price(target_price);
            state.breakout_detector.set_enabled(true);
            state.detector_armed = true;

            double sell_price_01 = to_price(stock.sellpricequeue[0]);
            LOG_M_DEBUG("{} 绿开翻红检测器已设置 (集合竞价结束): target={} ({}元), sell1={:.4f}, open={:.4f}, prev_close={:.4f}, mdtime={}",
                       symbol, target_price, price_util::price_to_yuan(target_price),
                       sell_price_01, open_price, prev_close, time_util::format_mdtime(stock.mdtime));
        } else {
            // 高开新高场景：标记场景，检测器需要等 30 秒整理期
            state.armed_scenario = BreakoutScenario::GAP_UP_NEW_HIGH;

            LOG_M_DEBUG("{} 高开新高场景: open={:.4f} >= prev_close={:.4f}, 等待30秒整理期, mdtime={}",
                       symbol, open_price, prev_close, time_util::format_mdtime(stock.mdtime));
        }
    }

public:
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;

        std::string symbol = get_symbol(stock);
        auto& stock_state = stock_states_[symbol];

        // ===== 检测新交易日并初始化（最先执行）=====
        if (stock_state.current_date != stock.mddate) {
            OnFirstTick(stock_state, stock, symbol);
        }

        // 信号已触发，跳过
        if (stock_state.buy_signal_triggered) {
            return;
        }

        // ===== 集合竞价结束后（09:25）：场景判断 =====
        if (is_after_auction_end(stock.mdtime) && !stock_state.scenario_determined) {
            OnOpeningCallEnd(stock_state, stock, symbol);
        }

        // ===== 以下逻辑只在开盘后（09:30）执行 =====
        if (!is_market_open(stock.mdtime)) {
            return;
        }

        // 09:30 后第一帧：初始化 highest_price（用于高开新高场景）
        if (!stock_state.highest_initialized) {
            stock_state.highest_initialized = true;
            stock_state.highest_price = to_price(stock.highpx);
            stock_state.highest_timestamp_mdtime = stock.mdtime;
            LOG_M_DEBUG("{} 开盘后初始化 highest_price={:.4f}, mdtime={}",
                       symbol, stock_state.highest_price, time_util::format_mdtime(stock.mdtime));
        }

        // 开盘后 10 分钟内
        int64_t time_since_open = get_time_since_open_ms(stock.mdtime);
        if (time_since_open > TEN_MINUTES_MS) {
            return;
        }

        double high_price = to_price(stock.highpx);

        // 涨幅超过阈值，取消策略
        int highest_gain_bp = calculate_percentage_bp(high_price, stock_state.prev_close);
        if (highest_gain_bp >= stock_state.cap_threshold_bp) {
            LOG_M_DEBUG("{} 最高涨幅超过阈值 {}bp (threshold={}bp). 取消.",
                        symbol, highest_gain_bp, stock_state.cap_threshold_bp);
            return;
        }

        // 高开新高场景：等待 30 秒整理期后设置检测器
        if (stock_state.armed_scenario == BreakoutScenario::GAP_UP_NEW_HIGH &&
            !stock_state.detector_armed) {
            armGapUpDetector(stock, stock_state, symbol);
        }

        // 更新最高价（用于高开新高场景）
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            stock_state.highest_timestamp_mdtime, stock.mdtime);

        if (consolidation_duration < THIRTY_SECONDS_MS) {
            if (high_price > stock_state.highest_price) {
                LOG_M_DEBUG("{} 新高: {:.4f} -> {:.4f}, consolidation重置",
                            symbol, stock_state.highest_price, high_price);
                stock_state.highest_price = high_price;
                stock_state.highest_timestamp_mdtime = stock.mdtime;
            }
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        // 只在开盘时间处理（09:30-11:30, 13:00-15:00）
        if (!is_market_open(order.mdtime)) return;

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

        // 委托给检测器处理
        if (state.breakout_detector.on_order(order, book)) {
            onBreakoutTriggered(state, symbol, order.mdtime, order.local_recv_timestamp);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        // 只在开盘时间处理（09:30-11:30, 13:00-15:00）
        if (!is_market_open(txn.mdtime)) return;

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

        // 委托给检测器处理
        if (state.breakout_detector.on_transaction(txn, book)) {
            onBreakoutTriggered(state, symbol, txn.mdtime, txn.local_recv_timestamp);
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

    // 高开新高场景：30秒整理后设置检测器监控 highest_price
    void armGapUpDetector(const MDStockStruct& stock,
                          ORBState& stock_state,
                          const std::string& symbol) {
        // 如果检测器已设置，不重复设置
        if (stock_state.detector_armed) return;

        // Must be at least 30 seconds after highest price
        // 使用 time_util 正确计算时间差（MDTime 不能直接做减法）
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            stock_state.highest_timestamp_mdtime, stock.mdtime);
        if (consolidation_duration < THIRTY_SECONDS_MS) return;

        // 30秒整理期满足，锁定当前 highest_price 作为目标价
        uint32_t target_price = to_price_int(stock_state.highest_price);

        stock_state.breakout_detector.set_target_price(target_price);
        stock_state.breakout_detector.set_enabled(true);
        stock_state.detector_armed = true;
        stock_state.armed_scenario = BreakoutScenario::GAP_UP_NEW_HIGH;

        int64_t consolidation_seconds = consolidation_duration / 1000;  // MDTime: 1 second = 1000 units
        LOG_M_DEBUG("{} 高开新高检测器已设置: target_price={} ({}元), consolidation={}s",
                   symbol, target_price, price_util::price_to_yuan(target_price),
                   consolidation_seconds);
    }

    // 突破触发回调
    void onBreakoutTriggered(ORBState& stock_state,
                             const std::string& symbol,
                             int32_t mdtime,
                             int64_t local_recv_timestamp) {
        stock_state.buy_signal_triggered = true;

        uint32_t target_price = stock_state.breakout_detector.get_target_price();
        auto stats = stock_state.breakout_detector.get_stats();

        // 计算下单价格 = min(target_price * 1.017, 涨停价)
        uint32_t order_price = static_cast<uint32_t>(target_price * 1.017);
        if (stock_state.limit_up_price > 0 && order_price > stock_state.limit_up_price) {
            order_price = stock_state.limit_up_price;
        }

        const char* reason = (stock_state.armed_scenario == BreakoutScenario::GAP_DOWN_RECOVERY)
                             ? "弱转强绿开翻红" : "弱转强高开新高";

        if (stats.current_volume == 0) {
            // 直接突破（target_price < best_ask）
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | Reason={} (直接突破)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    stock_state.open_price, stock_state.prev_close, reason);
        } else {
            // 盘口动力学触发
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | Open={:.4f} | "
                    "PrevClose={:.4f} | n={:.0f} | delta_n={} | Reason={} (动力学触发)",
                    symbol, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    stock_state.open_price, stock_state.prev_close,
                    stats.avg_volume, stats.total_buy_qty, reason);
        }

        LOG_M_DEBUG("========================================");
        LOG_M_DEBUG("SIGNAL TRIGGERED at {} for {}", time_util::format_mdtime(mdtime), symbol);
        LOG_M_DEBUG("Scenario: {}", reason);
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
#endif // OPENING_RANGE_BREAKOUT_STRATEGY_H
