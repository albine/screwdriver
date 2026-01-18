#ifndef PRICE_UTIL_H
#define PRICE_UTIL_H

#include <string>
#include <cstdint>
#include <cstdio>

// ==========================================
// 价格辅助工具类
// ==========================================
namespace price_util {

// 格式化价格显示（带人民币单位）
// 例如: 125000 -> "125000(12.50元)"
inline std::string format_price_display(uint32_t price) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u(%.2f元)", price, price / 10000.0);
    return std::string(buf);
}

// 价格转换为元（用于数值计算显示）
// 例如: 125000 -> 12.50
inline double price_to_yuan(uint32_t price) {
    return price / 10000.0;
}

} // namespace price_util

#endif // PRICE_UTIL_H
