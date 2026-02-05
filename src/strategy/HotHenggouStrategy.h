#ifndef HOT_HENGGOU_STRATEGY_H_NEW
#define HOT_HENGGOU_STRATEGY_H_NEW

#include "strategy_base.h"
#include "market_data_structs_aligned.h"
#include "trade_signal.h"
#include "logger.h"
#include "breakout_detector.h"
#include "utils/time_util.h"
#include "utils/price_util.h"
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <set>

#define LOG_MODULE MOD_STRATEGY

/**
 * HotHenggouStrategy (热门横沟策略)
 *
 * 动态添加/移除股票的横盘突破策略。
 *
 * 策略逻辑：
 * - Phase 1 (addStock): 回溯60秒历史数据，找到最高价
 * - Phase 2: 等待27秒整理期（无新高）
 * - Phase 3: 使用 BreakoutDetector 检测突破 highest_price
 *
 * 特点：
 * - 支持运行时动态添加/移除股票（通过 ZMQ 命令）
 * - 使用环形缓冲区（std::vector ring buffer）存储历史数据
 * - 线程安全设计
 */
class HotHenggouStrategy : public Strategy {
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
        return (time_hhmm >= 930 && time_hhmm < 1130) ||   // Morning session
               (time_hhmm >= 1300 && time_hhmm < 1500);    // Afternoon session
    }

    // Constants
    static constexpr int64_t LOOKBACK_WINDOW_MS = 60 * 1000;           // 60秒回溯窗口
    static constexpr int64_t CONSOLIDATION_HOLD_TIME_MS = 27 * 1000;   // 27秒整理期
    static constexpr int64_t CONSOLIDATION_UPPER_LIMIT_MS = 180 * 1000; // 3分钟横沟超时

    // Ring buffer configuration
    static constexpr size_t RING_BUFFER_SIZE = 2000;  // ~60s at 30 ticks/s

    // ==========================================
    // Ring Buffer for MDStockStruct history
    // ==========================================
    struct RingBuffer {
        std::vector<MDStockStruct> buf;
        size_t head = 0;      // 下一个写入位置
        size_t count = 0;     // 当前元素数量

        RingBuffer() {
            buf.resize(RING_BUFFER_SIZE);
        }

        void push(const MDStockStruct& data) {
            buf[head] = data;
            head = (head + 1) % RING_BUFFER_SIZE;
            if (count < RING_BUFFER_SIZE) {
                count++;
            }
        }

        // 获取从最旧到最新的数据范围（用于 lookback）
        // 返回指定时间窗口内的数据
        std::vector<MDStockStruct> get_range(int32_t start_mdtime, int32_t end_mdtime) const {
            std::vector<MDStockStruct> result;
            if (count == 0) return result;

            // 从最旧的数据开始遍历
            size_t start_idx = (head + RING_BUFFER_SIZE - count) % RING_BUFFER_SIZE;
            for (size_t i = 0; i < count; ++i) {
                size_t idx = (start_idx + i) % RING_BUFFER_SIZE;
                const auto& md = buf[idx];
                // 使用时间工具计算时间差
                int64_t diff_from_start = time_util::calculate_time_diff_ms(start_mdtime, md.mdtime);
                int64_t diff_from_end = time_util::calculate_time_diff_ms(md.mdtime, end_mdtime);
                if (diff_from_start >= 0 && diff_from_end >= 0) {
                    result.push_back(md);
                }
            }
            return result;
        }

        void clear() {
            head = 0;
            count = 0;
        }
    };

    // ==========================================
    // Per-stock state
    // ==========================================
    struct HotORBState {
        // Phase tracking
        bool needs_init = true;         // Phase 1: 需要初始化（从历史找最高价）
        bool consolidation_met = false; // Phase 2: 整理期已完成
        bool triggered = false;         // 已触发买入信号
        bool expired = false;           // 横沟超时

        // Price tracking
        uint32_t highest_price = 0;         // 最高价（内部格式 * 10000）
        int32_t highest_mdtime = 0;         // 最高价对应的 MDTime
        int frames_since_high = 0;          // 最高价后的帧数

        // Timestamps
        int32_t add_mdtime = 0;             // 添加时间
        int32_t triggered_mdtime = 0;       // 触发时间
        int triggered_frame = 0;            // 触发时的帧数

        // Cached prices
        uint32_t limit_up_price = 0;        // 涨停价
        uint32_t prev_close = 0;            // 前收盘价

        // Phase 3: BreakoutDetector 突破检测
        BreakoutDetector breakout_detector;
        bool detector_armed = false;        // Phase 2 完成后启用

        // Ring buffer for history
        RingBuffer history;

        // Date tracking (for new day detection)
        int32_t current_date = 0;
    };

    // Thread-safe stock states
    mutable std::mutex mutex_;
    std::map<std::string, HotORBState> stock_states_;
    std::set<std::string> active_symbols_;

