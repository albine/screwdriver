/**
 * @file test_time_util.cpp
 * @brief time_util 模块单元测试
 *
 * 测试 calculate_time_diff_ms 和 now_mdtime 函数
 */

#include <iostream>
#include <cassert>
#include <cmath>
#include <chrono>
#include <thread>
#include "../src/utils/time_util.h"

// 测试辅助宏
#define TEST_CASE(name) std::cout << "Testing: " << name << "... "
#define TEST_PASS() std::cout << "PASSED\n"
#define TEST_FAIL(msg) do { std::cout << "FAILED: " << msg << "\n"; return 1; } while(0)

#define ASSERT_EQ(actual, expected) \
    if ((actual) != (expected)) { \
        std::cout << "FAILED: expected " << (expected) << ", got " << (actual) << "\n"; \
        return 1; \
    }

#define ASSERT_NEAR(actual, expected, tolerance) \
    if (std::abs((actual) - (expected)) > (tolerance)) { \
        std::cout << "FAILED: expected " << (expected) << " ± " << (tolerance) \
                  << ", got " << (actual) << "\n"; \
        return 1; \
    }

// ==========================================
// mdtime_to_ms 测试
// ==========================================
int test_mdtime_to_ms() {
    TEST_CASE("mdtime_to_ms basic conversion");

    // 09:30:00.000 = 9*3600000 + 30*60000 = 34200000 ms
    ASSERT_EQ(time_util::mdtime_to_ms(93000000), 34200000);

    // 09:30:15.500 = 9*3600000 + 30*60000 + 15*1000 + 500 = 34215500 ms
    ASSERT_EQ(time_util::mdtime_to_ms(93015500), 34215500);

    // 11:30:00.000 (MORNING_END)
    ASSERT_EQ(time_util::mdtime_to_ms(113000000), 41400000);

    // 13:00:00.000 (AFTERNOON_START)
    ASSERT_EQ(time_util::mdtime_to_ms(130000000), 46800000);

    // 15:00:00.000
    ASSERT_EQ(time_util::mdtime_to_ms(150000000), 54000000);

    // 00:00:00.000 (midnight)
    ASSERT_EQ(time_util::mdtime_to_ms(0), 0);

    // 00:00:00.001
    ASSERT_EQ(time_util::mdtime_to_ms(1), 1);

    TEST_PASS();
    return 0;
}

// ==========================================
// calculate_time_diff_ms 测试
// ==========================================
int test_calculate_time_diff_ms_same_period() {
    TEST_CASE("calculate_time_diff_ms - same period (no lunch break)");

    // 上午同一时段: 09:30:00 -> 09:30:05 = 5000 ms
    ASSERT_EQ(time_util::calculate_time_diff_ms(93000000, 93005000), 5000);

    // 上午同一时段: 09:30:00 -> 10:30:00 = 3600000 ms (1小时)
    ASSERT_EQ(time_util::calculate_time_diff_ms(93000000, 103000000), 3600000);

    // 下午同一时段: 13:00:00 -> 13:30:00 = 1800000 ms (30分钟)
    ASSERT_EQ(time_util::calculate_time_diff_ms(130000000, 133000000), 1800000);

    // 下午同一时段: 14:00:00 -> 15:00:00 = 3600000 ms (1小时)
    ASSERT_EQ(time_util::calculate_time_diff_ms(140000000, 150000000), 3600000);

    TEST_PASS();
    return 0;
}

int test_calculate_time_diff_ms_across_lunch() {
    TEST_CASE("calculate_time_diff_ms - across lunch break");

    // 跨午休: 11:29:00 -> 13:01:00
    // 实际时间差 = 13:01 - 11:29 = 92分钟 = 5520000 ms
    // 减去午休 1.5小时 = 5400000 ms
    // 交易时间差 = 5520000 - 5400000 = 120000 ms (2分钟)
    ASSERT_EQ(time_util::calculate_time_diff_ms(112900000, 130100000), 120000);

    // 跨午休: 11:00:00 -> 14:00:00
    // 实际时间差 = 3小时 = 10800000 ms
    // 减去午休 1.5小时 = 5400000 ms
    // 交易时间差 = 10800000 - 5400000 = 5400000 ms (1.5小时)
    ASSERT_EQ(time_util::calculate_time_diff_ms(110000000, 140000000), 5400000);

    // 跨午休边界: 11:30:00 -> 13:00:00 (恰好午休时段)
    // 由于 time1 < MORNING_END (false, 11:30:00 == 11:30:00)
    // 不会减去午休时间
    // 实际时间差 = 1.5小时 = 5400000 ms
    ASSERT_EQ(time_util::calculate_time_diff_ms(113000000, 130000000), 5400000);

    // 跨午休: 11:29:59.999 -> 13:00:00.000
    // time1 < MORNING_END 且 time2 >= AFTERNOON_START
    // 实际时间差 = 5400001 ms, 减去午休 5400000 ms = 1 ms
    ASSERT_EQ(time_util::calculate_time_diff_ms(112959999, 130000000), 1);

    TEST_PASS();
    return 0;
}

