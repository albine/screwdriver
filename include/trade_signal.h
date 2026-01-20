#ifndef TRADE_SIGNAL_H
#define TRADE_SIGNAL_H

#include <string>
#include <cstdint>
#include "strategy_ids.h"

/**
 * @brief 交易信号结构体
 *
 * 用于策略触发买卖信号时，封装下单信息。
 * 策略调用 place_order() 时传入此结构体。
 */
struct TradeSignal {
    enum class Side {
        BUY,    // 买入
        SELL    // 卖出
    };

    std::string symbol;         // 股票代码（如 "600000.SH"）
    Side side;                  // 买卖方向
    uint32_t price;             // 价格（内部格式，乘以 10000）
    uint64_t quantity;          // 数量（股）
    int32_t trigger_time;       // 触发时间（MDTime 格式：HHMMSSMMM）
    std::string strategy_name;  // 策略实例名称（如 "600550.SH_Breakout"）
    uint8_t strategy_type_id = 0;  // 策略类型 ID（用于获取类型名）

    // 辅助方法：获取方向字符串
    const char* side_str() const {
        return (side == Side::BUY) ? "BUY" : "SELL";
    }

    // 获取策略类型名称（如 "BreakoutPriceVolumeStrategy"）
    const char* strategy_type_name() const {
        return StrategyIds::id_to_name(strategy_type_id);
    }
};

#endif // TRADE_SIGNAL_H
