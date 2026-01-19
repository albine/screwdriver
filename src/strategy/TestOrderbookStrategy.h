#ifndef TEST_ORDERBOOK_STRATEGY_H
#define TEST_ORDERBOOK_STRATEGY_H

#include "strategy_base.h"
#include "market_data_structs.h"
#include "logger.h"
#include "utils/time_util.h"
#include "utils/symbol_utils.h"
#include <string>

#define LOG_MODULE MOD_STRATEGY

/**
 * TestOrderbookStrategy
 *
 * 当 order 或 transaction 的 applseqnum 等于指定值时，打印十档订单簿
 */
class TestOrderbookStrategy : public Strategy {
private:
    int64_t target_seq_ = -1;  // 目标序号

public:
    explicit TestOrderbookStrategy(const std::string& strategy_name,
                                   const std::string& sym = "",
                                   int64_t target_seq = -1)
        : target_seq_(target_seq) {
        this->name = strategy_name;
        this->symbol = sym;
    }

    virtual ~TestOrderbookStrategy() = default;

    void on_start() override {
        LOG_M_INFO("TestOrderbookStrategy started for {}, target_seq={}", symbol, target_seq_);
    }

    void on_stop() override {
        LOG_M_INFO("TestOrderbookStrategy stopped for {}", symbol);
    }

    void on_tick(const MDStockStruct& stock) override {
        // 不需要处理 tick
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (target_seq_ > 0 && order.applseqnum == target_seq_) {
            LOG_M_INFO(">>> Order applseqnum={} matched! time={} price={} qty={} bs={}",
                       order.applseqnum,
                       time_util::format_mdtime(order.mdtime),
                       symbol_utils::int_to_price(order.orderprice),
                       order.orderqty,
                       order.orderbsflag == 1 ? "BUY" : "SELL");
            book.print_orderbook(10, "After Order " + std::to_string(order.applseqnum));
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        if (target_seq_ > 0 && txn.applseqnum == target_seq_) {
            LOG_M_INFO(">>> Transaction applseqnum={} matched! time={} price={} qty={} bs={}",
                       txn.applseqnum,
                       time_util::format_mdtime(txn.mdtime),
                       symbol_utils::int_to_price(txn.tradeprice),
                       txn.tradeqty,
                       txn.tradebsflag == 1 ? "BUY" : "SELL");
            book.print_orderbook(10, "After Transaction " + std::to_string(txn.applseqnum));
        }
    }
};

#undef LOG_MODULE
#endif // TEST_ORDERBOOK_STRATEGY_H
