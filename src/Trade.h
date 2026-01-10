#ifndef TRADE_H
#define TRADE_H

#include <cstdint>
#include <string>
#include <cstdio>

namespace TradeFlag {
    constexpr const char* ACTIVE_BUY = "0";      // 主动买入
    constexpr const char* ACTIVE_SELL = "1";    // 主动卖出
    constexpr const char* CANCEL_BUY = "-1";     // 买单撤单
    constexpr const char* CANCEL_SELL = "-11";    // 卖单撤单
}

class Trade {
public:
    uint32_t channel_code{};      // 频道代码
    uint64_t trade_seq{};         // 成交序号
    std::string stock_code{};        // 股票代码
    uint64_t bid_order_seq{};     // 买方委托序号
    uint64_t ask_order_seq{};     // 卖方委托序号
    uint32_t trade_time{};        // 成交时间
    uint32_t trade_price{};       // 成交价格 (单位：分)
    uint32_t trade_volume{};      // 成交数量 (股)
    uint32_t trade_type{};        // 成交类型
    std::string trade_flag;     // 买卖标志
    uint64_t backup_seq1{};       // 备用序号1
    
public:
    // 默认构造
    Trade();
    
    // 带参构造
    Trade(uint32_t channel, uint64_t seq, std::string code,
          uint64_t bid_seq, uint64_t ask_seq, uint32_t time,
          uint32_t price, uint32_t volume, uint32_t type,
          const std::string& flag, uint64_t backup = 0);
    
    // 判断是否撤单
    bool is_cancel() const;
    
    // 判断是否买单撤单
    bool is_cancel_buy() const;
    
    // 判断是否卖单撤单
    bool is_cancel_sell() const;
    
    // 判断是否主动买入
    bool is_active_buy() const;
    
    // 判断是否主动卖出
    bool is_active_sell() const;
    
    // 获取价格（元）
    double get_price_yuan() const;
    
    // 格式化输出
    std::string to_string() const;
};

#endif // TRADE_H
