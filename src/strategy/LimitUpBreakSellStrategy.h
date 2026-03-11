#ifndef LIMIT_UP_BREAK_SELL_STRATEGY_H
#define LIMIT_UP_BREAK_SELL_STRATEGY_H

#include "strategy_base.h"
#include "trade_signal.h"
#include "market_data_structs_aligned.h"
#include "logger.h"
#include "utils/time_util.h"
#include "utils/price_util.h"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <deque>

#define LOG_MODULE MOD_STRATEGY

// ==========================================
// LimitUpBreakSellStrategy - 涨停开板卖出策略
//
// 指定时间后，对涨停板股票监控封单流出。
// 当滑动窗口内 (撤单量+成交量) > 当前封单量 * threshold% 时，
// 发出卖出信号。
//
// 可配置参数 (通过 params JSON):
//   threshold  - 流出阈值百分比 (默认60)
//   window_ms  - 滑动窗口长度ms (默认300)
//   min_bid    - 封单量下限 (默认1000)
//   start_time - 监控开始时间 (默认"13:30")
//
// 状态机:
//   INACTIVE --[time >= start_time AND lastpx == maxpx]--> MONITORING
//   MONITORING --[flow_condition]--> TRIGGERED (terminal)
//   MONITORING --[lastpx < maxpx (backup)]--> TRIGGERED (terminal)
// ==========================================

class LimitUpBreakSellStrategy : public Strategy {
private:
    // ==========================================
    // 状态机
    // ==========================================
    enum class State {
        INACTIVE,      // 13:30前 / 未涨停
        MONITORING,    // 13:30后确认涨停，逐笔追踪
        TRIGGERED      // 卖出信号已发，终态
    };

    State state_ = State::INACTIVE;

    // ==========================================
    // 价格信息
    // ==========================================
    uint32_t limit_up_price_ = 0;   // 涨停价 (maxpx)
    int64_t last_px_ = 0;           // 最新价

    // ==========================================
    // 可配置参数 (可通过 params JSON 覆盖)
    // ==========================================
    int32_t start_time_ = 133000000;    // 监控开始时间 (MDTime格式, 默认13:30)
    uint32_t flow_threshold_pct_ = 60;  // 封单流出触发阈值 (百分比)
    int32_t flow_window_ms_ = 300;      // 滑动窗口长度 (毫秒)
    uint64_t min_bid_volume_ = 1000;    // 封单量下限 (低于此值直接触发)

    // ==========================================
    // 涨停价买单追踪
    // ==========================================
    std::unordered_map<uint64_t, uint64_t> limit_up_bid_orders_;  // orderno -> volume

    // ==========================================
    // 200ms 滑动窗口: 封单流出事件
    // ==========================================
    struct FlowEvent {
        int32_t time;       // MDTime
        uint64_t volume;    // 撤单量或成交量
    };
    std::deque<FlowEvent> flow_window_;

    // ==========================================
    // OrderBook 同步标记
    // ==========================================
    bool book_synced_ = false;  // 进入 MONITORING 后是否已从 OrderBook 同步

