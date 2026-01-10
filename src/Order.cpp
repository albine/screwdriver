#include "Order.h"

#include <utility>

// 默认构造
Order::Order()
    : order_seq(0)
    , stock_code("")
    , order_time(0)
    , order_type("")
    , channel_code(0)
    , order_price(0)
    , order_volume(0)
    , remaining_volume(0)
    , order_code("")
    , backup_seq1(0)
{}

// 带参构造
Order::Order(
    uint64_t seq,
    std::string code,
    uint32_t time,
    std::string code_val,
    uint32_t channel,
    uint32_t price,
    uint32_t volume,
    std::string side,
    uint64_t backup)
    : order_seq(seq)
    , stock_code(code)
    , order_time(time)
    , order_type(std::move(code_val))
    , channel_code(channel)
    , order_price(price)
    , order_volume(volume)
    , remaining_volume(volume)
    , order_code(std::move(side))
    , backup_seq1(backup)
{}

// 成交处理
void Order::on_trade(uint32_t trade_volume) {
    if (trade_volume > remaining_volume) {
        trade_volume = remaining_volume;  // 防御性编程
    }
    remaining_volume -= trade_volume;
}

// 撤单处理
void Order::on_cancel(uint32_t cancel_volume) {
    if (cancel_volume > remaining_volume) {
        cancel_volume = remaining_volume;  // 防御性编程
    }
    remaining_volume -= cancel_volume;
}

// 获取剩余委托量
uint32_t Order::get_remaining_volume() const {
    return remaining_volume;
}

// 判断是否完全成交/撤销
bool Order::is_filled() const {
    return remaining_volume == 0;
}

// 判断是否买单
bool Order::is_buy() const {
    return order_type == OrderType::LIMIT_BUY ||
           order_type == OrderType::MARKET_BUY ||
           order_type == OrderType::BEST_BUY ||
           order_type == OrderType::SH_LIMIT_BUY;
}

// 判断是否卖单
bool Order::is_sell() const {
    return order_type == OrderType::LIMIT_SELL ||
           order_type == OrderType::MARKET_SELL ||
           order_type == OrderType::BEST_SELL ||
           order_type == OrderType::SH_LIMIT_SELL;
}

// 判断是否撤单
bool Order::is_cancel() const {
    return order_type == OrderType::CANCEL_BUY ||
           order_type == OrderType::CANCEL_SELL;
}


// 获取价格（转换为元）
double Order::get_price_yuan() const {
    return order_price / 100.0;
}

// 格式化输出
std::string Order::to_string() const {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
            "Order[seq=%lu, code=%s, time=%u, code=%s, side=%s, "
            "price=%u(%.2f), vol=%u, remain=%u]",
            order_seq, stock_code.c_str(), order_time, order_type.c_str(), order_code.c_str(),
            order_price, get_price_yuan(), order_volume, remaining_volume);
    return std::string(buffer);
}

