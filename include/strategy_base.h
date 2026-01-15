#ifndef STRATEGY_BASE_H
#define STRATEGY_BASE_H

#include <string>
#include "market_data_structs.h"
#include "../src/FastOrderBook.h"

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

    // 唯一标识符，用于移除策略
    uint64_t id = 0;
    std::string name;
};

#endif // STRATEGY_BASE_H
