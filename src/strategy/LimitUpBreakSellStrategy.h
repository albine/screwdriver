#ifndef LIMIT_UP_BREAK_SELL_STRATEGY_H
#define LIMIT_UP_BREAK_SELL_STRATEGY_H

#include "strategy_base.h"
#include "trade_signal.h"
#include "market_data_structs_aligned.h"
#include "logger.h"
#include "utils/time_util.h"
#include "utils/price_util.h"
#include "utils/limit_up_flow_detector.h"
#include <nlohmann/json.hpp>
#include <string>

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
    // 可配置参数
    // ==========================================
    int32_t start_time_ = 133000000;    // 监控开始时间 (MDTime格式, 默认13:30)

    // ==========================================
    // 封单流出检测器
    // ==========================================
    LimitUpFlowDetector detector_;

    // ==========================================
    // 统计
    // ==========================================
    uint64_t tick_count_ = 0;
    uint64_t order_count_ = 0;
    uint64_t txn_count_ = 0;

public:
    explicit LimitUpBreakSellStrategy(const std::string& strategy_name,
                                      const std::string& sym = "",
                                      const std::string& params = "") {
        this->name = strategy_name;
        this->symbol = sym;

        // 默认检测器参数
        LimitUpFlowDetector::Config detector_config;
        detector_config.flow_threshold_pct = 60;
        detector_config.flow_window_ms = 300;
        detector_config.min_bid_volume = 1000;
        detector_config.cancel_weight = 0.2;

        if (!params.empty()) {
            try {
                auto j = nlohmann::json::parse(params);
                if (j.contains("threshold"))  detector_config.flow_threshold_pct = j["threshold"].get<uint32_t>();
                if (j.contains("window_ms"))  detector_config.flow_window_ms = j["window_ms"].get<int32_t>();
                if (j.contains("min_bid"))    detector_config.min_bid_volume = j["min_bid"].get<uint64_t>();
                if (j.contains("cancel_weight")) detector_config.cancel_weight = j["cancel_weight"].get<double>();
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

        detector_ = LimitUpFlowDetector(detector_config);
    }

    virtual ~LimitUpBreakSellStrategy() = default;

    void on_start() override {
        auto& cfg = detector_.config();
        LOG_M_INFO("LimitUpBreakSellStrategy started: {} | symbol={} | threshold={}% | window={}ms | min_bid={} | cancel_weight={:.2f} | start_time={}",
                   name, symbol, cfg.flow_threshold_pct, cfg.flow_window_ms, cfg.min_bid_volume, cfg.cancel_weight,
                   time_util::format_mdtime(start_time_));
    }

    void on_stop() override {
        LOG_M_INFO("LimitUpBreakSellStrategy stopped: {} | ticks={} | orders={} | txns={} | flows={} | state={}",
                   name, tick_count_, order_count_, txn_count_, detector_.flow_event_count(),
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
        if (!detector_.is_synced()) {
            detector_.sync_from_book(book, limit_up_price_, symbol);
        }

        detector_.on_order(order, limit_up_price_);

        if (detector_.check(order.mdtime, book, limit_up_price_)) {
            uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price_);
            uint64_t flow_sum = detector_.get_flow_sum(order.mdtime);
            LOG_M_INFO("{} | FLOW TRIGGER | flow_sum={} | current_bid={} | ratio={:.1f}% | events={}",
                       symbol, flow_sum, current_bid,
                       current_bid > 0 ? (flow_sum * 100.0 / current_bid) : 100.0,
                       detector_.flow_event_count());
            emit_sell_signal(order.mdtime);
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
        if (!detector_.is_synced()) {
            detector_.sync_from_book(book, limit_up_price_, symbol);
        }

        detector_.on_transaction(txn, limit_up_price_);

        if (detector_.check(txn.mdtime, book, limit_up_price_)) {
            uint64_t current_bid = book.get_bid_volume_at_price(limit_up_price_);
            uint64_t flow_sum = detector_.get_flow_sum(txn.mdtime);
            LOG_M_INFO("{} | FLOW TRIGGER | flow_sum={} | current_bid={} | ratio={:.1f}% | events={}",
                       symbol, flow_sum, current_bid,
                       current_bid > 0 ? (flow_sum * 100.0 / current_bid) : 100.0,
                       detector_.flow_event_count());
            emit_sell_signal(txn.mdtime);
        }
    }

private:
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
        LOG_M_INFO("  - Flow events: {}", detector_.flow_event_count());
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
