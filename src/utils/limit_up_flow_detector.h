#ifndef LIMIT_UP_FLOW_DETECTOR_H
#define LIMIT_UP_FLOW_DETECTOR_H

#include "time_util.h"
#include "../../include/market_data_structs_aligned.h"
#include "../FastOrderBook.h"
#include "../../include/logger.h"
#include <deque>
#include <unordered_map>
#include <cstdint>
#include <string>

#define LOG_MODULE "FlowDetector"

/**
 * LimitUpFlowDetector - 涨停封单流出检测器
 *
 * 从 LimitUpBreakSellStrategy 提取的核心检测逻辑，可复用于多个策略。
 *
 * 功能：
 * 1. 追踪涨停价买单（新增/撤单/成交）
 * 2. 滑动窗口计算封单流出量（成交 + 加权撤单）
 * 3. 判断炸板条件：flow_sum > current_bid * threshold% || current_bid <= min_bid
 *
 * 使用方式：
 *   LimitUpFlowDetector detector({.flow_threshold_pct=60, .flow_window_ms=300, ...});
 *   // 在 on_order / on_transaction 中喂数据
 *   detector.on_order(order, limit_up_price);
 *   detector.on_transaction(txn, limit_up_price);
 *   // 检查是否炸板
 *   if (detector.check(current_time, book, limit_up_price)) { ... }
 */
class LimitUpFlowDetector {
public:
    struct Config {
        uint32_t flow_threshold_pct = 60;   // 流出阈值百分比
        int32_t  flow_window_ms = 300;      // 滑动窗口长度(ms)
        uint64_t min_bid_volume = 1000;     // 封单量下限（低于直接触发）
        double   cancel_weight = 0.2;       // 撤单量权重
    };

    LimitUpFlowDetector() : config_() {}
    explicit LimitUpFlowDetector(const Config& config)
        : config_(config) {}

    // ==========================================
    // 喂数据接口
    // ==========================================

    /**
     * 处理逐笔委托：追踪涨停价买单 + 上海撤单流出
     */
    void on_order(const MDOrderStruct& order, uint32_t limit_up_price) {
        // 只关注涨停价买单
        if (order.orderbsflag != 1) return;  // 非买单
        if (static_cast<uint32_t>(order.orderprice) != limit_up_price) return;

        uint64_t order_id = (order.orderno != 0)
            ? static_cast<uint64_t>(order.orderno)
            : static_cast<uint64_t>(order.orderindex);

        if (order.ordertype == 10) {
            // 上海撤单：查 order_id，命中则加入 flow_window
            auto it = limit_up_bid_orders_.find(order_id);
            if (it != limit_up_bid_orders_.end()) {
                uint64_t cancel_vol = it->second;
                add_flow_event(order.mdtime, static_cast<uint64_t>(cancel_vol * config_.cancel_weight));
                limit_up_bid_orders_.erase(it);
            }
        } else {
            // 新买单：记录
            limit_up_bid_orders_[order_id] = static_cast<uint64_t>(order.orderqty);
        }
    }

    /**
     * 处理逐笔成交：成交流出 + 深圳撤单流出
     */
    void on_transaction(const MDTransactionStruct& txn, uint32_t limit_up_price) {
        if (txn.tradetype == 0) {
            // 成交 at 涨停价 -> 加入 flow_window
            if (static_cast<uint32_t>(txn.tradeprice) != limit_up_price) return;

            add_flow_event(txn.mdtime, static_cast<uint64_t>(txn.tradeqty));

            // 更新买单剩余量
            auto it = limit_up_bid_orders_.find(static_cast<uint64_t>(txn.tradebuyno));
            if (it != limit_up_bid_orders_.end()) {
                uint64_t traded = static_cast<uint64_t>(txn.tradeqty);
                if (traded >= it->second) {
                    limit_up_bid_orders_.erase(it);
                } else {
                    it->second -= traded;
                }
            }
        } else {
            // 深圳撤单：查 tradebuyno，命中则加入 flow_window
            auto it = limit_up_bid_orders_.find(static_cast<uint64_t>(txn.tradebuyno));
            if (it != limit_up_bid_orders_.end()) {
                uint64_t cancel_vol = static_cast<uint64_t>(txn.tradeqty);
                add_flow_event(txn.mdtime, static_cast<uint64_t>(cancel_vol * config_.cancel_weight));

                if (cancel_vol >= it->second) {
                    limit_up_bid_orders_.erase(it);
                } else {
                    it->second -= cancel_vol;
                }
            }
        }
    }

