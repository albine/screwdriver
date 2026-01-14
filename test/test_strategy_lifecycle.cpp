/**
 * 策略生命周期管理单元测试
 *
 * 测试重构后的 StrategyEngine 所有权管理功能
 */

#include <iostream>
#include <cassert>
#include <memory>
#include "../include/strategy_engine.h"
#include "../src/strategy/TestStrategy.h"
#include "../src/strategy/PrintStrategy.h"

// 简单的测试框架宏
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running test: " #name "..."; \
    test_##name(); \
    std::cout << " PASSED" << std::endl; \
} while(0)

// ==========================================
// 测试1：基本注册和所有权转移
// ==========================================
TEST(basic_registration) {
    StrategyEngine engine;

    // 创建策略
    auto strategy1 = std::make_unique<TestStrategy>("test1");
    auto strategy2 = std::make_unique<PrintStrategy>("print1");

    // 注册并转移所有权
    engine.register_strategy("600000.SH", std::move(strategy1));
    engine.register_strategy("000001.SZ", std::move(strategy2));

    // 验证策略数量
    assert(engine.strategy_count() == 2);

    // strategy1 和 strategy2 现在为 nullptr（所有权已转移）
    assert(strategy1 == nullptr);
    assert(strategy2 == nullptr);
}

// ==========================================
// 测试2：空指针处理
// ==========================================
TEST(null_pointer_handling) {
    StrategyEngine engine;

    // 注册空指针（应该被忽略）
    engine.register_strategy("600000.SH", nullptr);

    // 验证没有注册
    assert(engine.strategy_count() == 0);
}

// ==========================================
// 测试3：策略移除
// ==========================================
TEST(strategy_removal) {
    StrategyEngine engine;

    // 注册多个策略
    engine.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));
    engine.register_strategy("000001.SZ", std::make_unique<TestStrategy>("test2"));
    engine.register_strategy("600016.SH", std::make_unique<TestStrategy>("test3"));

    assert(engine.strategy_count() == 3);

    // 移除一个策略
    engine.unregister_strategy("000001.SZ");
    assert(engine.strategy_count() == 2);

    // 移除不存在的策略（应该安全）
    engine.unregister_strategy("NOTEXIST.SH");
    assert(engine.strategy_count() == 2);

    // 移除剩余策略
    engine.unregister_strategy("600000.SH");
    engine.unregister_strategy("600016.SH");
    assert(engine.strategy_count() == 0);
}

// ==========================================
// 测试4：不同symbol注册
// ==========================================
TEST(different_symbols) {
    StrategyEngine engine;

    // 注册不同 symbol
    engine.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));
    assert(engine.strategy_count() == 1);

    engine.register_strategy("000001.SZ", std::make_unique<PrintStrategy>("print1"));
    assert(engine.strategy_count() == 2);

    engine.register_strategy("600016.SH", std::make_unique<TestStrategy>("test2"));
    assert(engine.strategy_count() == 3);
}

// ==========================================
// 测试5：析构顺序安全性
// ==========================================
TEST(destruction_order_safety) {
    // 测试1：正常顺序
    {
        StrategyEngine engine;
        engine.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));
    }  // engine 析构，自动清理策略

    // 测试2：多个作用域
    {
        StrategyEngine engine;
        {
            auto strategy = std::make_unique<TestStrategy>("test1");
            engine.register_strategy("600000.SH", std::move(strategy));
        }  // strategy 已经转移，这里不会析构

        // engine 仍然持有策略
        assert(engine.strategy_count() == 1);
    }  // engine 析构，清理策略

    // 测试3：提前移除
    {
        StrategyEngine engine;
        engine.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));
        engine.unregister_strategy("600000.SH");  // 提前释放
        assert(engine.strategy_count() == 0);
    }  // engine 析构，没有策略需要清理
}

// ==========================================
// 测试6：多个 Engine 实例独立性
// ==========================================
TEST(multiple_engines) {
    StrategyEngine engine1;
    StrategyEngine engine2;

    // 分别注册策略
    engine1.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));
    engine2.register_strategy("000001.SZ", std::make_unique<TestStrategy>("test2"));

    // 验证独立性
    assert(engine1.strategy_count() == 1);
    assert(engine2.strategy_count() == 1);

    // 移除互不影响
    engine1.unregister_strategy("600000.SH");
    assert(engine1.strategy_count() == 0);
    assert(engine2.strategy_count() == 1);  // 不受影响
}

// ==========================================
// 测试7：大量策略注册和释放
// ==========================================
TEST(bulk_operations) {
    StrategyEngine engine;

    // 注册1000个策略
    for (int i = 0; i < 1000; ++i) {
        std::string symbol = std::to_string(600000 + i) + ".SH";
        engine.register_strategy(symbol, std::make_unique<TestStrategy>("test" + std::to_string(i)));
    }

    assert(engine.strategy_count() == 1000);

    // 移除一半
    for (int i = 0; i < 500; ++i) {
        std::string symbol = std::to_string(600000 + i) + ".SH";
        engine.unregister_strategy(symbol);
    }

    assert(engine.strategy_count() == 500);

    // 清空
    for (int i = 500; i < 1000; ++i) {
        std::string symbol = std::to_string(600000 + i) + ".SH";
        engine.unregister_strategy(symbol);
    }

    assert(engine.strategy_count() == 0);
}

// ==========================================
// 测试8：异常安全性
// ==========================================
class ThrowingStrategy : public Strategy {
public:
    ThrowingStrategy(bool throw_on_start = false)
        : throw_on_start_(throw_on_start) {}

    void on_start() override {
        if (throw_on_start_) {
            throw std::runtime_error("Intentional exception in on_start");
        }
    }

private:
    bool throw_on_start_;
};

TEST(exception_safety) {
    StrategyEngine engine;

    // 注册正常策略
    engine.register_strategy("600000.SH", std::make_unique<TestStrategy>("test1"));

    // 注册会抛异常的策略（注册本身不会抛异常）
    engine.register_strategy("000001.SZ", std::make_unique<ThrowingStrategy>(true));

    assert(engine.strategy_count() == 2);

    // 即使策略在 on_start 中抛异常，engine 仍然应该能正常管理生命周期
    try {
        engine.start();
    } catch (const std::exception& e) {
        // 捕获异常，继续测试
    }

    // 仍然可以移除策略
    engine.unregister_strategy("000001.SZ");
    assert(engine.strategy_count() == 1);
}

// ==========================================
// Main 测试入口
// ==========================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "策略生命周期管理单元测试" << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(basic_registration);
    RUN_TEST(null_pointer_handling);
    RUN_TEST(strategy_removal);
    RUN_TEST(different_symbols);
    RUN_TEST(destruction_order_safety);
    RUN_TEST(multiple_engines);
    RUN_TEST(bulk_operations);
    RUN_TEST(exception_safety);

    std::cout << "========================================" << std::endl;
    std::cout << "所有测试通过！✓" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
