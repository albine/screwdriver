# 实盘模式配置改进

## 问题描述

### 改进前的设计不一致

**回测模式**：
```cpp
// ✅ 读取配置文件
auto configs = parse_backtest_config("config/backtest.conf");
for (const auto& cfg : configs) {
    auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
    engine.register_strategy(cfg.symbol, std::move(strategy));
}
```

**实盘模式**：
```cpp
// ❌ 硬编码
std::string symbol = "002603.SZ";  // 硬编码股票
auto strategy = std::make_unique<PrintStrategy>("LiveStrat");  // 硬编码策略
engine.register_strategy(symbol, std::move(strategy));
```

### 问题分析

| 问题 | 影响 |
|------|------|
| **硬编码策略** | 只能使用 PrintStrategy，无法灵活切换到其他策略 |
| **硬编码股票** | 只能监控单一股票 002603.SZ |
| **代码重复** | 回测和实盘的策略加载逻辑重复 |
| **不可扩展** | 无法同时运行多个策略或监控多只股票 |
| **配置管理困难** | 修改策略需要重新编译代码 |

## 改进方案

### 1. 创建实盘配置文件

**config/live.conf**：
```conf
# 实盘交易配置文件
# 格式: 股票代码,策略名称

# 示例配置
002603.SZ,PrintStrategy
600759,PriceLevelVolumeStrategy
```

### 2. 统一配置解析逻辑

改进后的实盘模式：
```cpp
void run_live_mode(quill::Logger* logger, const std::string& config_file = "config/live.conf") {
    LOG_MODULE_INFO(logger, MOD_ENGINE, "=== Live Trading Mode ===");

    // 注册所有策略
    register_all_strategies();

    // 解析配置文件（与回测模式共享同一个解析器）
    auto configs = parse_backtest_config(config_file);

    if (configs.empty()) {
        LOG_MODULE_ERROR(logger, MOD_ENGINE, "No valid configurations found in {}", config_file);
        return;
    }

    // 创建策略引擎
    StrategyEngine engine;
    auto& factory = StrategyFactory::instance();

    // 为每个配置创建策略（与回测模式逻辑一致）
    for (const auto& cfg : configs) {
        if (!factory.has_strategy(cfg.strategy_name)) {
            LOG_MODULE_WARNING(logger, MOD_ENGINE, "Unknown strategy: {}, skipping", cfg.strategy_name);
            continue;
        }

        auto strategy = factory.create(cfg.strategy_name, cfg.symbol);
        engine.register_strategy(cfg.symbol, std::move(strategy));
        LOG_MODULE_INFO(logger, MOD_ENGINE, "Registered strategy {} for {}", cfg.strategy_name, cfg.symbol);
    }

    // 启动引擎和数据适配器...
    engine.start();
    // ...
}
```

## 改进效果对比

### Before（硬编码）

```cpp
// 只能监控一只股票，使用一个策略
std::string symbol = "002603.SZ";
auto strategy = std::make_unique<PrintStrategy>("LiveStrat");
engine.register_strategy(symbol, std::move(strategy));

// 问题：
// 1. 要监控其他股票？修改代码重新编译！
// 2. 要换策略？修改代码重新编译！
// 3. 要多个策略？需要大幅修改代码！
```

### After（配置驱动）

```conf
# config/live.conf - 无需修改代码，只需编辑配置文件

# 监控单只股票
002603.SZ,PrintStrategy

# 监控多只股票
600759.SH,PriceLevelVolumeStrategy
000001.SZ,TestStrategy
603122.SH,PerformanceStrategy

# 切换策略？直接修改配置！
# 添加股票？直接添加一行！
# 删除监控？删除对应行！
```

## 优势

### 1. 灵活性

```bash
# 修改策略配置
vim config/live.conf

# 直接重启，无需重新编译
./build/engine live
```

### 2. 一致性

| 方面 | 回测模式 | 实盘模式 |
|------|---------|---------|
| 配置方式 | ✅ 配置文件 | ✅ 配置文件 |
| 策略加载 | ✅ 工厂模式 | ✅ 工厂模式 |
| 多策略支持 | ✅ 支持 | ✅ 支持 |
| 代码逻辑 | ✅ 统一 | ✅ 统一 |

### 3. 可扩展性

```conf
# 轻松扩展到监控100只股票
600000.SH,PriceLevelVolumeStrategy
600001.SH,PriceLevelVolumeStrategy
600002.SH,PriceLevelVolumeStrategy
# ... 更多
```

### 4. 测试友好

```bash
# 开发环境
./build/engine live config/live_dev.conf

# 生产环境
./build/engine live config/live_prod.conf

# A/B测试
./build/engine live config/live_test_strategy_a.conf
./build/engine live config/live_test_strategy_b.conf
```

## 使用方式

### 默认配置

```bash
# 使用默认配置文件 config/live.conf
./build/engine live
```

