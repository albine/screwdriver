# 按交易所分片 StrategyEngine 实现计划

## 需求概述

将 StrategyEngine 从当前的 4 分片改为按交易所分片：
- **上海 (6开头)**: 2301 只股票 → 24 个线程 (~96 只/线程)
- **深圳 (0/3开头)**: 2882 只股票 → 29 个线程 (~99 只/线程)
- **总计**: 53 个工作线程

## 设计方案

### 采用方案 A：统一队列数组

```
索引布局: [0..23] = 上海 (SH), [24..52] = 深圳 (SZ)
总计: 53 个队列, 53 个工作线程
```

**优势**:
- 代码结构简单，单一队列数组
- 内存连续分配
- API 改动最小

### 核心数据结构

```cpp
// 新增配置结构体
struct ExchangeShardConfig {
    int sh_shard_count = 24;    // 上海分片数
    int sz_shard_count = 29;    // 深圳分片数

    int total_shards() const { return sh_shard_count + sz_shard_count; }
};

static constexpr ExchangeShardConfig DEFAULT_EXCHANGE_CONFIG = {24, 29};
```

### 新分片函数

```cpp
inline int get_exchange_shard_id(const char* symbol, const ExchangeShardConfig& config) {
    bool is_sh = (symbol[0] == '6');

    uint64_t hash = 0;
    for (const char* p = symbol; *p && (p - symbol) < 40; ++p) {
        hash = hash * 31 + static_cast<unsigned char>(*p);
    }

    if (is_sh) {
        return static_cast<int>(hash % config.sh_shard_count);  // [0, 24)
    } else {
        return config.sh_shard_count + static_cast<int>(hash % config.sz_shard_count);  // [24, 53)
    }
}
```

## 需要修改的文件

| 文件 | 修改内容 |
|------|----------|
| `include/symbol_utils.h` | 添加 `ExchangeShardConfig` 和 `get_exchange_shard_id()` |
| `include/strategy_engine.h` | 使用新配置，动态队列数量，修改 `get_shard_id()` |
| `include/history_data_replayer.h` | 接受 `ExchangeShardConfig`，更新分片逻辑 |
| `include/backtest_adapter.h` | 构造函数从 engine 获取配置 |
| `src/main.cpp` | 更新 `BacktestAdapter` 构造 |

## 详细实现步骤

### 步骤 1: 修改 `include/symbol_utils.h`

添加配置结构体和分片函数：
- `ExchangeShardConfig` 结构体
- `get_exchange_shard_id()` 函数

### 步骤 2: 修改 `include/strategy_engine.h`

1. 删除 `static const int SHARD_COUNT = 4`
2. 添加 `ExchangeShardConfig config_` 成员
3. 构造函数接受配置参数
4. `queues_` 和 `registry_` 大小改为 `config_.total_shards()`
5. `get_producer_tokens()` 改为动态大小的 vector
6. 启动 53 个 worker 线程

### 步骤 3: 修改 `include/history_data_replayer.h`

1. 添加 `ExchangeShardConfig config_` 成员
2. 构造函数接受配置
3. `shard_events_` 大小改为 `config_.total_shards()`
4. 使用 `get_exchange_shard_id()` 替换原有分片逻辑

### 步骤 4: 修改 `include/backtest_adapter.h`

1. 构造函数从 `engine->shard_config()` 获取配置
2. 传递给 `HistoryDataReplayer`

### 步骤 5: 修改 `src/main.cpp`

1. 更新 `BacktestAdapter` 构造调用

## 内存影响

**ObjectPool 容量**: 200,000 → **500,000** (每线程)

| 项目 | 原值 | 新值 |
|-----|------|------|
| ObjectPool 容量 | 200,000 | 500,000 |
| 单线程 ObjectPool | 6.4 MB | 16 MB |
| 53 线程总计 | ~339 MB | ~848 MB |
| 原 4 线程 | ~26 MB | — |

**修改点**: `strategy_engine.h` 中 `ObjectPool<OrderNode> local_pool(500000);`

对于 64GB+ 的 HFT 服务器完全可接受。

## CPU 绑核策略

**当前决定**: 先不绑核，让 OS 自动调度

**原因**:
- 先实现功能，后优化性能
- 128 核服务器资源充足，OS 调度即可
- 后续可根据 `perf` 和 `numastat` 分析结果决定是否绑核

**未来优化方向** (如需要):
- SDK receiver/poller 绑定到固定 CPU
- Worker 线程按 NUMA 节点分组

## 验证方法

1. **编译验证**: `./build.sh engine`
2. **回测测试**: `python run_backtest.py 20260115`
3. **检查日志**: 确认所有股票正确分配到对应交易所的线程
4. **验证绑核**: `ps -eo pid,tid,psr,comm | grep engine`
5. **性能对比**: 比较新旧分片策略的延迟