    // ==========================================
    // 统计
    // ==========================================
    uint64_t tick_count_ = 0;
    uint64_t order_count_ = 0;
    uint64_t txn_count_ = 0;
    uint64_t flow_event_count_ = 0;

public:
    explicit LimitUpBreakSellStrategy(const std::string& strategy_name,
                                      const std::string& sym = "",
                                      const std::string& params = "") {
        this->name = strategy_name;
        this->symbol = sym;
        if (!params.empty()) {
            try {
                auto j = nlohmann::json::parse(params);
                if (j.contains("threshold"))  flow_threshold_pct_ = j["threshold"].get<uint32_t>();
                if (j.contains("window_ms"))  flow_window_ms_ = j["window_ms"].get<int32_t>();
                if (j.contains("min_bid"))    min_bid_volume_ = j["min_bid"].get<uint64_t>();
                if (j.contains("start_time")) {
                    // 支持 "13:30" 格式或 MDTime 整数
                    auto& st = j["start_time"];
                    if (st.is_string()) {
                        std::string s = st.get<std::string>();
                        // 解析 "HH:MM" -> HHMMSSmmm
                        int h = 0, m = 0;
                        if (sscanf(s.c_str(), "%d:%d", &h, &m) == 2) {
                            start_time_ = h * 10000000 + m * 100000;
                        }
                    } else if (st.is_number()) {
                        start_time_ = st.get<int32_t>();
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // params 不是有效 JSON，使用默认值
            }
        }
    }

    virtual ~LimitUpBreakSellStrategy() = default;

    void on_start() override {
        LOG_M_INFO("LimitUpBreakSellStrategy started: {} | symbol={} | threshold={}% | window={}ms | min_bid={} | start_time={}",
                   name, symbol, flow_threshold_pct_, flow_window_ms_, min_bid_volume_,
                   time_util::format_mdtime(start_time_));
    }

    void on_stop() override {
        LOG_M_INFO("LimitUpBreakSellStrategy stopped: {} | ticks={} | orders={} | txns={} | flows={} | state={}",
                   name, tick_count_, order_count_, txn_count_, flow_event_count_,
                   state_ == State::TRIGGERED ? "TRIGGERED" :
                   state_ == State::MONITORING ? "MONITORING" : "INACTIVE");
    }

    // ==========================================
    // on_tick: 初始化价格，状态转换
    // ==========================================
    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        tick_count_++;

        if (state_ == State::TRIGGERED) return;

        // 初始化涨停价
        if (limit_up_price_ == 0 && stock.maxpx > 0) {
            limit_up_price_ = static_cast<uint32_t>(stock.maxpx);
            LOG_M_INFO("{} | limit_up_price initialized: {} ({})",
                       symbol, limit_up_price_, price_util::format_price_display(limit_up_price_));
        }

        last_px_ = stock.lastpx;

        if (state_ == State::INACTIVE) {
            // 检查是否满足进入 MONITORING 的条件
            if (stock.mdtime >= start_time_ && limit_up_price_ > 0 &&
                static_cast<uint32_t>(stock.lastpx) == limit_up_price_) {
                state_ = State::MONITORING;
                LOG_M_INFO("{} | INACTIVE -> MONITORING | time={} | lastpx={} | maxpx={}",
                           symbol, time_util::format_mdtime(stock.mdtime),
                           stock.lastpx, limit_up_price_);
                LOG_BIZ("SIGNAL", "MONITORING | {} | time={} | limit_up_price={}",
                        symbol, time_util::format_mdtime(stock.mdtime),
                        price_util::format_price_display(limit_up_price_));
            }
        } else if (state_ == State::MONITORING) {
            // Backup: 如果 lastpx 已经低于涨停价，说明已经开板
            if (static_cast<uint32_t>(stock.lastpx) < limit_up_price_) {
                LOG_M_INFO("{} | lastpx < maxpx detected in on_tick | lastpx={} | maxpx={}",
                           symbol, stock.lastpx, limit_up_price_);
                emit_sell_signal(stock.mdtime);
            }
        }
    }

    // ==========================================
    // on_order: 追踪涨停价买单 + 撤单流出
    // ==========================================
    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        order_count_++;

        if (state_ != State::MONITORING) return;
        if (limit_up_price_ == 0) return;

        // 首次进入 MONITORING: 从 OrderBook 同步涨停价上的已有买单
        if (!book_synced_) sync_from_book(book);

        // 买单 at 涨停价 -> 记录
        if (order.orderbsflag == 1 &&  // 买方向
            static_cast<uint32_t>(order.orderprice) == limit_up_price_) {

            // 订单 ID: SZ 用 orderindex, SH 用 orderno（与 FastOrderBook 一致）
            uint64_t order_id = (order.orderno != 0)
                ? static_cast<uint64_t>(order.orderno)
                : static_cast<uint64_t>(order.orderindex);

            if (order.ordertype == 10) {
                // 上海撤单 (ordertype == 10): 查 order_id，命中则加入 flow_window_
                auto it = limit_up_bid_orders_.find(order_id);
                if (it != limit_up_bid_orders_.end()) {
                    uint64_t cancel_vol = it->second;
                    flow_window_.push_back({order.mdtime, cancel_vol});
                    flow_event_count_++;
                    limit_up_bid_orders_.erase(it);
                    check_flow_condition(order.mdtime, book);
                }
            } else {
                // 新买单: 记录到 limit_up_bid_orders_
                limit_up_bid_orders_[order_id] = static_cast<uint64_t>(order.orderqty);
            }
        }
    }

    // ==========================================
    // on_transaction: 追踪成交流出 + SZ撤单流出
    // ==========================================
    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        txn_count_++;

        if (state_ != State::MONITORING) return;
        if (limit_up_price_ == 0) return;

        // 首次进入 MONITORING: 从 OrderBook 同步涨停价上的已有买单
        if (!book_synced_) sync_from_book(book);

