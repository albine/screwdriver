# 策略生命周期设计问题分析

## 当前设计的问题

### 1. 所有权和使用权分离

```cpp
// src/main.cpp:78-81, 116-118
StrategyEngine engine;                              // 使用者
std::vector<std::unique_ptr<Strategy>> strategies;  // 所有者

auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
engine.register_strategy(cfg.symbol, strategy.get());  // 传递裸指针
strategies.push_back(std::move(strategy));              // 转移所有权
```

**问题：**
- `strategies` 拥有策略对象（通过 `unique_ptr`）
- `engine` 使用策略对象（通过裸指针）
- **engine 不知道也不控制 strategies 的生命周期**
- 这种分离导致了悬空指针风险

### 2. 生命周期依赖不明确

```cpp
void run_backtest_mode(...) {
    StrategyEngine engine;      // 声明顺序：1
    std::vector<...> strategies; // 声明顺序：2

    engine.register_strategy(symbol, strategy.get()); // 隐式依赖

    // 函数结束时，析构顺序：
    // 1. strategies 先析构（后声明先析构）
    // 2. engine 后析构 -> 访问已释放的策略 -> 💥
}
```

**问题：**
- **隐式依赖**：engine 依赖 strategies，但这个依赖是隐式的
- **脆弱性**：仅靠声明顺序来保证正确性，极易出错
- **无类型保护**：C++ 无法在编译期检查这种生命周期依赖

### 3. 接口设计的误导性

```cpp
// include/strategy_engine.h:116-119
void register_strategy(const std::string& symbol, Strategy* strat) {
    int shard_id = get_shard_id(symbol);
    registry_[shard_id][symbol].push_back(strat);
}
```

**接口暗示：**
- 接受裸指针 → "我不管理生命周期"（观察者模式）
- 只是注册，不获取所有权

**实际行为：**
```cpp
// include/strategy_engine.h:147-154
void stop() {
    for (int i = 0; i < SHARD_COUNT; ++i) {
        for (auto& kv : registry_[i]) {
            for (auto* strat : kv.second) {
                strat->on_stop();  // ❌ 需要保证指针有效！
            }
        }
    }
}
```

**矛盾：**
- 接口说"不管理"，但实际需要保证生命周期
- 这是一个**接口谎言（Interface Lie）**

### 4. 违反 RAII 原则

RAII（Resource Acquisition Is Initialization）要求：
- 资源获取即初始化
- 对象自己管理资源的生命周期

**当前设计违反了这一原则：**
```cpp
StrategyEngine engine;  // engine 不是自包含的
// engine 的正确运行依赖外部的 strategies vector
// 这不是真正的 RAII
```

## 设计缺陷的影响

### 易错性极高

```cpp
// 错误1：声明顺序颠倒
std::vector<std::unique_ptr<Strategy>> strategies;  // 先声明
StrategyEngine engine;                              // 后声明
// 结果：engine 先析构，strategies 后析构 -> 段错误！

// 错误2：提前释放
{
    std::vector<std::unique_ptr<Strategy>> strategies;
    engine.register_strategy(symbol, strategy.get());
}  // strategies 作用域结束，被析构
// engine 还在使用这些指针 -> 悬空指针！

// 错误3：异常安全
try {
    strategies.push_back(create_strategy());
} catch (...) {
    engine.stop();  // 可能访问部分构造的策略
}
```

### 维护困难

- 代码审查时很难发现问题
- 新开发者容易犯错
- 重构时容易引入bug

### 扩展性差

```cpp
// 想要在运行时动态添加/删除策略？
void add_strategy_at_runtime(std::unique_ptr<Strategy> strat) {
    // 谁来管理这个新策略的生命周期？
    // strategies vector 在哪？
    // 如何保证线程安全？
}
```

## 根本原因：架构选择不明确

当前设计混合了两种模式，但没有明确选择：

### 模式1：组合模式（Composition）
- Engine **拥有** 策略
- Engine 管理策略的完整生命周期
- 策略是 Engine 的一部分

### 模式2：观察者模式（Observer）
- Engine **观察** 策略
- 策略独立存在
- Engine 只是接收通知

**当前设计的混乱：**
- 接口看起来像观察者模式（裸指针）
- 行为像组合模式（调用 on_stop）
- 实际是混乱的中间状态

## 推荐的重构方案

### 方案1：Engine 拥有策略（推荐⭐）

```cpp
class StrategyEngine {
private:
    // Engine 拥有所有策略
    std::unordered_map<std::string, std::unique_ptr<Strategy>> owned_strategies_;

    // 按 shard 分片的策略指针（用于快速查找）
    std::vector<StrategyMap> registry_;  // StrategyMap = unordered_map<string, vector<Strategy*>>

public:
    // 注册时转移所有权
    void register_strategy(const std::string& symbol, std::unique_ptr<Strategy> strat) {
        int shard_id = get_shard_id(symbol);
        Strategy* raw_ptr = strat.get();

        // 1. 保存所有权
        owned_strategies_[symbol] = std::move(strat);

        // 2. 注册裸指针到分片
        registry_[shard_id][symbol].push_back(raw_ptr);
    }

    // 可选：运行时移除策略
    void unregister_strategy(const std::string& symbol) {
        int shard_id = get_shard_id(symbol);

        // 1. 从 registry 移除
        registry_[shard_id].erase(symbol);

        // 2. 释放所有权（自动调用析构）
        owned_strategies_.erase(symbol);
    }
};

// 使用示例
void run_backtest_mode(...) {
    StrategyEngine engine;

    for (const auto& cfg : configs) {
        // 直接转移所有权，不需要外部 vector！
        auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
        engine.register_strategy(cfg.symbol, std::move(strategy));
    }

    engine.start();
    // ... 回测逻辑
    engine.stop();

    // engine 析构时，自动清理所有策略
    // 生命周期完全由 engine 管理，安全！
}
```

