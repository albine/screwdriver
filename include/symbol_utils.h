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

} // namespace symbol_utils

#endif // SYMBOL_UTILS_H