        if (txn.tradetype == 0) {
            // 成交 at 涨停价 -> 加入 flow_window_
            if (static_cast<uint32_t>(txn.tradeprice) == limit_up_price_) {
                flow_window_.push_back({txn.mdtime, static_cast<uint64_t>(txn.tradeqty)});
                flow_event_count_++;

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

                check_flow_condition(txn.mdtime, book);
            }
        } else {
            // 深圳撤单 (tradetype != 0): 查 tradebuyno，命中则加入 flow_window_
            auto it = limit_up_bid_orders_.find(static_cast<uint64_t>(txn.tradebuyno));
            if (it != limit_up_bid_orders_.end()) {
                uint64_t cancel_vol = static_cast<uint64_t>(txn.tradeqty);
                flow_window_.push_back({txn.mdtime, cancel_vol});
                flow_event_count_++;

                if (cancel_vol >= it->second) {
                    limit_up_bid_orders_.erase(it);
                } else {
                    it->second -= cancel_vol;
                }

                check_flow_condition(txn.mdtime, book);
            }
        }
    }

private:
    // ==========================================
    // 从 OrderBook 同步涨停价上的已有买单
    // ==========================================
    void sync_from_book(const FastOrderBook& book) {
        book_synced_ = true;
        uint64_t synced_volume = 0;
        uint64_t book_bid_volume = book.get_bid_volume_at_price(limit_up_price_);
        size_t synced_count = 0;
        book.for_each_bid_order_at_price(limit_up_price_, [this, &synced_volume, &synced_count](uint64_t seq, uint32_t volume) {
            limit_up_bid_orders_[seq] = static_cast<uint64_t>(volume);
            synced_volume += static_cast<uint64_t>(volume);
            ++synced_count;
        });
        LOG_M_INFO("{} | synced {} bid orders from OrderBook at limit_up_price={} | synced_volume={} | book_bid_volume={}",
                   symbol, synced_count,
                   price_util::format_price_display(limit_up_price_),
                   synced_volume, book_bid_volume);
        if (synced_volume != book_bid_volume) {
            LOG_M_WARNING("{} | sync mismatch at limit_up_price={} | synced_count={} | synced_volume={} | book_bid_volume={}",
                          symbol, price_util::format_price_display(limit_up_price_),
                          synced_count, synced_volume, book_bid_volume);
        }
    }

    // ==========================================
    // 检查封单流出触发条件
    // ==========================================
    void check_flow_condition(int32_t current_time, const FastOrderBook& book) {
        if (state_ != State::MONITORING) return;

        // 清理窗口外的条目
        while (!flow_window_.empty()) {
            if (time_util::is_within_ms(flow_window_.front().time, current_time, flow_window_ms_)) {
                break;
            }
            flow_window_.pop_front();
        }

        // 计算窗口内流出总量
        uint64_t flow_sum = 0;
        for (const auto& ev : flow_window_) {
            flow_sum += ev.volume;
        }

        // 获取当前 OrderBook 涨停价买委托量
        uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price_);

        // 触发条件: current_bid <= min_bid_volume_ OR flow_sum > current_bid * threshold%
        if (current_bid <= min_bid_volume_ || flow_sum * 100 > current_bid * flow_threshold_pct_) {
            LOG_M_INFO("{} | FLOW TRIGGER | flow_sum={} | current_bid={} | ratio={:.1f}% | window_size={}",
                       symbol, flow_sum, current_bid,
                       current_bid > 0 ? (flow_sum * 100.0 / current_bid) : 100.0,
                       flow_window_.size());
            emit_sell_signal(current_time);
        }
    }

    // ==========================================
    // 发出卖出信号
    // ==========================================
    void emit_sell_signal(int32_t trigger_time) {
        if (state_ == State::TRIGGERED) return;

        state_ = State::TRIGGERED;

        LOG_BIZ("SIGNAL",
                "SELL SIGNAL | {} | Time={} | Price={} | state=TRIGGERED",
                symbol, time_util::format_mdtime(trigger_time),
                price_util::format_price_display(limit_up_price_));

        LOG_M_INFO("========================================");
        LOG_M_INFO("SELL SIGNAL TRIGGERED: {}", symbol);
        LOG_M_INFO("  - Time: {}", time_util::format_mdtime(trigger_time));
        LOG_M_INFO("  - Price: {}", price_util::format_price_display(limit_up_price_));
        LOG_M_INFO("  - Flow events: {}", flow_event_count_);
        LOG_M_INFO("========================================");

        TradeSignal signal;
        signal.symbol = this->symbol;
        signal.side = TradeSignal::Side::SELL;
        signal.price = limit_up_price_;
        signal.quantity = 0;  // 不传数量，dealer 自行决定
        signal.trigger_time = trigger_time;
        signal.strategy_name = this->name;
        signal.strategy_type_id = strategy_type_id;
        place_order(signal);
    }
};

#undef LOG_MODULE
#endif // LIMIT_UP_BREAK_SELL_STRATEGY_H
