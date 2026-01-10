#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H

#include "../include/market_data_structs.h"
#include <map>
#include <unordered_map>
#include <list>
#include <queue>
#include <optional>
#include <vector>
#include <utility>
#include <string>

// 订单节点（包装 MDOrderStruct + 剩余量）
struct OrderNode {
    MDOrderStruct data;
    int64_t remaining_qty;

    OrderNode(const MDOrderStruct& order)
        : data(order), remaining_qty(order.orderqty) {}

    bool is_filled() const { return remaining_qty <= 0; }
    bool is_buy() const { return data.orderbsflag == 1; }
    bool is_sell() const { return data.orderbsflag == 2; }
    int64_t get_remaining_volume() const { return remaining_qty; }
    void on_trade(int64_t vol) { remaining_qty -= vol; }
    void on_cancel(int64_t vol) { remaining_qty -= vol; }
};

// 价格档位
struct PriceLevelMD {
    int64_t price = 0;
    int64_t total_volume = 0;
    uint32_t order_count = 0;
    std::queue<OrderNode*> orders;                      // 订单队列 (FIFO)
    std::unordered_map<int64_t, OrderNode*> order_map;  // applseqnum -> OrderNode* 快速查找

    void add_order(OrderNode* node);
    bool remove_order(int64_t applseqnum);
    void update_volume(int64_t applseqnum, int64_t volume_change);
    bool is_empty() const { return order_count == 0; }
    OrderNode* get_front_order();  // 获取队首订单（用于撮合）
};

class OrderBook {
private:
    // 价格索引: 买单降序（价格从高到低），卖单升序（价格从低到高）
    std::map<int64_t, PriceLevelMD, std::greater<int64_t>> bid_levels_;  // 买单
    std::map<int64_t, PriceLevelMD> ask_levels_;                          // 卖单

    // 市价单队列
    std::list<OrderNode> market_orders_;

    // 订单序号快速查找
    std::unordered_map<int64_t, OrderNode*> order_index_;  // applseqnum -> OrderNode*

    // 订单存储（保证指针稳定性）
    std::list<OrderNode> all_orders_;

    // 股票代码
    std::string stock_code_;

    // 辅助函数：判断订单类型
    bool is_market_order(int32_t ordertype) const { return ordertype == 1; }
    bool is_limit_order(int32_t ordertype) const { return ordertype == 2; }
    bool is_best_order(int32_t ordertype) const { return ordertype == 3; }
    bool is_cancel_order(int32_t ordertype) const { return ordertype == 4; }

    // 从市价单队列中删除订单
    bool remove_from_market_orders(int64_t applseqnum);

public:
    explicit OrderBook(const std::string& code = "");

    // ============ 核心接口（内部使用）============
    bool add_order(OrderNode* order);
    bool update_order(int64_t applseqnum, int64_t trade_volume);
    bool cancel_order(int64_t applseqnum, int64_t cancel_volume);

    // ============ 对外接口 ============
    bool on_order(const MDOrderStruct& order);
    bool on_transaction(const MDTransactionStruct& txn);

    // ============ 查询接口 ============
    std::optional<int64_t> get_best_bid() const;
    std::optional<int64_t> get_best_ask() const;
    int64_t get_volume_at_price(int64_t price, bool is_bid) const;

    // ============ 打板策略核心函数 ============
    int64_t sum_asks_in_range(int64_t low_price, int64_t high_price) const;
    int64_t get_bid_volume_at_limit_up(int64_t limit_up_price) const;

    // ============ 深度数据 ============
    std::vector<std::pair<int64_t, int64_t>> get_bid_depth(size_t levels = 10) const;
    std::vector<std::pair<int64_t, int64_t>> get_ask_depth(size_t levels = 10) const;

    // ============ 统计信息 ============
    size_t total_orders() const { return order_index_.size(); }
    size_t market_orders_count() const { return market_orders_.size(); }
    size_t bid_level_count() const { return bid_levels_.size(); }
    size_t ask_level_count() const { return ask_levels_.size(); }

    // ============ 调试输出 ============
    void print_book(size_t depth = 10, bool show_all = false) const;
    void debug_print_ask_levels(size_t max_orders_per_level = 10) const;
};

#endif // ORDER_BOOK_H
