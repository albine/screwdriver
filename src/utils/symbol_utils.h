#ifndef SYMBOL_UTILS_H
#define SYMBOL_UTILS_H

#include <string>

namespace symbol_utils {

// 自动补全股票代码后缀
// 规则：6开头 -> .SH（上海），其他 -> .SZ（深圳）
// 如果已有后缀则原样返回
inline std::string normalize_symbol(const std::string& symbol) {
    if (symbol.empty()) return symbol;

    // 已有后缀，原样返回
    if (symbol.find('.') != std::string::npos) {
        return symbol;
    }

    // 6开头是上海，其他是深圳
    if (symbol[0] == '6') {
        return symbol + ".SH";
    } else {
        return symbol + ".SZ";
    }
}

// 从完整代码中提取纯数字部分
// 例如：600759.SH -> 600759
inline std::string strip_suffix(const std::string& symbol) {
    auto pos = symbol.find('.');
    if (pos != std::string::npos) {
        return symbol.substr(0, pos);
    }
    return symbol;
}

// 判断是否是上海股票
inline bool is_shanghai(const std::string& symbol) {
    if (symbol.empty()) return false;
    return symbol[0] == '6';
}

// 判断是否是深圳股票
inline bool is_shenzhen(const std::string& symbol) {
    if (symbol.empty()) return false;
    return symbol[0] != '6';
}

// 价格转换：double 转 uint32_t（乘以 10000）
// 用于将元为单位的价格转换为内部整数格式
inline uint32_t price_to_int(double price) {
    return static_cast<uint32_t>(price * 10000);
}

// 价格转换：uint32_t 转 double（除以 10000）
// 用于将内部整数格式转换为元为单位的价格
inline double int_to_price(uint32_t price_int) {
    return price_int / 10000.0;
}

} // namespace symbol_utils

#endif // SYMBOL_UTILS_H
