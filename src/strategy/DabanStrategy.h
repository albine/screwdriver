#ifndef DABAN_STRATEGY_H
#define DABAN_STRATEGY_H

#include "strategy_base.h"
#include "trade_signal.h"
#include "logger.h"
#include "utils/price_util.h"
#include "utils/time_util.h"
#include "utils/limit_up_flow_detector.h"
#include "../utils/symbol_utils.h"
#include <deque>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <cmath>

#define LOG_MODULE MOD_STRATEGY

/**
 * 打板策略：基于涨停价委托强度的买入信号生成 + 炸板卖出
 *
 * 入场逻辑：
 * 1. 激活条件：10ms内有成交价涨幅 > 8%
 * 2. 休眠条件：10ms内所有成交价涨幅 < 7%
 * 3. 窗口：[t0, t]，最长1000ms，t0为首次>8%成交时间
 * 4. n：窗口内涨停价买单累计数量（包括已成交，撤单减去）
 * 5. m：t时刻卖一价到涨停价的所有卖单数量（实时计算）
 * 6. 触发：n > m/2（每轮只触发一次）
 *
 * 出场逻辑（BUY信号发出后）：
 * - 使用 LimitUpFlowDetector 监控封单流出
 * - 炸板（流出达阈值）→ 发 SELL 信号
 * - lastpx < 涨停价（backup）→ 发 SELL 信号
 * - 14:57后结束本轮，后续由 dealer 处理
 *
 * 状态机：
 *   SLEEPING → ACTIVE → EXIT_ARMED → DONE
 *   SLEEPING ←→ ACTIVE（可回退，直到 BUY_SIGNAL 发出）
 *   EXIT_ARMED 不可回退（已发出 BUY_SIGNAL）
 */
class DabanStrategy : public Strategy {
public:
    // ============ 状态枚举 ============
    enum class State {
        SLEEPING,      // 尚未激活，不允许触发 BUY
        ACTIVE,        // 已激活，允许检测并发出 BUY_SIGNAL
        EXIT_ARMED,    // BUY_SIGNAL 已发出，盯退出条件（不是持仓状态）
        DONE           // 本轮策略结束
    };

private:
    // ============ 入场配置参数 ============
    double activate_threshold_ = 0.08;    // 激活阈值 8%
    double sleep_threshold_ = 0.07;       // 休眠阈值 7%
    uint32_t max_window_ms_ = 1000;       // 最大窗口1000ms
    uint32_t trade_window_ms_ = 10;       // 成交窗口10ms

    // ============ 出场保底参数 ============
    double exit_volume_ratio_ = 0.5;         // 成交量/买一量 阈值（默认50%）
    double exit_min_bid_amount_ = 5000000.0; // 买一金额下限（元，默认500万）

    // ============ 延迟过滤 ============
    int64_t max_delay_ms_ = 3000;            // 行情延迟阈值（ms，超过则跳过）

    // ============ 交易时段常量 ============
    static constexpr int32_t MARKET_OPEN_TIME = 93000000;    // 09:30:00.000
    static constexpr int32_t MARKET_CLOSE_TIME = 145700000;  // 14:57:00.000

    // ============ 成交记录（10ms窗口）============
    struct TradeRecord {
        int32_t time;       // MDTime 格式
        uint32_t price;     // 内部价格格式（×10000）
        uint64_t volume;
    };

    // ============ 涨停价委托记录 ============
    struct LimitUpBid {
        uint64_t order_seq;
        int32_t time;       // MDTime 格式
        uint64_t volume;    // 原始委托量
    };

    // ============ 状态变量 ============
    State state_ = State::SLEEPING;
    int32_t t0_ = 0;                     // 首次>8%成交时间
    bool signal_triggered_ = false;
    int32_t signal_triggered_time_ = 0;

    // ============ 数据窗口 ============
    std::deque<TradeRecord> recent_trades_;      // 10ms成交窗口
    std::deque<LimitUpBid> limit_up_bids_;       // 涨停价委托窗口
    std::unordered_map<uint64_t, std::deque<LimitUpBid>::iterator> bid_index_;
    uint64_t n_ = 0;                             // 累计涨停委托数量

