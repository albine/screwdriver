#ifndef TEST_ORDERBOOK_STRATEGY_H
#define TEST_ORDERBOOK_STRATEGY_H

#include "strategy_base.h"
#include "market_data_structs.h"
#include "logger.h"
#include "utils/time_util.h"
#include "utils/symbol_utils.h"
#include <string>
#include <set>
#include <sstream>

#define LOG_MODULE MOD_STRATEGY

/**
 * TestOrderbookStrategy
 *
 * 当 order 或 transaction 的 applseqnum 等于指定值时，打印十档订单簿
 * 支持单个或多个目标 applseqnum（逗号分隔）
 */
class TestOrderbookStrategy : public Strategy {
private:
    std::set<int64_t> target_seqs_;  // 目标序号集合

public:
    explicit TestOrderbookStrategy(const std::string& strategy_name,
                                   const std::string& sym = "",
                                   int64_t target_seq = -1)
    {
        this->name = strategy_name;
        this->symbol = sym;
        if (target_seq > 0) {
            target_seqs_.insert(target_seq);
        }
    }

    // 支持多个目标序号（逗号分隔的字符串）
    explicit TestOrderbookStrategy(const std::string& strategy_name,
                                   const std::string& sym,
                                   const std::string& target_seqs_str)
    {
        this->name = strategy_name;
        this->symbol = sym;
        parse_target_seqs(target_seqs_str);
    }

    virtual ~TestOrderbookStrategy() = default;

    void parse_target_seqs(const std::string& str) {
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                int64_t seq = std::stoll(token);
                if (seq > 0) {
                    target_seqs_.insert(seq);
                }
            } catch (...) {
                // 忽略无效的数字
            }
        }
    }

    void on_start() override {
        LOG_M_INFO("TestOrderbookStrategy started for {}, {} target applseqnums",
                   symbol, target_seqs_.size());
    }

    void on_stop() override {
        LOG_M_INFO("TestOrderbookStrategy stopped for {}", symbol);
    }

    void on_tick(const MDStockStruct& stock) override {
        // 不需要处理 tick
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        if (target_seqs_.count(order.applseqnum) > 0) {
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
        if (target_seqs_.count(txn.applseqnum) > 0) {
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
