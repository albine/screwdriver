#include "Trade.h"

// 默认构造
Trade::Trade()
    : channel_code(0)
    , trade_seq(0)
    , stock_code("")
    , bid_order_seq(0)
    , ask_order_seq(0)
    , trade_time(0)
    , trade_price(0)
    , trade_volume(0)
    , trade_type(0)
    , backup_seq1(0)
{}

Trade::Trade(uint32_t channel, uint64_t seq, std::string code, uint64_t bid_seq, uint64_t ask_seq, uint32_t time,
    uint32_t price, uint32_t volume, uint32_t type, const std::string &flag, uint64_t backup)
: channel_code(channel)
, trade_seq(seq)
, stock_code(code)
, bid_order_seq(bid_seq)
, ask_order_seq(ask_seq)
, trade_time(time)
, trade_price(price)
, trade_volume(volume)
, trade_type(type)
, trade_flag(flag)
, backup_seq1(backup)
{}

// 判断是否撤单
bool Trade::is_cancel() const {
    return trade_flag == TradeFlag::CANCEL_BUY || trade_flag == TradeFlag::CANCEL_SELL;
}

// 判断是否买单撤单
bool Trade::is_cancel_buy() const {
    return trade_flag == TradeFlag::CANCEL_BUY;
}

// 判断是否卖单撤单
bool Trade::is_cancel_sell() const {
    return trade_flag == TradeFlag::CANCEL_SELL;
}

// 判断是否主动买入
bool Trade::is_active_buy() const {
    return trade_flag == TradeFlag::ACTIVE_BUY;
}

// 判断是否主动卖出
bool Trade::is_active_sell() const {
    return trade_flag == TradeFlag::ACTIVE_SELL;
}

// 获取价格（元）
double Trade::get_price_yuan() const {
    return trade_price / 100.0;
}

// 格式化输出
std::string Trade::to_string() const {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
            "Trade[seq=%lu, code=%s, time=%u, bid_seq=%lu, ask_seq=%lu, "
            "price=%u(%.2f), vol=%u, flag=%s]",
            trade_seq, stock_code.c_str(), trade_time, bid_order_seq, ask_order_seq,
            trade_price, get_price_yuan(), trade_volume, trade_flag.c_str());
    return std::string(buffer);
}