    // ============ 涨停检测 ============
    int32_t limit_up_time_ = 0;
    bool limit_up_detected_ = false;

    // ============ 价格缓存（内部格式×10000）============
    uint32_t prev_close_ = 0;         // 昨收价
    uint32_t limit_up_price_ = 0;     // 涨停价
    uint32_t activate_price_ = 0;     // 激活阈值价格（涨幅 > 8% 的价格边界）
    uint32_t sleep_price_ = 0;        // 休眠阈值价格

    // ============ 周期性任务管理 ============
    int32_t last_tick_time_ = 0;
    uint32_t tick_interval_ms_ = 10;

    // ============ 炸板检测器（EXIT_ARMED 阶段使用）============
    LimitUpFlowDetector flow_detector_;

    // ============ 统计信息 ============
    uint64_t tick_count_ = 0;
    uint64_t order_count_ = 0;
    uint64_t transaction_count_ = 0;
    uint64_t activate_count_ = 0;
    uint64_t sleep_count_ = 0;
    uint64_t trigger_count_ = 0;
    uint64_t max_n_ = 0;
    uint64_t max_m_ = 0;

public:
    explicit DabanStrategy(const std::string& strategy_name, const std::string& sym)
    {
        this->name = strategy_name;
        this->symbol = sym;
        this->enabled_ = true;

        // 炸板检测器：比 LimitUpBreakSellStrategy 更敏感的参数
        LimitUpFlowDetector::Config detector_config;
        detector_config.flow_threshold_pct = 30;
        detector_config.flow_window_ms = 200;
        detector_config.min_bid_volume = 500;
        detector_config.cancel_weight = 0.3;
        flow_detector_ = LimitUpFlowDetector(detector_config);
    }

    virtual ~DabanStrategy() = default;

    // ==========================================
    // 生命周期回调
    // ==========================================
    void on_start() override {
        auto& cfg = flow_detector_.config();
        LOG_M_DEBUG("DabanStrategy started: {} | symbol={} | enabled={} | "
                   "detector: threshold={}% window={}ms min_bid={} cancel_weight={:.2f}",
                   name, symbol, is_enabled() ? "true" : "false",
                   cfg.flow_threshold_pct, cfg.flow_window_ms, cfg.min_bid_volume, cfg.cancel_weight);
    }

    void on_stop() override {
        LOG_M_DEBUG("DabanStrategy stopped: {} | ticks={} | orders={} | txns={} | triggers={} | state={}",
                   name, tick_count_, order_count_, transaction_count_, trigger_count_,
                   state_to_string(state_));
        print_status();
    }

    // ==========================================
    // 市场数据回调
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        tick_count_++;

        if (state_ == State::DONE) return;

        // 初始化价格信息（从 MDStockStruct 获取）
        if (prev_close_ == 0 && stock.preclosepx > 0) {
            prev_close_ = static_cast<uint32_t>(stock.preclosepx);
            limit_up_price_ = calculate_limit_up_price(prev_close_);
            activate_price_ = calculate_threshold_price(prev_close_, activate_threshold_);
            sleep_price_ = calculate_threshold_price(prev_close_, sleep_threshold_);

            LOG_M_DEBUG("DabanStrategy initialized prices: {} | prev_close={:.4f} | "
                       "limit_up={:.4f} | activate={:.4f} | sleep={:.4f}",
                       symbol,
                       symbol_utils::int_to_price(prev_close_),
                       symbol_utils::int_to_price(limit_up_price_),
                       symbol_utils::int_to_price(activate_price_),
                       symbol_utils::int_to_price(sleep_price_));
        }

        // 过滤集合竞价（9:30前不触发任何交易逻辑）
        if (stock.mdtime < MARKET_OPEN_TIME) return;
        if (check_close(stock.mdtime)) return;