### 指定配置文件

```bash
# 使用自定义配置文件
./build/engine live config/my_custom.conf
```

### 配置文件格式

```conf
# 注释行以 # 开头
# 空行会被忽略

# 格式：股票代码,策略名称
600759,PriceLevelVolumeStrategy    # 带交易所后缀
002603.SZ,PrintStrategy             # 不带后缀会自动识别
000001,TestStrategy                 # 0/2/3开头 -> .SZ
                                    # 6开头 -> .SH
```

## 配置示例

### 示例1：生产环境监控

```conf
# config/live_prod.conf
# 生产环境：监控核心股票

# 主力股票
600759.SH,PriceLevelVolumeStrategy
000001.SZ,PriceLevelVolumeStrategy

# 备选股票
600016.SH,TestStrategy
```

### 示例2：开发调试

```conf
# config/live_dev.conf
# 开发环境：调试策略

# 单只股票，打印详细信息
002603.SZ,PrintStrategy
```

### 示例3：性能测试

```conf
# config/live_perf.conf
# 性能测试：多股票高负载

600000.SH,PerformanceStrategy
600001.SH,PerformanceStrategy
600002.SH,PerformanceStrategy
# ... 更多股票
```

## 代码复用

### 共享配置解析器

```cpp
// include/backtest_config.h
// 回测和实盘共享同一个配置解析器

inline std::vector<BacktestEntry> parse_backtest_config(const std::string& filepath) {
    // 统一的配置解析逻辑
    // 支持注释、空行、自动补全交易所后缀
    // ...
}
```

**优势：**
- ✅ 单一数据源（DRY原则）
- ✅ 统一的配置格式
- ✅ 统一的错误处理
- ✅ 更容易维护

### 共享策略加载逻辑

```cpp
// 回测和实盘使用相同的策略加载流程：
for (const auto& cfg : configs) {
    // 1. 检查策略是否存在
    if (!factory.has_strategy(cfg.strategy_name)) {
        LOG_WARNING("Unknown strategy");
        continue;
    }

    // 2. 创建策略
    auto strategy = factory.create(cfg.strategy_name, cfg.symbol);

    // 3. 注册到引擎
    engine.register_strategy(cfg.symbol, std::move(strategy));
}
```

## 迁移指南

### 对于现有代码

如果你之前硬编码了策略：

**步骤1：创建配置文件**
```bash
cat > config/live.conf << EOF
# 你之前硬编码的配置
002603.SZ,PrintStrategy
EOF
```

**步骤2：重新编译**
```bash
cmake --build build --target engine -j$(nproc)
```

**步骤3：运行**
```bash
./build/engine live
```

就是这么简单！

### 添加新策略

**步骤1：编辑配置文件**
```bash
echo "600759.SH,PriceLevelVolumeStrategy" >> config/live.conf
```

**步骤2：重启程序**
```bash
# 无需重新编译！
./build/engine live
```

## 最佳实践

### 1. 环境分离

```
config/
  ├── backtest.conf          # 回测配置
  ├── live.conf              # 默认实盘配置
  ├── live_dev.conf          # 开发环境
  ├── live_test.conf         # 测试环境
  └── live_prod.conf         # 生产环境
```

### 2. 版本控制

```bash
# 配置文件也应该纳入版本控制
git add config/*.conf
git commit -m "Update live trading config"
```

### 3. 配置验证

```bash
# 运行前验证配置
./build/engine live --validate-config  # 未来功能

# 或者先在回测模式测试
./build/engine backtest config/live.conf
```

## 常见问题

### Q1: 为什么回测和实盘用不同的配置文件？

**A**: 灵活性和安全性：
- 回测可能需要测试多种策略组合
- 实盘需要更谨慎的配置
- 可以同时维护多个配置集

但你也可以共用同一个配置文件：
```bash
./build/engine backtest config/shared.conf
./build/engine live config/shared.conf
```

### Q2: 能否在运行时动态修改配置？

**A**: 当前不支持，需要重启程序。未来可以添加：
- 热重载功能
- 配置文件监听
- 信号触发重载

### Q3: 配置文件支持哪些格式？

**A**: 当前只支持简单的 CSV 格式：
```
股票代码,策略名称
```

未来可以扩展支持：
- JSON 格式（更灵活）
- YAML 格式（更易读）
- TOML 格式（更结构化）

## 总结

| 改进点 | Before | After |
|--------|--------|-------|
| **策略配置** | 硬编码 | 配置文件 |
| **灵活性** | 低 | 高 |
| **可维护性** | 差 | 好 |
| **代码一致性** | 不一致 | 一致 |
| **扩展性** | 困难 | 容易 |

**核心收益**：
- ✅ 消除硬编码
- ✅ 统一配置管理
- ✅ 提高开发效率
- ✅ 降低维护成本

**升级建议**：立即迁移到配置驱动模式！