public:
    explicit HotHenggouStrategy(const std::string& strategy_name,
                                 const std::string& sym = "") {
        this->name = strategy_name;
        this->symbol = sym;
    }

    virtual ~HotHenggouStrategy() = default;

    void on_start() override {}
    void on_stop() override {}

    // ==========================================
    // Dynamic stock management (thread-safe)
    // ==========================================

    /**
     * Add a stock to monitor
     * Called from ZMQ handler thread
     */
    bool addStock(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (active_symbols_.count(symbol) > 0) {
            LOG_M_INFO("[HotHG] {} 已在监控列表中", symbol);
            return true;  // Already monitoring
        }

        // Create new state with lazy init flag
        HotORBState& state = stock_states_[symbol];
        state.needs_init = true;
        state.triggered = false;
        state.expired = false;
        state.consolidation_met = false;
        state.detector_armed = false;
        state.history.clear();
        state.breakout_detector.reset();
        state.breakout_detector.set_enabled(false);

        active_symbols_.insert(symbol);

        LOG_M_INFO("[HotHG] 添加股票监控: {}", symbol);
        LOG_BIZ(BIZ_STRA, "{} | ADD | 添加热门横沟监控", symbol);
        return true;
    }

    /**
     * Remove a stock from monitoring
     * Called from ZMQ handler thread
     */
    bool removeStock(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = active_symbols_.find(symbol);
        if (it == active_symbols_.end()) {
            LOG_M_WARNING("[HotHG] {} 不在监控列表中", symbol);
            return false;
        }

        active_symbols_.erase(it);
        stock_states_.erase(symbol);

        LOG_M_INFO("[HotHG] 移除股票监控: {}", symbol);
        LOG_BIZ(BIZ_STRA, "{} | REMOVE | 移除热门横沟监控", symbol);
        return true;
    }

    /**
     * Check if a stock is being monitored
     */
    bool hasStock(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_symbols_.count(symbol) > 0;
    }

    /**
     * Get count of monitored stocks
     */
    size_t getStockCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_symbols_.size();
    }

    // ==========================================
    // Market data callbacks
    // ==========================================

    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;

        std::string sym = get_symbol(stock);

        // Check market hours
        if (!is_market_open(stock.mdtime)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Check if this symbol is being monitored
        if (active_symbols_.count(sym) == 0) {
            return;
        }

        auto& state = stock_states_[sym];

        // Detect new trading day
        if (state.current_date != stock.mddate) {
            onMarketOpen(state, stock, sym);
        }

        // Add to history buffer
        state.history.push(stock);

        // Skip if signal already triggered or expired
        if (state.triggered || state.expired) {
            return;
        }

        // Update limit up price
        if (stock.maxpx > 0) {
            state.limit_up_price = static_cast<uint32_t>(stock.maxpx);
        }

        // Phase 1: Lazy initialization from history
        if (state.needs_init) {
            initializeFromHistory(stock, state, sym);
            state.needs_init = false;
        }

        // Check consolidation timeout (3 minutes)
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_mdtime, stock.mdtime);
        if (consolidation_duration > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            LOG_M_INFO("[HotHG] {} 横沟超时({}秒 > 180秒)，策略取消",
                       sym, consolidation_duration / 1000);
            return;
        }

        // Phase 2: Check consolidation period
        if (!state.consolidation_met) {
            checkConsolidation(stock, state, sym);
        }

        // Phase 3: Use tick data as fallback trigger
        if (state.detector_armed && !state.triggered) {
            if (state.breakout_detector.on_tick(stock)) {
                onBreakoutTriggered(state, sym, stock.mdtime, stock.local_recv_timestamp);
            }
        }

        // Update highest price (only in Phase 2, before consolidation is met)
        updateHighestPrice(stock, state, sym);
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string sym(order.htscsecurityid);

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = stock_states_.find(sym);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.triggered || !state.detector_armed || state.expired) return;

        // Check expiration in Phase 3
        int64_t time_since_highest = time_util::calculate_time_diff_ms(
            state.highest_mdtime, order.mdtime);
        if (time_since_highest > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_order(order, book)) {
            onBreakoutTriggered(state, sym, order.mdtime, order.local_recv_timestamp);
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;

        std::string sym(txn.htscsecurityid);

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = stock_states_.find(sym);
        if (it == stock_states_.end()) return;

        auto& state = it->second;
        if (state.triggered || !state.detector_armed || state.expired) return;

        // Check expiration in Phase 3
        int64_t time_since_highest = time_util::calculate_time_diff_ms(
            state.highest_mdtime, txn.mdtime);
        if (time_since_highest > CONSOLIDATION_UPPER_LIMIT_MS) {
            state.expired = true;
            state.detector_armed = false;
            state.breakout_detector.set_enabled(false);
            return;
        }

        // Delegate to breakout detector
        if (state.breakout_detector.on_transaction(txn, book)) {
            onBreakoutTriggered(state, sym, txn.mdtime, txn.local_recv_timestamp);
        }
    }