        if (state_ == State::SLEEPING || state_ == State::ACTIVE) {
            check_tick_state_switch(stock);

            // 入场阶段保底触发（逐笔行情开盘前可能延迟）
            if (state_ == State::ACTIVE && limit_up_price_ > 0 && !signal_triggered_) {
                // 条件1: 卖一价==涨停价 且 成交量 > 卖一量 * exit_volume_ratio_
                if (stock.sellpricequeue[0] == static_cast<int64_t>(limit_up_price_) &&
                    stock.sellorderqtyqueue[0] > 0 &&
                    stock.totalvolumetrade > static_cast<int64_t>(stock.sellorderqtyqueue[0] * exit_volume_ratio_)) {
                    LOG_M_INFO("DabanStrategy BACKUP BUY (tick cond1): {} | "
                              "ask1_price={} == limit_up | volume={} > ask1_qty={}*{:.0f}%",
                              symbol, stock.sellpricequeue[0],
                              stock.totalvolumetrade, stock.sellorderqtyqueue[0], exit_volume_ratio_ * 100);
                    emit_buy_signal_from_tick(stock.mdtime);
                }
                // 条件2: 卖一价==涨停价 且 卖一金额 <= exit_min_bid_amount_
                else if (stock.sellpricequeue[0] == static_cast<int64_t>(limit_up_price_) &&
                         stock.sellorderqtyqueue[0] > 0) {
                    double ask1_amount = static_cast<double>(stock.sellorderqtyqueue[0]) *
                                         symbol_utils::int_to_price(limit_up_price_);
                    if (ask1_amount <= exit_min_bid_amount_) {
                        LOG_M_INFO("DabanStrategy BACKUP BUY (tick cond2): {} | "
                                  "ask1_price={} == limit_up | ask1_amount={:.0f} <= {:.0f}",
                                  symbol, stock.sellpricequeue[0], ask1_amount, exit_min_bid_amount_);
                        emit_buy_signal_from_tick(stock.mdtime);
                    }
                }
                // 条件3: 买一价==涨停价 且 买一量>0（已封板）
                else if (stock.buypricequeue[0] == static_cast<int64_t>(limit_up_price_) &&
                         stock.buyorderqtyqueue[0] > 0) {
                    LOG_M_INFO("DabanStrategy BACKUP BUY (tick cond3): {} | "
                              "bid1_price={} == limit_up | bid1_qty={}",
                              symbol, stock.buypricequeue[0], stock.buyorderqtyqueue[0]);
                    emit_buy_signal_from_tick(stock.mdtime);
                }
            }
        }

        // EXIT_ARMED 阶段: tick 级别炸板检测
        // 使用 else if: 防止同一 tick 内 BUY 触发后立即进入 SELL 检测
        else if (state_ == State::EXIT_ARMED && limit_up_price_ > 0) {
            int64_t bid1_qty = stock.buyorderqtyqueue[0];
            int64_t bid1_price = stock.buypricequeue[0];
            bool at_limit_up = (bid1_price == static_cast<int64_t>(limit_up_price_));

            // 条件1: 在涨停，但成交量 > 买一量 * exit_volume_ratio_（封单被快速消耗）
            if (at_limit_up && bid1_qty > 0 &&
                stock.totalvolumetrade > static_cast<int64_t>(bid1_qty * exit_volume_ratio_)) {
                LOG_M_INFO("DabanStrategy BREAK (tick cond1): {} | "
                          "volume={} > bid1_qty={}*{:.0f}%",
                          symbol, stock.totalvolumetrade, bid1_qty, exit_volume_ratio_ * 100);
                emit_sell_signal(stock.mdtime);
            }
            // 条件2: 在涨停，但买一金额 <= exit_min_bid_amount_
            else if (at_limit_up && bid1_qty > 0) {
                // 买一金额 = 买一量 × 涨停价（元）
                double bid1_amount = static_cast<double>(bid1_qty) *
                                     symbol_utils::int_to_price(limit_up_price_);
                if (bid1_amount <= exit_min_bid_amount_) {
                    LOG_M_INFO("DabanStrategy BREAK (tick cond2): {} | "
                              "bid1_amount={:.0f} <= {:.0f}",
                              symbol, bid1_amount, exit_min_bid_amount_);
                    emit_sell_signal(stock.mdtime);
                }
            }
            // 条件3: lastpx < 涨停价（已开板）
            else if (static_cast<uint32_t>(stock.lastpx) < limit_up_price_) {
                LOG_M_INFO("DabanStrategy BREAK (tick cond3): {} | "
                          "lastpx={} < limit_up={}",
                          symbol, stock.lastpx, limit_up_price_);
                emit_sell_signal(stock.mdtime);
            }
        }
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        int32_t current_time = order.mdtime;
        if (state_ == State::DONE) return;
        if (current_time < MARKET_OPEN_TIME) return;
        if (check_close(current_time)) return;
        if (prev_close_ == 0) return;  // 尚未初始化价格

