#ifndef ORDER_H
#define ORDER_H

#include <cstdint>
#include <string>
#include <cstdio>

// 委托类型常量定义（string 类型）
namespace OrderType {
    constexpr const char* MARKET_BUY = "1";      // 市价委买
    constexpr const char* LIMIT_BUY = "2";       // 限价委买
    constexpr const char* BEST_BUY = "3";        // 本方最优委买
    constexpr const char* MARKET_SELL = "11";    // 市价委卖
    constexpr const char* LIMIT_SELL = "12";     // 限价委卖
    constexpr const char* BEST_SELL = "13";      // 本方最优委卖
    constexpr const char* CANCEL_BUY = "-1";     // 撤买
    constexpr const char* CANCEL_SELL = "-11";   // 撤卖
    constexpr const char* SH_LIMIT_BUY = "0";   // 上海委买
    constexpr const char* SH_LIMIT_SELL = "10";   // 上海委卖
}

class Order {
public:
    uint64_t order_seq;         // 委托序号
    std::string stock_code;        // 股票代码
    uint32_t order_time;        // 委托时间
    std::string order_type;     // 委托代码
    uint32_t channel_code;      // 频道代码
    uint32_t order_price;       // 委托价格 (单位：分，如1050表示10.50元)
    uint32_t order_volume;      // 委托数量 (股)
    uint32_t remaining_volume;  // 剩余委托量 (股)
    std::string order_code;     // 委托类型（方向）
    uint64_t backup_seq1;       // 备用序号1
    
public:
    // 默认构造
    Order();

    // 带参构造
    Order(uint64_t seq, std::string code, uint32_t time, std::string code_val,
          uint32_t channel, uint32_t price, uint32_t volume,
          std::string side, uint64_t backup = 0);
    
    // ============ 核心方法 ============
    
    // 成交处理
    void on_trade(uint32_t trade_volume);
    
    // 撤单处理
    void on_cancel(uint32_t cancel_volume);
    
    // 获取剩余委托量
    uint32_t get_remaining_volume() const;
    
    // 判断是否完全成交/撤销
    bool is_filled() const;
    
    // 判断是否买单
    bool is_buy() const;
    
    // 判断是否卖单
    bool is_sell() const;

    // 判断是否撤单
    bool is_cancel() const;
    
    // 获取价格（转换为元）
    double get_price_yuan() const;
    
    // 格式化输出
    std::string to_string() const;
};

#endif // ORDER_H