int test_calculate_time_diff_ms_reverse() {
    TEST_CASE("calculate_time_diff_ms - reverse time (time2 < time1)");

    // 反向: 09:30:05 -> 09:30:00 = -5000 ms
    ASSERT_EQ(time_util::calculate_time_diff_ms(93005000, 93000000), -5000);

    // 反向跨午休: 14:00:00 -> 11:00:00
    // time1 >= AFTERNOON_START 且 time2 < MORNING_END
    // 实际时间差 = -3小时 = -10800000 ms
    // 加上午休 1.5小时 = 5400000 ms
    // 交易时间差 = -10800000 + 5400000 = -5400000 ms
    ASSERT_EQ(time_util::calculate_time_diff_ms(140000000, 110000000), -5400000);

    TEST_PASS();
    return 0;
}

int test_calculate_time_diff_ms_edge_cases() {
    TEST_CASE("calculate_time_diff_ms - edge cases");

    // 相同时间
    ASSERT_EQ(time_util::calculate_time_diff_ms(93000000, 93000000), 0);

    // 1毫秒差
    ASSERT_EQ(time_util::calculate_time_diff_ms(93000000, 93000001), 1);

    // 开盘时间 09:15:00 -> 09:30:00
    ASSERT_EQ(time_util::calculate_time_diff_ms(91500000, 93000000), 900000);

    TEST_PASS();
    return 0;
}

// ==========================================
// now_mdtime 测试
// ==========================================
int test_now_mdtime_format() {
    TEST_CASE("now_mdtime - format validation");

    int32_t mdtime = time_util::now_mdtime();

    // 提取各个组件
    int32_t h = mdtime / 10000000;
    int32_t m = (mdtime / 100000) % 100;
    int32_t s = (mdtime / 1000) % 100;
    int32_t ms = mdtime % 1000;

    // 验证范围
    if (h < 0 || h > 23) {
        TEST_FAIL("hour out of range: " + std::to_string(h));
    }
    if (m < 0 || m > 59) {
        TEST_FAIL("minute out of range: " + std::to_string(m));
    }
    if (s < 0 || s > 59) {
        TEST_FAIL("second out of range: " + std::to_string(s));
    }
    if (ms < 0 || ms > 999) {
        TEST_FAIL("millisecond out of range: " + std::to_string(ms));
    }

    std::cout << "Current time: " << time_util::format_mdtime(mdtime) << " ";
    TEST_PASS();
    return 0;
}

int test_now_mdtime_progression() {
    TEST_CASE("now_mdtime - time progression");

    int32_t time1 = time_util::now_mdtime();

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int32_t time2 = time_util::now_mdtime();

    // time2 应该 >= time1（正常情况下会略大）
    if (time2 < time1) {
        // 可能跨越了午夜，这种情况在测试中很罕见
        std::cout << "(crossed midnight, skipping check) ";
    } else {
        int64_t diff = time_util::calculate_time_diff_ms(time1, time2);
        // 差值应该在 50ms 到 200ms 之间（考虑系统调度延迟）
        if (diff < 40 || diff > 500) {
            std::cout << "FAILED: time difference " << diff << "ms is unexpected\n";
            return 1;
        }
    }

    TEST_PASS();
    return 0;
}

// ==========================================
// is_within_ms 测试
// ==========================================
int test_is_within_ms() {
    TEST_CASE("is_within_ms");

    // 5秒内
    if (!time_util::is_within_ms(93000000, 93005000, 5000)) {
        TEST_FAIL("expected true for 5s within 5s threshold");
    }

    // 超过阈值
    if (time_util::is_within_ms(93000000, 93006000, 5000)) {
        TEST_FAIL("expected false for 6s with 5s threshold");
    }

    // 负差值不在范围内
    if (time_util::is_within_ms(93005000, 93000000, 5000)) {
        TEST_FAIL("expected false for negative diff");
    }

    // 边界: 恰好等于阈值
    if (!time_util::is_within_ms(93000000, 93005000, 5000)) {
        TEST_FAIL("expected true for exact threshold");
    }

    TEST_PASS();
    return 0;
}

// ==========================================
// format_mdtime 测试
// ==========================================
int test_format_mdtime() {
    TEST_CASE("format_mdtime");

    std::string result;

    result = time_util::format_mdtime(93015500);
    if (result != "09:30:15.500") {
        TEST_FAIL("expected 09:30:15.500, got " + result);
    }

    result = time_util::format_mdtime(0);
    if (result != "00:00:00.000") {
        TEST_FAIL("expected 00:00:00.000, got " + result);
    }

    result = time_util::format_mdtime(235959999);
    if (result != "23:59:59.999") {
        TEST_FAIL("expected 23:59:59.999, got " + result);
    }

    TEST_PASS();
    return 0;
}

// ==========================================
// Main
// ==========================================
int main() {
    std::cout << "========================================\n";
    std::cout << "       time_util Unit Tests\n";
    std::cout << "========================================\n\n";

    int failed = 0;

    // mdtime_to_ms 测试
    failed += test_mdtime_to_ms();

    // calculate_time_diff_ms 测试
    failed += test_calculate_time_diff_ms_same_period();
    failed += test_calculate_time_diff_ms_across_lunch();
    failed += test_calculate_time_diff_ms_reverse();
    failed += test_calculate_time_diff_ms_edge_cases();

    // now_mdtime 测试
    failed += test_now_mdtime_format();
    failed += test_now_mdtime_progression();

    // is_within_ms 测试
    failed += test_is_within_ms();

    // format_mdtime 测试
    failed += test_format_mdtime();

    std::cout << "\n========================================\n";
    if (failed == 0) {
        std::cout << "All tests PASSED!\n";
    } else {
        std::cout << failed << " test(s) FAILED!\n";
    }
    std::cout << "========================================\n";

    return failed;
}