        // 延迟过滤：本地接收时间比行情时间晚超过阈值则跳过
        if (time_util::is_delayed(order.local_recv_timestamp, order.mdtime, max_delay_ms_)) return;

        order_count_++;

        // EXIT_ARMED 阶段: 喂数据给炸板检测器
        if (state_ == State::EXIT_ARMED) {
            if (!flow_detector_.is_synced()) {
                flow_detector_.sync_from_book(book, limit_up_price_, symbol);
            }
            flow_detector_.on_order(order, limit_up_price_);
            if (flow_detector_.check(current_time, book, limit_up_price_)) {
                uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price_);
                uint64_t flow_sum = flow_detector_.get_flow_sum(current_time);
                LOG_M_INFO("DabanStrategy FLOW TRIGGER: {} | flow_sum={} | current_bid={} | ratio={:.1f}%",
                           symbol, flow_sum, current_bid,
                           current_bid > 0 ? (flow_sum * 100.0 / current_bid) : 100.0);
                emit_sell_signal(current_time);
            }
            return;
        }

        // 入场阶段: 原有逻辑
        bool is_cancel = (order.ordertype == 10);  // 上海撤单

        if (is_cancel) {
            handle_cancel(order);
        } else {
            handle_order(order);
        }

        // 周期性任务
        if (current_time - last_tick_time_ >= static_cast<int32_t>(tick_interval_ms_)) {
            process_periodic_tasks(current_time, book);
            last_tick_time_ = current_time;
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        int32_t current_time = txn.mdtime;
        if (state_ == State::DONE) return;
        if (current_time < MARKET_OPEN_TIME) return;
        if (check_close(current_time)) return;
        if (prev_close_ == 0) return;

        // 延迟过滤：本地接收时间比行情时间晚超过阈值则跳过
        if (time_util::is_delayed(txn.local_recv_timestamp, txn.mdtime, max_delay_ms_)) return;

        transaction_count_++;

        // EXIT_ARMED 阶段: 喂数据给炸板检测器
        if (state_ == State::EXIT_ARMED) {
            if (!flow_detector_.is_synced()) {
                flow_detector_.sync_from_book(book, limit_up_price_, symbol);
            }
            flow_detector_.on_transaction(txn, limit_up_price_);
            if (flow_detector_.check(current_time, book, limit_up_price_)) {
                uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price_);
                uint64_t flow_sum = flow_detector_.get_flow_sum(current_time);
                LOG_M_INFO("DabanStrategy FLOW TRIGGER: {} | flow_sum={} | current_bid={} | ratio={:.1f}%",
                           symbol, flow_sum, current_bid,
                           current_bid > 0 ? (flow_sum * 100.0 / current_bid) : 100.0);
                emit_sell_signal(current_time);
            }
            return;
        }

        // 入场阶段: 原有逻辑
        bool is_cancel = (txn.tradetype != 0);

        if (is_cancel) {
            handle_cancel_from_transaction(txn);
        } else {
            handle_trade(txn);
        }

        // 周期性任务
        if (current_time - last_tick_time_ >= static_cast<int32_t>(tick_interval_ms_)) {
            process_periodic_tasks(current_time, book);
            last_tick_time_ = current_time;
        }
    }

