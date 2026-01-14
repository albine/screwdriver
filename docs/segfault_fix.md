# 段错误修复说明

## 问题描述

回测程序在完成所有任务后，退出时发生段错误（SIGSEGV）：

```
2026-01-14 13:10:09.846198651 [INFO] [145760] [Strategy] ========================================
Segmentation fault (core dumped)
```

## 根本原因分析

### 1. 对象析构顺序问题

在 `run_backtest_mode` 函数中（src/main.cpp:78-81）：

```cpp
StrategyEngine engine;                              // 第78行：先声明
std::vector<std::unique_ptr<Strategy>> strategies;  // 第81行：后声明
```

**C++析构顺序规则：按声明的逆序析构**

当函数结束时：
1. 先析构 `strategies`（后声明的先析构）→ 所有策略对象被释放
2. 再析构 `engine` → 调用 `StrategyEngine::~StrategyEngine()`

### 2. 悬空指针访问

```cpp
// strategy_engine.h:104-106
~StrategyEngine() {
    stop();  // 析构函数调用stop()
}
```

```cpp
// strategy_engine.h:141-154
void stop() {
    running_ = false;
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }

    // 调用所有策略的 on_stop()
    for (int i = 0; i < SHARD_COUNT; ++i) {
        for (auto& kv : registry_[i]) {
            for (auto* strat : kv.second) {
                strat->on_stop();  // ⚠️ 访问悬空指针！
            }
        }
    }
}
```

**问题时序：**
1. `run_backtest_mode` 显式调用 `engine.stop()`（第一次）
2. 函数结束，`strategies` 被析构，策略对象被释放
3. `engine` 被析构，调用 `stop()`（第二次）
4. `stop()` 尝试访问 `registry_` 中的策略指针
5. **这些指针指向已释放的内存 → 段错误**

### 3. 内核日志分析

```
engine[146231]: segfault at 55d0006aaae6 ip 000055d51ebd3243
Code: ... <ff> 50 18 ...  # call [rax+0x18]
```

- `ff 50 18` 是 `call [rax+0x18]` 指令，这是虚函数调用
- RAX 中存储的是已释放对象的虚函数表指针
- 访问无效内存导致段错误

## 修复方案

### 方案1：防止重复调用 stop()（已采用）

在 `StrategyEngine` 中添加 `stopped_` 标志：

```cpp
// strategy_engine.h 修改
private:
    std::atomic<bool> running_{true};
    std::atomic<bool> stopped_{false};  // 新增

void stop() {
    // 防止重复调用
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;  // 已经停止过了
    }

    running_ = false;
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }

    // 调用所有策略的 on_stop()
    for (int i = 0; i < SHARD_COUNT; ++i) {
        for (auto& kv : registry_[i]) {
            for (auto* strat : kv.second) {
                strat->on_stop();
            }
        }
    }
}
```

**优点：**
- 安全可靠，防止任何重复调用
- 使用原子操作，线程安全
- 不改变用户代码的使用方式

### 其他可选方案（未采用）

#### 方案2：调整变量声明顺序

```cpp
std::vector<std::unique_ptr<Strategy>> strategies;  // 先声明
StrategyEngine engine;                              // 后声明
```

缺点：依赖声明顺序，容易被忽略

#### 方案3：移除析构函数中的 stop()

要求用户必须显式调用 `stop()`，违反 RAII 原则

#### 方案4：使用智能指针管理策略

需要大规模重构，破坏现有接口

## 验证结果

修复后程序正常退出：

```bash
$ export LD_LIBRARY_PATH=./fastfish/libs:$LD_LIBRARY_PATH
$ ./build/engine backtest
...
2026-01-14 13:14:33.769545211 [INFO] [148958] [Strategy] ========================================
$ echo $?
0
```

## 经验教训

1. **对象生命周期管理**：持有裸指针时，必须确保指向的对象在指针使用期间有效
2. **析构函数的幂等性**：析构函数中的操作应该是幂等的，重复调用不应产生副作用
3. **RAII 原则**：资源管理应该绑定到对象生命周期，但要注意析构顺序
4. **原子操作**：使用 `compare_exchange_strong` 可以实现线程安全的单次执行保护

## 相关修改文件

- `include/strategy_engine.h` - 添加 `stopped_` 标志和检查逻辑
- `include/strategy_factory.h` - 修复 CreatorFunc 参数匹配问题