private:
    // ==========================================
    // Internal methods
    // ==========================================

    void onMarketOpen(HotORBState& state, const MDStockStruct& stock, const std::string& sym) {
        state.highest_price = static_cast<uint32_t>(stock.highpx);
        state.highest_mdtime = stock.mdtime;
        state.needs_init = true;  // Re-init on new day
        state.consolidation_met = false;
        state.triggered = false;
        state.expired = false;
        state.current_date = stock.mddate;
        state.detector_armed = false;
        state.breakout_detector.reset();
        state.breakout_detector.set_enabled(false);
        state.history.clear();

        // Cache prices
        state.prev_close = static_cast<uint32_t>(stock.preclosepx);
        state.limit_up_price = static_cast<uint32_t>(stock.maxpx);

        LOG_M_DEBUG("[HotHG] {} 开盘: prev_close={}, limit_up={}",
                   sym, price_util::format_price_display(state.prev_close),
                   price_util::format_price_display(state.limit_up_price));
    }

    /**
     * Phase 1: Initialize from history (lazy initialization)
     * Looks back 60 seconds to find highest price
     */
    void initializeFromHistory(const MDStockStruct& stock, HotORBState& state, const std::string& sym) {
        // Calculate lookback window
        int32_t current_mdtime = stock.mdtime;
        // Get approximate start time (subtract 60 seconds in mdtime format)
        // Note: This is a simplification; proper mdtime subtraction would need
        // to handle hour/minute boundaries
        int64_t current_ms = time_util::mdtime_to_ms(current_mdtime);
        int64_t start_ms = current_ms - LOOKBACK_WINDOW_MS;
        // Convert back to mdtime format (approximate)
        int32_t h = static_cast<int32_t>(start_ms / 3600000);
        int32_t m = static_cast<int32_t>((start_ms / 60000) % 60);
        int32_t s = static_cast<int32_t>((start_ms / 1000) % 60);
        int32_t ms = static_cast<int32_t>(start_ms % 1000);
        int32_t start_mdtime = h * 10000000 + m * 100000 + s * 1000 + ms;

        auto window_data = state.history.get_range(start_mdtime, current_mdtime);

        state.add_mdtime = current_mdtime;
        state.frames_since_high = 1;

        if (window_data.empty()) {
            // No history available, use current high_price
            state.highest_price = static_cast<uint32_t>(stock.highpx);
            state.highest_mdtime = stock.mdtime;
        } else {
            // Find highest price in lookback window
            uint32_t highest = static_cast<uint32_t>(window_data.front().highpx);
            int32_t highest_ts = window_data.front().mdtime;
            size_t highest_idx = 0;

            for (size_t i = 0; i < window_data.size(); ++i) {
                uint32_t hp = static_cast<uint32_t>(window_data[i].highpx);
                if (hp > highest) {
                    highest = hp;
                    highest_ts = window_data[i].mdtime;
                    highest_idx = i;
                }
            }

            state.highest_price = highest;
            state.highest_mdtime = highest_ts;
            state.frames_since_high = static_cast<int>(window_data.size() - highest_idx);
        }

        LOG_M_INFO("[HotHG] {} 初始化: highest={} frames_since_high={} history_size={}",
                   sym, price_util::format_price_display(state.highest_price),
                   state.frames_since_high, window_data.size());
        LOG_BIZ(BIZ_STRA, "{} | INIT | highest={} | add_time={}",
                sym, price_util::format_price_display(state.highest_price),
                time_util::format_mdtime(state.add_mdtime));
    }

    /**
     * Phase 2: Check consolidation period (27 seconds since last high)
     */
    void checkConsolidation(const MDStockStruct& stock, HotORBState& state, const std::string& sym) {
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_mdtime, stock.mdtime);

        // Check if consolidation period is met (27 seconds)
        if (consolidation_duration >= CONSOLIDATION_HOLD_TIME_MS) {
            state.consolidation_met = true;

            // Arm the breakout detector with highest_price as target
            state.breakout_detector.set_target_price(state.highest_price);
            state.breakout_detector.set_enabled(true);
            state.detector_armed = true;

            int64_t consolidation_seconds = consolidation_duration / 1000;
            LOG_M_INFO("[HotHG] {} Phase2完成: 顿点{}秒, 目标价={}",
                       sym, consolidation_seconds,
                       price_util::format_price_display(state.highest_price));
            LOG_BIZ(BIZ_STRA, "{} | PHASE2 | 顿点{}秒 | target={}",
                    sym, consolidation_seconds,
                    price_util::format_price_display(state.highest_price));
        }
    }

    /**
     * Update highest price tracking (only in Phase 2, before consolidation is met)
     */
    void updateHighestPrice(const MDStockStruct& stock, HotORBState& state, const std::string& sym) {
        // Increment frame counter
        state.frames_since_high++;

        // Don't update after consolidation is met (highest price is locked)
        if (state.consolidation_met) {
            return;
        }

        // Check if still within consolidation window (27 seconds)
        int64_t consolidation_duration = time_util::calculate_time_diff_ms(
            state.highest_mdtime, stock.mdtime);
        if (consolidation_duration >= CONSOLIDATION_HOLD_TIME_MS) {
            return;
        }

        // Update if new high
        uint32_t high_price = static_cast<uint32_t>(stock.highpx);
        if (high_price > state.highest_price) {
            state.highest_price = high_price;
            state.highest_mdtime = stock.mdtime;
            state.frames_since_high = 1;  // Reset frame counter
            LOG_M_DEBUG("[HotHG] {} 新高: {}", sym, price_util::format_price_display(high_price));
        }
    }

    /**
     * Breakout triggered callback
     */
    void onBreakoutTriggered(HotORBState& state, const std::string& sym,
                             int32_t mdtime, int64_t local_recv_timestamp) {
        state.triggered = true;
        state.triggered_mdtime = mdtime;
        state.triggered_frame = state.frames_since_high;

        // Check market data delay
        int64_t delay_ms = time_util::calculate_time_diff_ms(mdtime, time_util::now_mdtime());
        if (delay_ms > 5000) {
            LOG_BIZ(BIZ_STRA,
                    "{} | SKIP | MARKET_TIME={} | LOCAL_TIME={} | Reason=热门横沟突破信号延迟过大 (delay={}ms > 5000ms)",
                    sym, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp), delay_ms);
            LOG_M_INFO("[HotHG] {} 突破信号因延迟过大被跳过: delay={}ms", sym, delay_ms);
            return;
        }

        uint32_t target_price = state.breakout_detector.get_target_price();
        auto stats = state.breakout_detector.get_stats();

        // Calculate order price = min(target_price * 1.014, limit_up_price)
        uint32_t order_price = static_cast<uint32_t>(target_price * 1.014);
        if (state.limit_up_price > 0 && order_price > state.limit_up_price) {
            order_price = state.limit_up_price;
        }

        if (stats.current_volume == 0) {
            // Direct breakout (target_price < best_ask)
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | "
                    "PrevClose={} | Reason=热门横沟突破 (直接突破)",
                    sym, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    price_util::format_price_display(state.prev_close));
        } else {
            // Order book dynamics triggered
            LOG_BIZ(BIZ_STRA,
                    "{} | BUY | MARKET_TIME={} | LOCAL_TIME={} | Price={} | "
                    "PrevClose={} | n={:.0f} | delta_n={} | Reason=热门横沟突破 (动力学触发)",
                    sym, time_util::format_mdtime(mdtime),
                    time_util::format_ns_time(local_recv_timestamp),
                    price_util::format_price_display(order_price),
                    price_util::format_price_display(state.prev_close),
                    stats.avg_volume, stats.total_buy_qty);
        }

        LOG_M_INFO("========================================");
        LOG_M_INFO("[HotHG] SIGNAL TRIGGERED at {} for {}", time_util::format_mdtime(mdtime), sym);
        LOG_M_INFO("[HotHG] Scenario: 热门横沟突破");
        LOG_M_INFO("[HotHG] Target Price: {}", price_util::format_price_display(target_price));
        LOG_M_INFO("[HotHG] Order Price: {}", price_util::format_price_display(order_price));
        if (stats.current_volume > 0) {
            LOG_M_INFO("[HotHG] n={:.0f}, delta_n={}", stats.avg_volume, stats.total_buy_qty);
        }
        LOG_M_INFO("========================================");

        // Place buy order
        TradeSignal signal;
        signal.symbol = sym;
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
#endif // HOT_HENGGOU_STRATEGY_H_NEW
