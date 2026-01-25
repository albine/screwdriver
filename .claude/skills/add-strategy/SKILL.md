---
name: add-strategy
description: Add a new trading strategy to the project. Use when user wants to create, add, or implement a new strategy, or mentions "新策略", "添加策略", "创建策略".
---

# Add New Strategy

当用户需要添加新策略时，按以下流程操作。

## 流程概述

1. **运行脚本** - 使用 `add_strategy.py` 自动生成代码框架
2. **实现逻辑** - 编辑生成的策略文件
3. **编译验证** - 运行 `./build.sh engine`
4. **配置测试** - 添加到回测配置并测试

## 第一步：运行自动化脚本

```bash
python script/add_strategy.py StrategyName [--id N] [--desc "描述"]
```

**参数说明**：
- `StrategyName`: 策略名称（不含 Strategy 后缀，脚本会自动添加）
- `--id N`: 可选，指定策略 ID（1-251），默认自动分配
- `--desc`: 可选，策略描述

**示例**：
```bash
python script/add_strategy.py VolumeSpike
python script/add_strategy.py VolumeSpike --id 10 --desc "成交量突破策略"
```

**脚本自动完成**：
1. 创建 `src/strategy/{StrategyName}Strategy.h`
2. 更新 `include/strategy_ids.h`（ID 定义和映射）
3. 更新 `src/main.cpp`（注册策略）
4. 更新 `config/strategy_backtest.conf`（可用策略列表）

## 第二步：实现策略逻辑

编辑生成的策略文件 `src/strategy/{StrategyName}Strategy.h`：

### 关键方法

| 方法 | 用途 | 触发时机 |
|------|------|----------|
| `on_start()` | 初始化 | 策略启动时 |
| `on_stop()` | 清理 | 策略停止时 |
| `on_tick(stock)` | 处理快照行情 | 收到股票快照时 |
| `on_order(order, book)` | 处理委托 | 收到逐笔委托时 |
| `on_transaction(txn, book)` | 处理成交 | 收到逐笔成交时 |

### 常用工具

**价格转换**（必须使用）：
```cpp
#include "utils/symbol_utils.h"
uint32_t price_int = symbol_utils::price_to_int(12.50);  // 元 → 内部格式
double price = symbol_utils::int_to_price(125000);       // 内部格式 → 元
```

**时间工具**：
```cpp
#include "utils/time_util.h"
bool is_open = time_util::is_continuous_auction(timestamp);  // 是否连续竞价
int minutes = time_util::to_minutes(timestamp);              // 转分钟数
```

**日志**：
```cpp
LOG_M_INFO("message, value={}", value);   // 系统日志
LOG_BIZ(BIZ_ORDR, "下单 price={}", price); // 业务日志
```

### 策略状态管理

```cpp
// 启用/禁用检查
if (!is_enabled()) return;

// 手动禁用策略
disable();

// 信号触发后的状态
signal_triggered_ = true;  // 标记已触发，避免重复
```

## 第三步：编译

```bash
./build.sh engine
```

修复所有编译错误后继续。

## 第四步：测试

1. 编辑 `config/strategy_backtest.conf`，添加测试配置：
   ```
   000001,{StrategyName}Strategy
   ```

2. 运行回测：
   ```bash
   python run_backtest.py YYYYMMDD
   ```

3. 检查日志：
   ```bash
   tail -f logs/backtest_biz.log
   ```

## 注意事项

- **禁止手动创建策略文件** - 必须使用 `add_strategy.py` 脚本
- **ID 范围**: 1-251（252-255 保留）
- **命名规范**: CamelCase，如 `VolumeSpike` → `VolumeSpikeStrategy`
- **价格处理**: 禁止直接 `* 10000`，必须用 `symbol_utils` 转换

## 现有策略参考

查看现有策略实现：
- `src/strategy/OpeningRangeBreakoutStrategy.h` - 开盘区间突破
- `src/strategy/BreakoutPriceVolumeStrategy.h` - 突破价量监控
