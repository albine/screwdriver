#ifndef STRATEGY_IDS_H
#define STRATEGY_IDS_H

#include <cstdint>
#include <string>
#include <unordered_map>

namespace StrategyIds {

// ==========================================
// 策略类型 ID 定义 (8-bit, 0-255)
// ==========================================
constexpr uint8_t UNKNOWN                         = 0;
constexpr uint8_t OPENING_RANGE_BREAKOUT          = 1;
constexpr uint8_t BREAKOUT_PRICE_VOLUME           = 5;

// ==========================================
// 策略名称 <-> ID 映射
// ==========================================
inline uint8_t name_to_id(const std::string& name) {
    static const std::unordered_map<std::string, uint8_t> map = {
        {"OpeningRangeBreakoutStrategy", OPENING_RANGE_BREAKOUT},
        {"BreakoutPriceVolumeStrategy", BREAKOUT_PRICE_VOLUME},
    };
    auto it = map.find(name);
    return (it != map.end()) ? it->second : UNKNOWN;
}

inline const char* id_to_name(uint8_t id) {
    switch (id) {
        case OPENING_RANGE_BREAKOUT:   return "OpeningRangeBreakoutStrategy";
        case BREAKOUT_PRICE_VOLUME:    return "BreakoutPriceVolumeStrategy";
        default:                       return "Unknown";
    }
}

// ==========================================
// 唯一 ID 构造/解析
// ==========================================

// 从 symbol (如 "600759.SH" 或 "600759") 解析 stock_code 和 exchange bit
inline bool parse_symbol(const std::string& symbol, uint32_t& stock_code, bool& is_shanghai) {
    // 提取数字部分
    size_t num_end = 0;
    while (num_end < symbol.size() && symbol[num_end] >= '0' && symbol[num_end] <= '9') {
        num_end++;
    }
    if (num_end == 0 || num_end > 6) return false;

    stock_code = static_cast<uint32_t>(std::stoul(symbol.substr(0, num_end)));

    // 判断交易所：'6'开头为上海
    is_shanghai = (symbol[0] == '6');

    return true;
}

// 构造唯一 ID
// bits 0-7:   strategy_id (0-255)
// bit 8:      exchange (1=SH, 0=SZ)
// bits 9-29:  stock_code (0-999999)
inline uint32_t make_unique_id(uint32_t stock_code, bool is_shanghai, uint8_t strategy_id) {
    return (stock_code << 9) | (is_shanghai ? 0x100 : 0) | strategy_id;
}

// 从字符串构造唯一 ID
inline uint32_t make_unique_id(const std::string& symbol, const std::string& strategy_name) {
    uint32_t stock_code;
    bool is_shanghai;
    if (!parse_symbol(symbol, stock_code, is_shanghai)) {
        return 0;  // 无效
    }
    uint8_t strategy_id = name_to_id(strategy_name);
    return make_unique_id(stock_code, is_shanghai, strategy_id);
}

// 从 symbol 和 strategy_type_id 构造唯一 ID
inline uint32_t make_unique_id(const std::string& symbol, uint8_t strategy_type_id) {
    uint32_t stock_code;
    bool is_shanghai;
    if (!parse_symbol(symbol, stock_code, is_shanghai)) {
        return 0;  // 无效
    }
    return make_unique_id(stock_code, is_shanghai, strategy_type_id);
}

// 解析唯一 ID
inline void parse_unique_id(uint32_t unique_id, uint32_t& stock_code, bool& is_shanghai, uint8_t& strategy_id) {
    strategy_id = unique_id & 0xFF;
    is_shanghai = (unique_id >> 8) & 0x1;
    stock_code = (unique_id >> 9) & 0x1FFFFF;
}

// 从唯一 ID 获取 symbol 字符串 (格式: "600759.SH")
inline std::string unique_id_to_symbol(uint32_t unique_id) {
    uint32_t stock_code;
    bool is_shanghai;
    uint8_t strategy_id;
    parse_unique_id(unique_id, stock_code, is_shanghai, strategy_id);

    char buf[16];
    snprintf(buf, sizeof(buf), "%06u.%s", stock_code, is_shanghai ? "SH" : "SZ");
    return std::string(buf);
}

// 从 symbol 解析 shard_id (用于队列路由)
inline int get_shard_id_from_symbol(const std::string& symbol, int shard_count) {
    uint32_t stock_code;
    bool is_shanghai;
    if (!parse_symbol(symbol, stock_code, is_shanghai)) {
        return 0;
    }
    // 用 stock_code + exchange bit 计算 hash
    uint32_t hash = (stock_code << 1) | (is_shanghai ? 1 : 0);
    return static_cast<int>(hash % shard_count);
}

} // namespace StrategyIds

#endif // STRATEGY_IDS_H
