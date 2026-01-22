#ifndef STRATEGY_BASE_H
#define STRATEGY_BASE_H

#include <string>
#include "market_data_structs_aligned.h"
#include "../src/FastOrderBook.h"

// 前向声明
struct ControlMessage;
struct TradeSignal;
class StrategyContext;

// ==========================================
// 策略基类
// ==========================================
class Strategy {
public:
    virtual ~Strategy() = default;

    // 生命周期回调
    virtual void on_start() {}
    virtual void on_stop() {}

    // 行情数据回调
    virtual void on_tick(const MDStockStruct& stock) {}

    // 委托数据回调
    virtual void on_order(const MDOrderStruct& order, const FastOrderBook& book) {}

    // 成交数据回调
    virtual void on_transaction(const MDTransactionStruct& transaction, const FastOrderBook& book) {}

    // OrderBook 快照回调
    virtual void on_orderbook_snapshot(const MDOrderbookStruct& snapshot) {}

    // 控制消息回调（默认实现处理 ENABLE/DISABLE）
    // 实现在 src/strategy_base.cpp
    virtual void on_control_message(const ControlMessage& msg);

    // 唯一标识符，用于移除策略
    uint64_t id = 0;
    std::string name;
    std::string symbol;  // 股票代码（如 "600000.SH"）

    // 策略类型 ID（8-bit，由工厂设置，用于快速匹配控制消息）
    uint8_t strategy_type_id = 0;

    // 启用/禁用状态（无需 atomic，同一策略的所有回调在同一线程顺序执行）
    bool is_enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    // 设置策略上下文（用于下单等操作）
    void set_context(StrategyContext* ctx) { ctx_ = ctx; }

protected:
    bool enabled_ = true;       // 默认启用
    StrategyContext* ctx_ = nullptr;  // 策略上下文

    // 下单方法（策略调用此方法发出交易信号）
    void place_order(const TradeSignal& signal);
};

#endif // STRATEGY_BASE_H
