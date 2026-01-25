#ifndef BREAKOUT_DETECTOR_H
#define BREAKOUT_DETECTOR_H

#include "FastOrderBook.h"
#include "market_data_structs_aligned.h"
#include "utils/time_util.h"
#include "logger.h"
#include <deque>
#include <functional>
#include <cstdint>
#include <optional>

#define LOG_MODULE "BreakoutDetector"

/**
 * BreakoutDetector - 盘口动力学突破检测器
 *
 * 用于检测价格是否突破指定价位，使用盘口动力学提高触发速度。
 *
 * 检测逻辑：
 * 1. 维护200ms滚动窗口，跟踪目标价档位的挂单量和主买成交量
 * 2. 计算 n = 窗口内挂单量平均值
 * 3. 计算 delta_n = 窗口内主买成交累计和
 * 4. 触发条件（满足任一）：
 *    - delta_n >= n（主买力度足以吃掉平均挂单量）
 *    - target_price < best_ask（价格已被突破）
 *
 * 档位稀疏处理：
 * 高价股可能目标价精确点位无挂单，智能选择监控档位：
 * - 优先监控目标价档位
 * - 若目标价无挂单，转而监控 best_ask
 * - 主买成交判断也覆盖 [best_ask, target_price] 区间
 *
 * 使用方式：
 * ```cpp
 * BreakoutDetector detector;
 * detector.set_target_price(target_price);
 * detector.set_callback([this](uint32_t price, int32_t time) {
 *     // 突破回调，执行买入逻辑
 * });
 * detector.set_enabled(true);
 *
 * // 在策略的 on_order / on_transaction 中调用
 * detector.on_order(order, book);
 * detector.on_transaction(txn, book);
 * ```
 */
class BreakoutDetector {
public:
    // 突破回调函数类型：(突破价格, 触发时间)
    using BreakoutCallback = std::function<void(uint32_t price, int32_t mdtime)>;

    // 默认构造函数
    BreakoutDetector()
        : target_price_(0)
        , callback_(nullptr)
        , triggered_(false)
        , enabled_(false) {}

    // 带参数构造函数
    explicit BreakoutDetector(uint32_t target_price, BreakoutCallback callback = nullptr)
        : target_price_(target_price)
        , callback_(std::move(callback))
        , triggered_(false)
        , enabled_(target_price > 0) {}

    // 设置/更新目标价格（会重置状态）
    void set_target_price(uint32_t price) {
        target_price_ = price;
        reset();
    }

    uint32_t get_target_price() const { return target_price_; }

    // 设置回调
    void set_callback(BreakoutCallback callback) {
        callback_ = std::move(callback);
    }

    // 启用/禁用
    void set_enabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled) {
            window_.clear();
        }
    }
    bool is_enabled() const { return enabled_; }

    // 是否已触发
    bool is_triggered() const { return triggered_; }

    // 重置状态（用于重新开始监控）
    void reset() {
        triggered_ = false;
        window_.clear();
    }

    // 处理委托事件
    // 返回值：true = 本次检测触发了突破信号
    bool on_order(const MDOrderStruct& order, const FastOrderBook& book) {
        if (!enabled_ || triggered_) return false;

        auto current_volume = get_volume_at_target(book);
        if (!current_volume.has_value()) {
            // 目标价太高，无法监控，跳过本次数据
            auto best_ask = book.get_best_ask();
            LOG_M_DEBUG("[ORDER SKIP] symbol={} mdtime={} target={} best_ask={} reason=目标价太高无法监控",
                       order.htscsecurityid, time_util::format_mdtime(order.mdtime),
                       target_price_, best_ask.value_or(0));
            return false;
        }
        add_to_window(order.mdtime, current_volume.value(), 0);

        // 打印当前状态
        auto best_ask = book.get_best_ask();
        auto stats = get_stats();
        LOG_M_DEBUG("[ORDER] symbol={} mdtime={} target={} best_ask={} n={:.0f} delta_n={} vol={} | orderindex={} price={} qty={} bs={}",
                   order.htscsecurityid, time_util::format_mdtime(order.mdtime),
                   target_price_, best_ask.value_or(0), stats.avg_volume, stats.total_buy_qty, stats.current_volume,
                   order.orderindex, order.orderprice, order.orderqty, order.orderbsflag);

        bool result = check_trigger(order.mdtime);
        if (result) {
            LOG_M_DEBUG("[ORDER TRIGGER] symbol={} mdtime={} orderindex={} orderprice={} orderqty={} bsflag={}",
                       order.htscsecurityid, time_util::format_mdtime(order.mdtime),
                       order.orderindex, order.orderprice, order.orderqty, order.orderbsflag);
        }
        return result;
    }

    // 处理成交事件
    // 返回值：true = 本次检测触发了突破信号
    bool on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) {
        if (!enabled_ || triggered_) return false;

        auto current_volume = get_volume_at_target(book);
        if (!current_volume.has_value()) {
            // 目标价太高，无法监控，跳过本次数据
            auto best_ask = book.get_best_ask();
            LOG_M_DEBUG("[TXN SKIP] symbol={} mdtime={} target={} best_ask={} reason=目标价太高无法监控",
                       txn.htscsecurityid, time_util::format_mdtime(txn.mdtime),
                       target_price_, best_ask.value_or(0));
            return false;
        }

        // 只统计成交价等于目标价的主买成交
        uint64_t buy_trade_qty = get_buy_trade_qty(txn, book);

        add_to_window(txn.mdtime, current_volume.value(), buy_trade_qty);

        // 打印当前状态
        auto best_ask = book.get_best_ask();
        auto stats = get_stats();
        LOG_M_DEBUG("[TXN] symbol={} mdtime={} target={} best_ask={} n={:.0f} delta_n={} vol={} | tradeindex={} price={} qty={} bs={} buy_qty={}",
                   txn.htscsecurityid, time_util::format_mdtime(txn.mdtime),
                   target_price_, best_ask.value_or(0), stats.avg_volume, stats.total_buy_qty, stats.current_volume,
                   txn.tradeindex, txn.tradeprice, txn.tradeqty, txn.tradebsflag, buy_trade_qty);

        bool result = check_trigger(txn.mdtime);
        if (result) {
            LOG_M_DEBUG("[TXN TRIGGER] symbol={} mdtime={} tradeindex={} tradeprice={} tradeqty={} bsflag={}",
                       txn.htscsecurityid, time_util::format_mdtime(txn.mdtime),
                       txn.tradeindex, txn.tradeprice, txn.tradeqty, txn.tradebsflag);
        }
        return result;
    }

    // 获取当前统计信息（用于调试/日志）
    struct Stats {
        double avg_volume;      // 窗口内平均挂单量 (n)
        uint64_t total_buy_qty; // 窗口内主买累计量 (delta_n)
        uint64_t current_volume;// 当前挂单量
        size_t window_size;     // 窗口内数据点数
    };

    Stats get_stats() const {
        Stats stats = {0.0, 0, 0, window_.size()};
        if (window_.empty()) return stats;

        uint64_t total_volume = 0;
        for (const auto& s : window_) {
            total_volume += s.volume;
            stats.total_buy_qty += s.buy_trade_qty;
        }
        stats.avg_volume = static_cast<double>(total_volume) / window_.size();
        stats.current_volume = window_.back().volume;
        return stats;
    }