    /**
     * 从 OrderBook 同步涨停价上的已有买单
     * 进入监控状态后首次调用
     */
    void sync_from_book(const FastOrderBook& book, uint32_t limit_up_price,
                        const std::string& symbol = "") {
        uint64_t synced_volume = 0;
        size_t synced_count = 0;
        uint64_t book_bid_volume = book.get_bid_volume_at_price(limit_up_price);

        book.for_each_bid_order_at_price(limit_up_price,
            [this, &synced_volume, &synced_count](uint64_t seq, uint32_t volume) {
                limit_up_bid_orders_[seq] = static_cast<uint64_t>(volume);
                synced_volume += static_cast<uint64_t>(volume);
                ++synced_count;
            });

        LOG_M_INFO("FlowDetector synced {} bid orders at limit_up | symbol={} | "
                   "synced_volume={} | book_bid_volume={}",
                   synced_count, symbol, synced_volume, book_bid_volume);

        if (synced_volume != book_bid_volume) {
            LOG_M_WARNING("FlowDetector sync mismatch | symbol={} | "
                          "synced_count={} | synced_volume={} | book_bid_volume={}",
                          symbol, synced_count, synced_volume, book_bid_volume);
        }

        synced_ = true;
    }

    bool is_synced() const { return synced_; }

    // ==========================================
    // 检测接口
    // ==========================================

    /**
     * 检查是否触发炸板条件
     * @return true 表示封单流出达到阈值（炸板）
     */
    bool check(int32_t current_time, const FastOrderBook& book, uint32_t limit_up_price) {
        // 清理窗口外的条目
        cleanup_flow_window(current_time);

        // 计算窗口内流出总量
        uint64_t flow_sum = 0;
        for (const auto& ev : flow_window_) {
            flow_sum += ev.volume;
        }

        // 获取当前 OrderBook 涨停价买委托量
        uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price);

        // 触发条件: current_bid <= min_bid OR flow_sum > current_bid * threshold%
        return (current_bid <= config_.min_bid_volume ||
                flow_sum * 100 > current_bid * config_.flow_threshold_pct);
    }

    /**
     * 重置所有状态（策略回到 SLEEPING 时调用）
     */
    void reset() {
        limit_up_bid_orders_.clear();
        flow_window_.clear();
        synced_ = false;
    }

    // ==========================================
    // 统计/调试
    // ==========================================

    uint64_t flow_event_count() const { return flow_event_count_; }

    uint64_t get_flow_sum(int32_t current_time) {
        cleanup_flow_window(current_time);
        uint64_t sum = 0;
        for (const auto& ev : flow_window_) {
            sum += ev.volume;
        }
        return sum;
    }

    const Config& config() const { return config_; }

private:
    struct FlowEvent {
        int32_t time;       // MDTime
        uint64_t volume;    // 加权后的流出量
    };

    void add_flow_event(int32_t time, uint64_t volume) {
        flow_window_.push_back({time, volume});
        flow_event_count_++;
    }

    void cleanup_flow_window(int32_t current_time) {
        while (!flow_window_.empty()) {
            if (time_util::is_within_ms(flow_window_.front().time, current_time, config_.flow_window_ms)) {
                break;
            }
            flow_window_.pop_front();
        }
    }

    Config config_;
    std::unordered_map<uint64_t, uint64_t> limit_up_bid_orders_;  // orderno -> volume
    std::deque<FlowEvent> flow_window_;
    uint64_t flow_event_count_ = 0;
    bool synced_ = false;
};

#undef LOG_MODULE

#endif // LIMIT_UP_FLOW_DETECTOR_H
