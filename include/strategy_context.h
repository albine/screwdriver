#ifndef STRATEGY_CONTEXT_H
#define STRATEGY_CONTEXT_H

#include "trade_signal.h"
#include "logger.h"
#include "symbol_utils.h"
#include <memory>

// 前向声明
class ZmqClient;

/**
 * @brief 策略上下文接口
 *
 * 封装策略与外部交互的操作，实现运算逻辑和下单操作的解耦。
 * 实盘和回测模式使用不同的 Context 实现。
 */
class StrategyContext {
public:
    virtual ~StrategyContext() = default;

    /**
     * @brief 下单
     * @param signal 交易信号
     */
    virtual void place_order(const TradeSignal& signal) = 0;

    // 将来可扩展：
    // virtual void cancel_order(const std::string& order_id) = 0;
    // virtual Position get_position(const std::string& symbol) = 0;
    // virtual double get_available_cash() = 0;
};


/**
 * @brief 回测上下文
 *
 * 仅记录日志，不真实下单。
 */
class BacktestContext : public StrategyContext {
public:
    void place_order(const TradeSignal& signal) override {
        // 回测模式：仅记录到业务日志
        LOG_BIZ(BIZ_ORDR,
                "[BACKTEST] {} | {} | {} @ {} | qty={} | trigger_time={}",
                signal.strategy_name,
                signal.symbol,
                signal.side_str(),
                symbol_utils::int_to_price(signal.price),
                signal.quantity,
                signal.trigger_time);
    }
};


/**
 * @brief 实盘上下文
 *
 * 调用 ZmqClient 发送真实下单请求。
 */
class LiveContext : public StrategyContext {
public:
    explicit LiveContext(ZmqClient* zmq_client)
        : zmq_client_(zmq_client) {}

    void place_order(const TradeSignal& signal) override;

private:
    ZmqClient* zmq_client_ = nullptr;
};

#endif // STRATEGY_CONTEXT_H
