#include "OrderBook.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

// ============ PriceLevelMD 实现 ============

void PriceLevelMD::add_order(OrderNode* node) {
    orders.push(node);
    order_map[node->data.applseqnum] = node;
    total_volume += node->remaining_qty;
    order_count++;
}

bool PriceLevelMD::remove_order(int64_t applseqnum) {
    auto it = order_map.find(applseqnum);
    if (it == order_map.end()) return false;
    order_count--;
    order_map.erase(it);
    return true;
}

void PriceLevelMD::update_volume(int64_t applseqnum, int64_t volume_change) {
    total_volume += volume_change;
    if (total_volume < 0) total_volume = 0;
}

// 获取队首订单（用于撮合）
OrderNode* PriceLevelMD::get_front_order() {
    // 清理已删除的订单
    while (!orders.empty()) {
        OrderNode* order = orders.front();
        if (order_map.find(order->data.applseqnum) != order_map.end()) {
            return order;  // 找到有效订单
        }
        orders.pop();  // 移除无效订单
    }
    return nullptr;
}

// ============ OrderBook 实现 ============

OrderBook::OrderBook(const std::string& code)
    : stock_code_(code)
{}

// 从市价单队列中删除订单
bool OrderBook::remove_from_market_orders(int64_t applseqnum) {
    for (auto it = market_orders_.begin(); it != market_orders_.end(); ++it) {
        if (it->data.applseqnum == applseqnum) {
            market_orders_.erase(it);
            return true;
        }
    }
    return false;
}

// ============ 对外接口 ============

bool OrderBook::on_order(const MDOrderStruct& order) {
    if (is_cancel_order(order.ordertype)) {
        // 撤单类型：在 on_transaction 中处理
        return true;
    }
    // 创建 OrderNode 并存储
    all_orders_.emplace_back(order);
    return add_order(&all_orders_.back());
}

bool OrderBook::on_transaction(const MDTransactionStruct& txn) {
    // 撤单判断
    if (txn.tradesellno == 0) {
        // 买撤单
        return cancel_order(txn.tradebuyno, txn.tradeqty);
    } else if (txn.tradebuyno == 0) {
        // 卖撤单
        return cancel_order(txn.tradesellno, txn.tradeqty);
    }
    // 正常成交：同时更新买方和卖方订单
    bool bid_updated = update_order(txn.tradebuyno, txn.tradeqty);
    bool ask_updated = update_order(txn.tradesellno, txn.tradeqty);
    return bid_updated && ask_updated;
}

// ============ 添加订单 ============
bool OrderBook::add_order(OrderNode* order) {
    if (!order) return false;

    // 添加到索引
    order_index_[order->data.applseqnum] = order;

    int32_t ordertype = order->data.ordertype;

    // 根据委托类型处理
    if (is_market_order(ordertype)) {
        // 市价单：加入市价单队列
        market_orders_.push_back(*order);
        // 更新索引指向新位置
        order_index_[order->data.applseqnum] = &market_orders_.back();
    }
    else if (is_limit_order(ordertype)) {
        // 限价单：加入价格档位
        if (order->is_buy()) {
            auto& level = bid_levels_[order->data.orderprice];
            level.price = order->data.orderprice;
            level.add_order(order);
        } else {
            auto& level = ask_levels_[order->data.orderprice];
            level.price = order->data.orderprice;
            level.add_order(order);
        }
    }
    else if (is_best_order(ordertype)) {
        // 本方最优
        if (order->is_buy()) {
            // 本方最优买：加入买方最优档位
            if (bid_levels_.empty()) {
                // 如果没有买单，暂时当作市价单处理
                market_orders_.push_back(*order);
                order_index_[order->data.applseqnum] = &market_orders_.back();
            } else {
                // 加入最优买价档位
                auto best_bid_price = bid_levels_.begin()->first;
                // 更新订单价格为实际加入的档位价格
                order->data.orderprice = best_bid_price;
                auto& level = bid_levels_[best_bid_price];
                level.add_order(order);
            }
        } else {
            // 本方最优卖：加入卖方最优档位
            if (ask_levels_.empty()) {
                // 如果没有卖单，暂时当作市价单处理
                market_orders_.push_back(*order);
                order_index_[order->data.applseqnum] = &market_orders_.back();
            } else {
                // 加入最优卖价档位
                auto best_ask_price = ask_levels_.begin()->first;
                // 更新订单价格为实际加入的档位价格
                order->data.orderprice = best_ask_price;
                auto& level = ask_levels_[best_ask_price];
                level.add_order(order);
            }
        }
    }
    else {
        // 撤单类型不应该在这里处理
        return false;
    }

    return true;
}