**优点：**
- ✅ 明确的所有权：Engine 拥有策略
- ✅ 自包含：不依赖外部生命周期
- ✅ 符合 RAII 原则
- ✅ 易于理解和使用
- ✅ 支持动态添加/删除策略
- ✅ 线程安全（如果加锁）

**缺点：**
- 需要修改接口
- 需要重构现有代码

### 方案2：共享所有权（备选）

```cpp
class StrategyEngine {
private:
    using StrategyMap = std::unordered_map<std::string, std::vector<std::shared_ptr<Strategy>>>;
    std::vector<StrategyMap> registry_;

public:
    void register_strategy(const std::string& symbol, std::shared_ptr<Strategy> strat) {
        int shard_id = get_shard_id(symbol);
        registry_[shard_id][symbol].push_back(strat);
    }
};

// 使用
void run_backtest_mode(...) {
    StrategyEngine engine;
    std::vector<std::shared_ptr<Strategy>> strategies;  // 外部也可以持有

    for (const auto& cfg : configs) {
        auto strategy = std::make_shared<SomeStrategy>(cfg.symbol);
        engine.register_strategy(cfg.symbol, strategy);
        strategies.push_back(strategy);  // 可选：外部保留引用
    }

    // 析构顺序无所谓，引用计数保证安全
}
```

**优点：**
- ✅ 灵活：多处可以持有策略
- ✅ 自动生命周期管理
- ✅ 析构顺序无关紧要

**缺点：**
- ❌ 性能开销（引用计数的原子操作）
- ❌ 所有权不明确（谁真正拥有？）
- ❌ 循环引用风险

### 方案3：生命周期参数（理论上的最佳方案）

如果 C++ 有 Rust 的生命周期标注：

```rust
// 伪代码（C++ 不支持）
struct StrategyEngine<'a> {
    strategies: Vec<&'a mut Strategy>,  // 'a 表示生命周期
}

// 编译器保证：strategies 的生命周期 >= engine
```

但 C++ 无法表达，只能靠文档和约定。

### 方案4：当前的临时修复（已实现）

```cpp
void stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;  // 防止重复调用
    }
    // ...
}
```

**评价：**
- ✅ 快速修复，防止崩溃
- ✅ 最小改动
- ❌ **治标不治本**
- ❌ 设计问题依然存在
- ❌ 容易在其他场景出错

## 对比分析

| 方案 | 所有权清晰度 | RAII | 易用性 | 性能 | 重构成本 |
|------|------------|------|--------|------|---------|
| **方案1：Engine拥有** | ⭐⭐⭐⭐⭐ | ✅ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 中等 |
| 方案2：共享所有权 | ⭐⭐⭐ | ✅ | ⭐⭐⭐⭐ | ⭐⭐⭐ | 中等 |
| 方案4：当前临时修复 | ⭐ | ❌ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 低 |

## 推荐的重构路径

### 第一阶段：立即（已完成）
- ✅ 添加 `stopped_` 标志，防止段错误
- ✅ 文档说明生命周期要求

### 第二阶段：短期（建议）
- 重构 `StrategyEngine`，改为方案1（Engine拥有策略）
- 修改 `register_strategy` 接口：
  ```cpp
  void register_strategy(const std::string& symbol, std::unique_ptr<Strategy> strat)
  ```
- 移除 `run_backtest_mode` 中的 `strategies` vector
- 更新所有调用点

### 第三阶段：中期（可选）
- 添加运行时策略管理：
  - `add_strategy_runtime()`
  - `remove_strategy_runtime()`
- 考虑策略热插拔
- 添加策略生命周期钩子：
  - `on_register()`
  - `on_unregister()`

### 第四阶段：长期（架构优化）
- 策略池管理
- 策略工厂与 Engine 集成
- 策略配置热重载

## 总结

**你的直觉是对的**：这个段错误不是偶然的 bug，而是暴露了深层的设计问题：

1. **所有权混乱**：谁拥有策略？
2. **生命周期依赖隐式**：engine 依赖 strategies，但无类型保护
3. **接口误导**：裸指针暗示"不管理"，实际需要保证有效性
4. **违反 RAII**：不是自包含的资源管理

**根本解决方案**是重构为方案1（Engine拥有策略），从架构上消除这类问题。

**当前的 `stopped_` 标志**只是临时止血，真正的治疗需要手术（重构）。

## 参考资料

- [C++ Core Guidelines: R.3 - A raw pointer is non-owning](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rr-ptr)
- [Herb Sutter: "Leak-Freedom in C++... By Default"](https://herbsutter.com/2013/06/05/gotw-91-solution-smart-pointer-parameters/)
- [RAII 原则](https://en.cppreference.com/w/cpp/language/raii)
