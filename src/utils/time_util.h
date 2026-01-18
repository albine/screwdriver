#ifndef TIME_UTIL_H
#define TIME_UTIL_H

#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>

// ==========================================
// 时间辅助工具类
// ==========================================
namespace time_util {

// 午休时间常量（MDTime格式）
constexpr int32_t MORNING_END = 113000000;    // 11:30:00.000
constexpr int32_t AFTERNOON_START = 130000000; // 13:00:00.000
constexpr int64_t LUNCH_BREAK_MS = 5400000;    // 1.5小时 = 5400秒 = 5400000毫秒

// 将 MDTime 转换为毫秒数（从午夜开始）
// MDTime格式: HHMMSSMMM (9位数字)
// 例如: 093015500 = 09:30:15.500
inline int64_t mdtime_to_ms(int32_t mdtime) {
    int32_t h = mdtime / 10000000;
    int32_t m = (mdtime / 100000) % 100;
    int32_t s = (mdtime / 1000) % 100;
    int32_t ms = mdtime % 1000;
    return static_cast<int64_t>(h) * 3600000 + m * 60000 + s * 1000 + ms;
}

// 计算两个时间的差值（毫秒），考虑午休时间
// 返回 time2 - time1 的交易时间差（跳过午休）
// 如果 time1 在 11:30 之前，time2 在 13:00 之后，则减去午休时间
inline int64_t calculate_time_diff_ms(int32_t time1, int32_t time2) {
    int64_t ms1 = mdtime_to_ms(time1);
    int64_t ms2 = mdtime_to_ms(time2);
    int64_t diff = ms2 - ms1;

    // 检查是否跨越午休时间
    // 情况1: time1 在午休前，time2 在午休后
    if (time1 < MORNING_END && time2 >= AFTERNOON_START) {
        diff -= LUNCH_BREAK_MS;
    }
    // 情况2: time1 在午休后，time2 在午休前（反向计算时）
    else if (time1 >= AFTERNOON_START && time2 < MORNING_END) {
        diff += LUNCH_BREAK_MS;
    }

    return diff;
}

// 判断两个时间是否在指定毫秒数内（用于检测时间窗口）
inline bool is_within_ms(int32_t time1, int32_t time2, int64_t threshold_ms) {
    int64_t diff = calculate_time_diff_ms(time1, time2);
    return (diff >= 0 && diff <= threshold_ms);
}

// 格式化 MDTime 为可读字符串
// 例如: 093015500 -> "09:30:15.500"
inline std::string format_mdtime(int32_t mdtime) {
    int32_t h = mdtime / 10000000;
    int32_t m = (mdtime / 100000) % 100;
    int32_t s = (mdtime / 1000) % 100;
    int32_t ms = mdtime % 1000;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", h, m, s, ms);
    return std::string(buf);
}

} // namespace time_util

#endif // TIME_UTIL_H