// ============ 修改订单（成交后） ============
bool OrderBook::update_order(int64_t applseqnum, int64_t trade_volume) {
    auto it = order_index_.find(applseqnum);
    if (it == order_index_.end()) {
        return false;
    }

    OrderNode* order = it->second;
    int64_t old_remaining = order->get_remaining_volume();

    // 更新订单
    order->on_trade(trade_volume);

    int32_t ordertype = order->data.ordertype;

    // 根据订单类型处理
    if (is_market_order(ordertype)) {
        // 市价单：如果全部成交，从市价单队列移除
        if (order->is_filled()) {
            remove_from_market_orders(applseqnum);
            order_index_.erase(it);
        }
    } else if (is_limit_order(ordertype) || is_best_order(ordertype)) {
        // 限价单/本方最优：更新档位总量
        if (order->is_buy()) {
            auto level_it = bid_levels_.find(order->data.orderprice);

            if (level_it != bid_levels_.end()) {
                int64_t volume_change =
                    order->get_remaining_volume() - old_remaining;
                level_it->second.update_volume(applseqnum, volume_change);

                // 如果全部成交，从档位移除订单
                if (order->is_filled()) {
                    level_it->second.remove_order(applseqnum);

                    // 如果档位为空，删除档位
                    if (level_it->second.is_empty()) {
                        bid_levels_.erase(level_it);
                    }

                    // 从索引移除
                    order_index_.erase(it);
                }
            }
        } else {
            auto level_it = ask_levels_.find(order->data.orderprice);

            if (level_it != ask_levels_.end()) {
                int64_t volume_change =
                    order->get_remaining_volume() - old_remaining;
                level_it->second.update_volume(applseqnum, volume_change);

                // 如果全部成交，从档位移除订单
                if (order->is_filled()) {
                    level_it->second.remove_order(applseqnum);

                    // 如果档位为空，删除档位
                    if (level_it->second.is_empty()) {
                        ask_levels_.erase(level_it);
                    }

                    // 从索引移除
                    order_index_.erase(it);
                }
            }
        }
    }

    return true;
}

// ============ 撤单处理 ============
bool OrderBook::cancel_order(int64_t applseqnum, int64_t cancel_volume) {
    auto it = order_index_.find(applseqnum);
    if (it == order_index_.end()) {
        return false;
    }

    OrderNode* order = it->second;
    int64_t old_remaining = order->get_remaining_volume();

    // 更新订单
    order->on_cancel(cancel_volume);

    int32_t ordertype = order->data.ordertype;

    // 根据订单类型处理
    if (is_market_order(ordertype)) {
        // 市价单：如果全部撤销，从市价单队列移除
        if (order->is_filled()) {
            remove_from_market_orders(applseqnum);
            order_index_.erase(it);
        }
    } else if (is_limit_order(ordertype) || is_best_order(ordertype)) {
        // 限价单/本方最优：更新档位
        if (order->is_buy()) {
            auto level_it = bid_levels_.find(order->data.orderprice);

            if (level_it != bid_levels_.end()) {
                int64_t volume_change =
                    order->get_remaining_volume() - old_remaining;
                level_it->second.update_volume(applseqnum, volume_change);

                // 如果全部撤销，从档位移除
                if (order->is_filled()) {
                    level_it->second.remove_order(applseqnum);

                    // 如果档位为空，删除档位
                    if (level_it->second.is_empty()) {
                        bid_levels_.erase(level_it);
                    }

                    // 从索引移除
                    order_index_.erase(it);
                }
            }
        } else {
            auto level_it = ask_levels_.find(order->data.orderprice);

            if (level_it != ask_levels_.end()) {
                int64_t volume_change =
                    order->get_remaining_volume() - old_remaining;
                level_it->second.update_volume(applseqnum, volume_change);

                // 如果全部撤销，从档位移除
                if (order->is_filled()) {
                    level_it->second.remove_order(applseqnum);

                    // 如果档位为空，删除档位
                    if (level_it->second.is_empty()) {
                        ask_levels_.erase(level_it);
                    }

                    // 从索引移除
                    order_index_.erase(it);
                }
            }
        }
    }

    return true;
}

// ============ 查询接口 ============

std::optional<int64_t> OrderBook::get_best_bid() const {
    if (bid_levels_.empty()) return std::nullopt;
    return bid_levels_.begin()->first;
}

std::optional<int64_t> OrderBook::get_best_ask() const {
    if (ask_levels_.empty()) return std::nullopt;
    return ask_levels_.begin()->first;
}

int64_t OrderBook::get_volume_at_price(int64_t price, bool is_bid) const {
    if (is_bid) {
        auto it = bid_levels_.find(price);
        return it != bid_levels_.end() ? it->second.total_volume : 0;
    } else {
        auto it = ask_levels_.find(price);
        return it != ask_levels_.end() ? it->second.total_volume : 0;
    }
}