private:
    // ============ 入场阶段: 内部事件处理 ============

    void handle_order(const MDOrderStruct& order) {
        // 只关注买单
        if (order.orderbsflag != 1) return;  // 1=买

        // 非激活状态不处理委托
        if (state_ != State::ACTIVE) return;

        // 已触发不再累计
        if (signal_triggered_) return;

        uint32_t order_price = static_cast<uint32_t>(order.orderprice);

        // 只关注涨停价买单
        if (order_price == limit_up_price_) {
            LimitUpBid bid{
                static_cast<uint64_t>(order.orderno),
                order.mdtime,
                static_cast<uint64_t>(order.orderqty)
            };

            limit_up_bids_.push_back(bid);
            auto it = limit_up_bids_.end();
            --it;
            bid_index_[bid.order_seq] = it;

            n_ += bid.volume;
            max_n_ = std::max(max_n_, n_);

            LOG_M_DEBUG("DabanStrategy BID: {} | seq={} | time={} | volume={} | n={}",
                       symbol, bid.order_seq, order.mdtime, bid.volume, n_);
        }
    }

    void handle_trade(const MDTransactionStruct& txn) {
        // 记录成交到10ms窗口
        TradeRecord rec{
            txn.mdtime,
            static_cast<uint32_t>(txn.tradeprice),
            static_cast<uint64_t>(txn.tradeqty)
        };
        recent_trades_.push_back(rec);

        // 调试日志
        double gain = (rec.price > prev_close_) ?
            (static_cast<double>(rec.price - prev_close_) * 100.0 / prev_close_) : 0.0;
        LOG_M_DEBUG("DabanStrategy TRADE: {} | time={} | price={:.4f} ({:+.2f}%) | volume={}",
                   symbol, txn.mdtime,
                   symbol_utils::int_to_price(rec.price),
                   gain, rec.volume);
    }

    void handle_cancel(const MDOrderStruct& order) {
        // 从委托消息处理撤单
        if (state_ != State::ACTIVE) return;

        uint64_t order_seq = static_cast<uint64_t>(order.orderno);
        auto it = bid_index_.find(order_seq);
        if (it != bid_index_.end()) {
            auto bid_it = it->second;
            uint64_t cancel_volume = static_cast<uint64_t>(order.orderqty);

            if (n_ >= cancel_volume) {
                n_ -= cancel_volume;
            } else {
                n_ = 0;
            }

            LOG_M_DEBUG("DabanStrategy CANCEL (order): {} | seq={} | volume={} | n={}",
                       symbol, order_seq, cancel_volume, n_);
        }
    }

    void handle_cancel_from_transaction(const MDTransactionStruct& txn) {
        // 从成交消息处理撤单
        if (state_ != State::ACTIVE) return;

        // 尝试用买单序号查找
        uint64_t bid_seq = static_cast<uint64_t>(txn.tradebuyno);

        auto it = bid_index_.find(bid_seq);
        if (it != bid_index_.end()) {
            uint64_t cancel_volume = static_cast<uint64_t>(txn.tradeqty);

            if (n_ >= cancel_volume) {
                n_ -= cancel_volume;
            } else {
                n_ = 0;
            }

            LOG_M_DEBUG("DabanStrategy CANCEL (txn): {} | seq={} | volume={} | n={}",
                       symbol, bid_seq, cancel_volume, n_);
        }
    }

    // ============ 周期性任务 ============

    void process_periodic_tasks(int32_t current_time, const FastOrderBook& book) {
        // 1. 清理过期成交（10ms窗口外）
        cleanup_old_trades(current_time);

        // 2. 状态转换检查
        check_state_transition(current_time);

        // 3. 如果ACTIVE，清理1000ms窗口外的委托
        if (state_ == State::ACTIVE) {
            cleanup_old_bids(current_time);
        }

        // 4. 检测涨停时间（只检测一次）
        if (!limit_up_detected_) {
            detect_limit_up(current_time, book);
        }

        // 5. 触发条件检查（ACTIVE且未触发）
        if (state_ == State::ACTIVE && !signal_triggered_) {
            if (check_trigger(current_time, book)) {
                emit_buy_signal(current_time, book);
            }
        }
    }

    // ============ 状态转换 ============

    bool check_close(int32_t current_time) {
        if (current_time < MARKET_CLOSE_TIME) {
            return false;
        }

        if (state_ == State::DONE) {
            return true;
        }

        LOG_M_INFO("DabanStrategy CLOSE: {} | {} -> DONE at time={}",
                  symbol, state_to_string(state_),
                  time_util::format_mdtime(current_time));
        state_ = State::DONE;
        return true;
    }

    void check_tick_state_switch(const MDStockStruct& stock) {
        int64_t ask1_price = stock.sellpricequeue[0];
        int64_t bid1_price = stock.buypricequeue[0];

        if (ask1_price > 0 && prev_close_ > 0) {
            double ask1_gain = static_cast<double>(ask1_price - static_cast<int64_t>(prev_close_)) /
                               static_cast<double>(prev_close_);

            if (state_ == State::SLEEPING && ask1_gain > activate_threshold_) {
                activate(stock.mdtime);
            } else if (state_ == State::ACTIVE && ask1_gain < sleep_threshold_) {
                sleep();
            }
        } else if (ask1_price == 0) {
            // 无卖单，可能涨停封死
            if (state_ == State::SLEEPING &&
                bid1_price == static_cast<int64_t>(limit_up_price_) &&
                bid1_price > 0) {
                activate(stock.mdtime);
            }
        }
    }

    void check_state_transition(int32_t current_time) {
        bool has_qualified = has_qualified_trades();
        bool all_below_sleep = all_trades_below_sleep();

        State old_state = state_;

        switch (state_) {
        case State::SLEEPING:
            if (has_qualified) {
                activate(current_time);
            }
            break;

        case State::ACTIVE:
            if (all_below_sleep) {
                sleep();
            }
            // [7%, 8%)区间：保持ACTIVE状态
            break;

        case State::EXIT_ARMED:
            // BUY_SIGNAL 已发出，不回退
            break;

        case State::DONE:
            // 终态
            break;
        }

        // 记录状态变化
        if (old_state != state_) {
            LOG_M_INFO("DabanStrategy STATE_CHANGE: {} | {} -> {} at time={}",
                      symbol, state_to_string(old_state),
                      state_to_string(state_), current_time);
        }
    }

    bool has_qualified_trades() const {
        for (const auto& trade : recent_trades_) {
            if (trade.price > activate_price_) {
                return true;
            }
        }
        return false;
    }

    bool all_trades_below_sleep() const {
        if (recent_trades_.empty()) {
            return false;  // 无成交，不触发休眠
        }

        for (const auto& trade : recent_trades_) {
            if (trade.price >= sleep_price_) {
                return false;
            }
        }
        return true;
    }

    void activate(int32_t current_time) {
        state_ = State::ACTIVE;
        t0_ = current_time;
        signal_triggered_ = false;
        activate_count_++;

        LOG_M_INFO("DabanStrategy ACTIVATE: {} | t0={} | qualified_trades={}",
                  symbol, t0_, recent_trades_.size());
    }

    void sleep() {
        state_ = State::SLEEPING;
        t0_ = 0;
        n_ = 0;
        signal_triggered_ = false;
        limit_up_detected_ = false;
        limit_up_time_ = 0;

        // 清空窗口
        limit_up_bids_.clear();
        bid_index_.clear();

        sleep_count_++;

        LOG_M_INFO("DabanStrategy SLEEP: {} | all trades below {:.2f}%",
                  symbol, sleep_threshold_ * 100);
    }

    // ============ 入场触发检测 ============

    uint64_t calculate_m(const FastOrderBook& book) const {
        auto best_ask = book.get_best_ask();
        if (!best_ask.has_value()) {
            return 0;
        }

        // 实时计算：卖一价到涨停价的卖单总量
        uint64_t m = book.get_ask_volume_in_range(best_ask.value(), limit_up_price_);
        return m;
    }

    bool check_trigger(int32_t current_time, const FastOrderBook& book) {
        uint64_t m = calculate_m(book);
        max_m_ = std::max(max_m_, m);

        if (m == 0) {
            // 没有卖单，可能已涨停封死，不触发
            LOG_M_DEBUG("DabanStrategy CHECK_TRIGGER: {} | m=0 (no asks), skip trigger", symbol);
            return false;
        }

        double ratio = static_cast<double>(n_) / m;

        LOG_M_DEBUG("DabanStrategy CHECK_TRIGGER: {} | time={} | n={} | m={} | ratio={:.4f}",
                   symbol, current_time, n_, m, ratio);

        return n_ > (m / 2);
    }

    // ============ 信号发送 ============

    void emit_buy_signal(int32_t current_time, const FastOrderBook& book) {
        signal_triggered_ = true;
        signal_triggered_time_ = current_time;
        state_ = State::EXIT_ARMED;
        trigger_count_++;

        uint64_t m = calculate_m(book);
        double ratio = (m > 0) ? (static_cast<double>(n_) / m) : 0.0;

        LOG_BIZ(BIZ_STRA,
                "{} | BUY | MARKET_TIME={} | Price={} | "
                "n={} | m={} | ratio={:.4f} | t0={} | Reason=打板策略触发",
                symbol, time_util::format_mdtime(current_time),
                price_util::format_price_display(limit_up_price_),
                n_, m, ratio, t0_);

        LOG_M_INFO("DabanStrategy BUY TRIGGER: {} | time={} | limit_up_time={} | "
                  "n={} | m={} | ratio={:.4f} | t0={}",
                  symbol, current_time, limit_up_time_,
                  n_, m, ratio, t0_);

        // 发送买入信号
        TradeSignal signal;
        signal.symbol = this->symbol;
        signal.side = TradeSignal::Side::BUY;
        signal.price = limit_up_price_;
        signal.quantity = 0;  // 不传数量，对面自行决定
        signal.trigger_time = current_time;
        signal.strategy_name = this->name;
        signal.strategy_type_id = strategy_type_id;
        place_order(signal);

        LOG_M_INFO("DabanStrategy -> EXIT_ARMED: {} | monitoring for exit", symbol);
    }

    void emit_buy_signal_from_tick(int32_t current_time) {
        signal_triggered_ = true;
        signal_triggered_time_ = current_time;
        state_ = State::EXIT_ARMED;
        trigger_count_++;

        LOG_BIZ(BIZ_STRA,
                "{} | BUY | MARKET_TIME={} | Price={} | "
                "Reason=打板策略保底触发(tick)",
                symbol, time_util::format_mdtime(current_time),
                price_util::format_price_display(limit_up_price_));

        LOG_M_INFO("DabanStrategy BACKUP BUY TRIGGER: {} | time={}",
                  symbol, current_time);

        // 发送买入信号
        TradeSignal signal;
        signal.symbol = this->symbol;
        signal.side = TradeSignal::Side::BUY;
        signal.price = limit_up_price_;
        signal.quantity = 0;
        signal.trigger_time = current_time;
        signal.strategy_name = this->name;
        signal.strategy_type_id = strategy_type_id;
        place_order(signal);

        LOG_M_INFO("DabanStrategy -> EXIT_ARMED: {} | monitoring for exit (backup)", symbol);
    }

    void emit_sell_signal(int32_t trigger_time) {
        if (state_ != State::EXIT_ARMED) return;

        state_ = State::DONE;

        LOG_BIZ(BIZ_STRA,
                "{} | SELL | MARKET_TIME={} | Price={} | "
                "flow_events={} | Reason=炸板卖出",
                symbol, time_util::format_mdtime(trigger_time),
                price_util::format_price_display(limit_up_price_),
                flow_detector_.flow_event_count());

        LOG_M_INFO("========================================");
        LOG_M_INFO("DabanStrategy SELL TRIGGERED: {}", symbol);
        LOG_M_INFO("  - Time: {}", time_util::format_mdtime(trigger_time));
        LOG_M_INFO("  - Flow events: {}", flow_detector_.flow_event_count());
        LOG_M_INFO("========================================");

        // 发送卖出信号
        TradeSignal signal;
        signal.symbol = this->symbol;
        signal.side = TradeSignal::Side::SELL;
        signal.price = limit_up_price_;
        signal.quantity = 0;  // 不传数量，对面自行决定
        signal.trigger_time = trigger_time;
        signal.strategy_name = this->name;
        signal.strategy_type_id = strategy_type_id;
        place_order(signal);
    }

    // ============ 窗口清理 ============

    void cleanup_old_bids(int32_t current_time) {
        // 计算窗口起点：[max(t0, t-1000), t]
        int32_t window_start = (current_time >= t0_ + static_cast<int32_t>(max_window_ms_)) ?
                            (current_time - static_cast<int32_t>(max_window_ms_)) : t0_;

        // 从头部删除过期委托
        while (!limit_up_bids_.empty() &&
               limit_up_bids_.front().time < window_start) {
            auto& bid = limit_up_bids_.front();
            if (n_ >= bid.volume) {
                n_ -= bid.volume;
            } else {
                n_ = 0;
            }
            bid_index_.erase(bid.order_seq);
            limit_up_bids_.pop_front();
        }
    }

    void cleanup_old_trades(int32_t current_time) {
        // 删除10ms窗口外的成交：[t-10, t)
        int32_t window_start = (current_time >= static_cast<int32_t>(trade_window_ms_)) ?
                            (current_time - static_cast<int32_t>(trade_window_ms_)) : 0;

        while (!recent_trades_.empty() &&
               recent_trades_.front().time < window_start) {
            recent_trades_.pop_front();
        }
    }

    // ============ 涨停检测 ============

    void detect_limit_up(int32_t current_time, const FastOrderBook& book) {
        auto best_bid = book.get_best_bid();
        if (!best_bid.has_value()) {
            return;
        }

        // 买一价=涨停价 且 没有卖单
        if (best_bid.value() == limit_up_price_ && !book.get_best_ask().has_value()) {
            // 买一档总量>0
            uint64_t bid_volume = book.get_bid_volume_at_price(limit_up_price_);

            if (bid_volume > 0) {
                limit_up_time_ = current_time;
                limit_up_detected_ = true;

                LOG_M_INFO("DabanStrategy LIMIT_UP detected: {} | time={} | volume={}",
                          symbol, limit_up_time_, bid_volume);
            }
        }
    }

    // ============ 辅助函数 ============

    void print_status() const {
        LOG_M_INFO("=== DabanStrategy Status: {} ===", symbol);
        LOG_M_INFO("State: {} | t0: {} | n: {} | Signal triggered: {}",
                  state_to_string(state_), t0_, n_,
                  signal_triggered_ ? "Yes" : "No");
        LOG_M_INFO("Prices: prev_close={:.4f} | limit_up={:.4f}",
                  symbol_utils::int_to_price(prev_close_),
                  symbol_utils::int_to_price(limit_up_price_));
        LOG_M_INFO("Stats: activates={} | sleeps={} | triggers={} | max_n={} | max_m={} | flow_events={}",
                  activate_count_, sleep_count_, trigger_count_, max_n_, max_m_,
                  flow_detector_.flow_event_count());
    }

    static std::string state_to_string(State state) {
        switch (state) {
        case State::SLEEPING:     return "SLEEPING";
        case State::ACTIVE:       return "ACTIVE";
        case State::EXIT_ARMED:   return "EXIT_ARMED";
        case State::DONE:         return "DONE";
        default:                  return "UNKNOWN";
        }
    }

    // ============ 价格计算 ============

    static uint32_t calculate_limit_up_price(uint32_t prev_close) {
        // 昨收×1.1，四舍五入
        // 内部格式是 ×10000，所以直接乘以 1.1
        uint64_t temp = static_cast<uint64_t>(prev_close) * 110;
        uint32_t result = static_cast<uint32_t>((temp + 50) / 100);
        return result;
    }

    static uint32_t calculate_threshold_price(uint32_t prev_close, double percent) {
        // 昨收×(1+percent)，四舍五入
        double temp = prev_close * (1.0 + percent);
        return static_cast<uint32_t>(temp + 0.5);
    }
};

#undef LOG_MODULE

#endif // DABAN_STRATEGY_H
