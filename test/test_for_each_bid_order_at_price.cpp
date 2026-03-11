#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "FastOrderBook.h"
#include "ObjectPool.h"
#include "market_data_structs_aligned.h"

namespace {

MDOrderStruct make_order(uint64_t order_id,
                         uint32_t price,
                         uint32_t qty,
                         int32_t side,
                         int32_t type,
                         int32_t source,
                         int64_t orderindex,
                         int64_t orderno,
                         int64_t applseqnum) {
    MDOrderStruct order{};
    std::strncpy(order.htscsecurityid, source == 101 ? "600000.SH" : "000001.SZ",
                 sizeof(order.htscsecurityid) - 1);
    order.securityidsource = source;
    order.securitytype = 1;
    order.orderindex = orderindex;
    order.orderno = orderno;
    order.orderprice = price;
    order.orderqty = qty;
    order.ordertype = type;
    order.orderbsflag = side;
    order.applseqnum = applseqnum;
    order.mddate = 20260311;
    order.mdtime = 133000000;
    order.local_recv_timestamp = applseqnum;
    (void)order_id;
    return order;
}

MDTransactionStruct make_trade(int32_t source,
                               uint64_t buy_order_id,
                               uint64_t sell_order_id,
                               uint32_t price,
                               uint32_t qty,
                               int32_t trade_bs_flag,
                               int64_t applseqnum) {
    MDTransactionStruct txn{};
    std::strncpy(txn.htscsecurityid, source == 101 ? "600000.SH" : "000001.SZ",
                 sizeof(txn.htscsecurityid) - 1);
    txn.securityidsource = source;
    txn.securitytype = 1;
    txn.tradebuyno = buy_order_id;
    txn.tradesellno = sell_order_id;
    txn.tradeprice = price;
    txn.tradeqty = qty;
    txn.tradetype = 0;
    txn.tradebsflag = trade_bs_flag;
    txn.applseqnum = applseqnum;
    txn.mddate = 20260311;
    txn.mdtime = 133001000;
    txn.local_recv_timestamp = applseqnum;
    return txn;
}

std::vector<std::pair<uint64_t, uint32_t>> collect_bid_orders(const FastOrderBook& book, uint32_t price) {
    std::vector<std::pair<uint64_t, uint32_t>> result;
    book.for_each_bid_order_at_price(price, [&](uint64_t seq, uint32_t volume) {
        result.emplace_back(seq, volume);
    });
    return result;
}

bool expect_orders(const std::string& name,
                   const std::vector<std::pair<uint64_t, uint32_t>>& actual,
                   const std::vector<std::pair<uint64_t, uint32_t>>& expected) {
    if (actual == expected) {
        return true;
    }

    std::cerr << "[FAIL] " << name << "\n";
    std::cerr << "  expected:";
    for (const auto& [seq, vol] : expected) {
        std::cerr << " (" << seq << "," << vol << ")";
    }
    std::cerr << "\n";
    std::cerr << "  actual  :";
    for (const auto& [seq, vol] : actual) {
        std::cerr << " (" << seq << "," << vol << ")";
    }
    std::cerr << "\n";
    return false;
}

bool expect_u64(const std::string& name, uint64_t actual, uint64_t expected) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "[FAIL] " << name << " expected=" << expected << " actual=" << actual << "\n";
    return false;
}

}  // namespace

int main() {
    ObjectPool<OrderNode> pool(32);
    FastOrderBook book(1, pool, 90000, 110000);

    bool ok = true;

    const uint32_t target_price = 100000;
    const uint32_t other_price = 100100;

    MDOrderStruct sz_bid_1 = make_order(1001, target_price, 500, 1, 2, 102, 1001, 0, 1);
    MDOrderStruct sz_bid_2 = make_order(1002, target_price, 300, 1, 2, 102, 1002, 0, 2);
    MDOrderStruct sz_bid_other = make_order(1003, other_price, 700, 1, 2, 102, 1003, 0, 3);
    MDOrderStruct sh_bid = make_order(9001, target_price, 800, 1, 2, 101, 0, 9001, 4);
    MDOrderStruct sz_ask = make_order(2001, target_price, 400, 2, 2, 102, 2001, 0, 5);

    ok &= book.on_order(sz_bid_1);
    ok &= book.on_order(sz_bid_2);
    ok &= book.on_order(sz_bid_other);
    ok &= book.on_order(sh_bid);
    ok &= book.on_order(sz_ask);

    ok &= expect_orders("initial target price bids",
                        collect_bid_orders(book, target_price),
                        {{1001, 500}, {1002, 300}, {9001, 800}});
    ok &= expect_orders("other price bids",
                        collect_bid_orders(book, other_price),
                        {{1003, 700}});
    ok &= expect_u64("initial bid volume at target price",
                     book.get_bid_volume_at_price(target_price), 1600);

    MDTransactionStruct partial_trade = make_trade(102, 1001, 2001, target_price, 200, 2, 6);
    ok &= book.on_transaction(partial_trade);
    ok &= expect_orders("after partial trade",
                        collect_bid_orders(book, target_price),
                        {{1001, 300}, {1002, 300}, {9001, 800}});
    ok &= expect_u64("bid volume after partial trade",
                     book.get_bid_volume_at_price(target_price), 1400);

    MDOrderStruct sz_cancel = make_order(1002, target_price, 300, 1, 4, 102, 1002, 0, 7);
    ok &= book.on_order(sz_cancel);
    ok &= expect_orders("after sz cancel",
                        collect_bid_orders(book, target_price),
                        {{1001, 300}, {9001, 800}});
    ok &= expect_u64("bid volume after sz cancel",
                     book.get_bid_volume_at_price(target_price), 1100);

    MDOrderStruct sh_cancel = make_order(9001, target_price, 800, 1, 10, 101, 0, 9001, 8);
    ok &= book.on_order(sh_cancel);
    ok &= expect_orders("after sh cancel",
                        collect_bid_orders(book, target_price),
                        {{1001, 300}});
    ok &= expect_u64("bid volume after sh cancel",
                     book.get_bid_volume_at_price(target_price), 300);

    MDOrderStruct sz_cancel_last = make_order(1001, target_price, 300, 1, 4, 102, 1001, 0, 9);
    ok &= book.on_order(sz_cancel_last);
    ok &= expect_orders("after removing last bid",
                        collect_bid_orders(book, target_price),
                        {});
    ok &= expect_u64("bid volume after removing last bid",
                     book.get_bid_volume_at_price(target_price), 0);

    if (!ok) {
        return 1;
    }

    std::cout << "test_for_each_bid_order_at_price passed\n";
    return 0;
}