// ============ 打板策略核心函数 ============

int64_t OrderBook::sum_asks_in_range(int64_t low_price, int64_t high_price) const {
    int64_t total = 0;

    auto it_low = ask_levels_.lower_bound(low_price);
    auto it_high = ask_levels_.upper_bound(high_price);

    for (auto it = it_low; it != it_high; ++it) {
        total += it->second.total_volume;
    }

    return total;
}

int64_t OrderBook::get_bid_volume_at_limit_up(int64_t limit_up_price) const {
    return get_volume_at_price(limit_up_price, true);
}

// ============ 深度数据 ============

std::vector<std::pair<int64_t, int64_t>> OrderBook::get_bid_depth(size_t levels) const {
    std::vector<std::pair<int64_t, int64_t>> result;
    result.reserve(levels);

    size_t count = 0;
    for (const auto& [price, level] : bid_levels_) {
        if (count++ >= levels) break;
        result.emplace_back(price, level.total_volume);
    }

    return result;
}

std::vector<std::pair<int64_t, int64_t>> OrderBook::get_ask_depth(size_t levels) const {
    std::vector<std::pair<int64_t, int64_t>> result;
    result.reserve(levels);

    size_t count = 0;
    for (const auto& [price, level] : ask_levels_) {
        if (count++ >= levels) break;
        result.emplace_back(price, level.total_volume);
    }

    return result;
}

// ============ 调试输出 ============

void OrderBook::print_book(size_t depth, bool show_all) const {
    auto bids = get_bid_depth(show_all ? 1000 : depth);
    auto asks = get_ask_depth(show_all ? 1000 : depth);

    // 假设 datamultiplepowerof10 = 4
    double price_divisor = 10000.0;

    printf("\n========== %s ==========\n", stock_code_.c_str());

    // 卖盘：从高到低显示（最接近成交价的在最下面）
    if (!asks.empty()) {
        size_t ask_count = asks.size();
        for (size_t i = ask_count; i > 0; --i) {
            size_t idx = i - 1;
            int64_t price = asks[idx].first;
            int64_t volume = asks[idx].second;

            printf("ASK%-3zu  %8.2f  %ld\n",
                   ask_count - idx,
                   price / price_divisor,
                   volume);
        }
    } else {
        printf("(No ask orders)\n");
    }

    printf("---------------------------\n");

    // 买盘：从高到低显示（最接近成交价的在最上面）
    if (!bids.empty()) {
        for (size_t i = 0; i < bids.size(); ++i) {
            int64_t price = bids[i].first;
            int64_t volume = bids[i].second;

            printf("BID%-3zu  %8.2f  %ld\n",
                   i + 1,
                   price / price_divisor,
                   volume);
        }
    } else {
        printf("(No bid orders)\n");
    }

    printf("===========================\n");
    printf("Stock: %s | Total orders: %zu | Market orders: %zu\n",
           stock_code_.c_str(), total_orders(), market_orders_count());
    printf("Bid levels: %zu | Ask levels: %zu\n",
           bid_level_count(), ask_level_count());
    printf("\n");
}

void OrderBook::debug_print_ask_levels(size_t max_orders_per_level) const {
    printf("\n========== DEBUG: Ask Levels Detail ==========\n");
    printf("Total ask levels: %zu\n\n", ask_levels_.size());

    double price_divisor = 10000.0;

    size_t level_idx = 0;
    for (const auto& [price, level] : ask_levels_) {
        level_idx++;
        printf("Level %zu: Price=%.2f (%ld), TotalVolume=%ld, OrderCount=%u\n",
               level_idx,
               price / price_divisor,
               price,
               level.total_volume,
               level.order_count);

        // 打印前 max_orders_per_level 个订单
        size_t order_idx = 0;
        for (const auto& [applseqnum, order_ptr] : level.order_map) {
            if (order_idx >= max_orders_per_level) {
                printf("  ... (%u more orders)\n",
                       level.order_count - (uint32_t)max_orders_per_level);
                break;
            }

            printf("  Order %zu: seq=%ld, type=%d, bsflag=%d, price=%ld, qty=%ld, remaining=%ld\n",
                   ++order_idx,
                   applseqnum,
                   order_ptr->data.ordertype,
                   order_ptr->data.orderbsflag,
                   order_ptr->data.orderprice,
                   order_ptr->data.orderqty,
                   order_ptr->remaining_qty);
        }

        printf("\n");

        // 只显示前10个价格档位
        if (level_idx >= 10) {
            printf("... (%zu more levels)\n", ask_levels_.size() - 10);
            break;
        }
    }

    printf("==============================================\n\n");
}