private:
    struct WindowSnapshot {
        int32_t mdtime;
        uint64_t volume;        // 目标价档位的挂单量
        uint64_t buy_trade_qty; // 主买成交量（单笔）

        WindowSnapshot(int32_t t, uint64_t v, uint64_t b)
            : mdtime(t), volume(v), buy_trade_qty(b) {}
    };

    uint32_t target_price_;
    BreakoutCallback callback_;
    bool triggered_;
    bool enabled_;
    std::deque<WindowSnapshot> window_;

    static constexpr int WINDOW_MS = 200;  // 滚动窗口大小（毫秒）

    // 获取目标价位的挂单量（智能处理档位稀疏）
    // 返回 0 表示已被突破，返回 nullopt 表示目标价太高无法监控
    std::optional<uint64_t> get_volume_at_target(const FastOrderBook& book) const {
        auto best_ask = book.get_best_ask();
        if (!best_ask.has_value()) return 0;  // 卖盘空，视为突破

        // 目标价已小于卖一价，说明已突破
        if (target_price_ < best_ask.value()) {
            return 0;
        }

        // 优先监控目标价档位
        uint64_t vol = book.get_volume_at_price(target_price_);
        if (vol > 0) return vol;

        // 目标价无挂单，找比目标价高的最近有挂单的档位
        auto ask_levels = book.get_ask_levels(10);
        for (const auto& [price, volume] : ask_levels) {
            if (price > target_price_ && volume > 0) {
                return volume;  // 监控这个更高的档位
            }
        }

        // 找不到比目标价高的档位（目标价太高），无法监控
        return std::nullopt;
    }

    // 判断成交是否为有效主买（成交价必须等于目标价）
    uint64_t get_buy_trade_qty(const MDTransactionStruct& txn, const FastOrderBook& book) const {
        if (txn.tradebsflag != 1) return 0;  // 非主买

        uint32_t trade_price = static_cast<uint32_t>(txn.tradeprice);

        // 只统计成交价等于目标价的主买成交
        if (trade_price == target_price_) {
            return txn.tradeqty;
        }
        return 0;
    }

    // 添加数据到窗口，并清理过期数据
    void add_to_window(int32_t mdtime, uint64_t volume, uint64_t buy_trade_qty) {
        window_.emplace_back(mdtime, volume, buy_trade_qty);

        // 移除超过200ms的旧数据
        while (!window_.empty()) {
            if (time_util::is_within_ms(window_.front().mdtime, mdtime, WINDOW_MS)) {
                break;
            }
            window_.pop_front();
        }
    }

    // 检查触发条件
    bool check_trigger(int32_t mdtime) {
        if (triggered_ || window_.empty()) return false;

        const auto& latest = window_.back();

        // 条件1：目标价已小于卖一价（已被突破）
        if (latest.volume == 0) {
            LOG_M_DEBUG("[TRIGGER] 直接突破: target_price={} < best_ask, mdtime={}",
                       target_price_, time_util::format_mdtime(mdtime));
            trigger(mdtime);
            return true;
        }

        // 计算 n（平均挂单量）
        uint64_t total_volume = 0;
        for (const auto& s : window_) {
            total_volume += s.volume;
        }
        double n = static_cast<double>(total_volume) / window_.size();

        // 边界保护
        if (n < 1.0) return false;

        // 计算 delta_n（主买累计量，只统计目标价的成交）
        uint64_t delta_n = 0;
        for (const auto& s : window_) {
            delta_n += s.buy_trade_qty;
        }

        // 条件2：delta_n >= n
        if (delta_n >= static_cast<uint64_t>(n)) {
            LOG_M_DEBUG("[TRIGGER] 动力学突破: target_price={} n={:.0f} delta_n={} window_size={} mdtime={}",
                       target_price_, n, delta_n, window_.size(), time_util::format_mdtime(mdtime));
            trigger(mdtime);
            return true;
        }

        return false;
    }

    void trigger(int32_t mdtime) {
        triggered_ = true;
        if (callback_) {
            callback_(target_price_, mdtime);
        }
    }
};

#undef LOG_MODULE
#endif // BREAKOUT_DETECTOR_H
