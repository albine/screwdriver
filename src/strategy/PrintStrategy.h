#ifndef PRINT_STRATEGY_H
#define PRINT_STRATEGY_H

#include "strategy_base.h"
#include <iostream>
#include <string>

// ==========================================
// 简单打印策略
// ==========================================
// 每隔一定数量的订单打印一次最优报价
class PrintStrategy : public Strategy {
public:
    explicit PrintStrategy(const std::string& n) {
        name = n;
    }

    void on_tick(const MDStockStruct& stock) override {
        if (!is_enabled()) return;
        // Simple tick print
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        // 每 1000 个订单打印一次最优报价
        if (order.applseqnum % 1000 == 0) {
            auto bid = book.get_best_bid();
            auto ask = book.get_best_ask();
            std::cout << "[Strat:" << name << "] OrderSeq:" << order.applseqnum
                      << " BestBid:" << (bid ? std::to_string(*bid) : "None")
                      << " BestAsk:" << (ask ? std::to_string(*ask) : "None")
                      << std::endl;
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (!is_enabled()) return;
        // Transaction logic
    }
};

#endif // PRINT_STRATEGY_H
