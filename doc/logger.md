# HFT日志模块使用指南

## 概述

本日志模块基于 [Quill](https://github.com/odygrd/quill) 异步日志库，针对高频交易场景优化：

- **超低延迟**：热路径 ~10ns（RDTSC时间戳）
- **异步架构**：Lock-free队列，后台线程处理I/O
- **纳秒精度**：时间戳精确到纳秒
- **双日志分离**：程序日志与业务日志独立文件

---

## 快速开始

### 1. 包含头文件

```cpp
#include "logger.h"
```

### 2. 初始化日志

```cpp
int main() {
    // 初始化程序日志 (输出到 logs/trading.log)
    auto* logger = hft::logger::init();

    // 初始化业务日志 (输出到 logs/business.log)
    auto* biz_logger = hft::logger::init_biz();

    // ... 使用日志 ...

    // 程序退出前关闭日志
    hft::logger::shutdown();
    return 0;
}
```

### 3. 自定义配置

```cpp
// 程序日志配置
hft::logger::LogConfig config;
config.log_dir = "logs";
config.log_file = "trading.log";
config.max_file_size = 100 * 1024 * 1024;  // 100MB
config.max_backup_files = 10;
config.console_output = true;
auto* logger = hft::logger::init(config);

// 业务日志配置
hft::logger::BizLogConfig biz_config;
biz_config.log_dir = "logs";
biz_config.log_file = "business.log";
biz_config.console_output = false;  // 业务日志默认不输出控制台
auto* biz_logger = hft::logger::init_biz(biz_config);
```

---

## 程序日志

### 日志等级

| 等级 | 标识 | 用途 |
|------|------|------|
| DEBUG | `DBUG` | 调试信息（生产环境建议关闭） |
| INFO | `INFO` | 常规信息 |
| WARNING | `WARN` | 警告信息 |
| ERROR | `ERR!` | 错误信息 |

### 基本用法

```cpp
LOG_DEBUG(logger, "Debug message, value={}", 42);
LOG_INFO(logger, "Info message");
LOG_WARNING(logger, "Warning message");
LOG_ERROR(logger, "Error message");
```

### 模块日志

为日志添加模块标识，便于定位问题来源：

```cpp
// 方式1：直接指定模块名
LOG_MODULE_INFO(logger, MOD_ENGINE, "Engine starting...");
LOG_MODULE_ERROR(logger, MOD_GATEWAY, "Connection failed");

// 方式2：文件级模块宏（推荐）
#define LOG_MODULE MOD_ORDERBOOK  // 在文件开头定义

LOG_M_INFO(logger, "Order added, id={}", order_id);    // 自动带[OrderBook]前缀
LOG_M_ERROR(logger, "Invalid price={}", price);
```

### 预定义模块名

| 宏 | 输出 | 用途 |
|------|------|------|
| `MOD_ENGINE` | `[Engine]` | 交易引擎 |
| `MOD_ORDERBOOK` | `[OrderBook]` | 订单簿 |
| `MOD_STRATEGY` | `[Strategy]` | 策略 |
| `MOD_GATEWAY` | `[Gateway]` | 网关 |
| `MOD_MARKET` | `[Market]` | 行情 |

### 输出格式

```
2026-01-11 09:30:00.123456789 [INFO] [12345] [Engine] Engine starting...
│                             │      │       │        └── 消息内容
│                             │      │       └── 模块名
│                             │      └── 线程ID
│                             └── 日志等级（4字符）
└── 纳秒精度时间戳
```

---

## 业务日志

业务日志用于记录交易相关的关键事件，独立于程序日志，便于监控和审计。

### 业务类型标识

| 宏 | 标识 | 用途 |
|------|------|------|
| `BIZ_SESS` | `[SESS]` | 会话（登录/登出/心跳） |
| `BIZ_ORDR` | `[ORDR]` | 下单 |
| `BIZ_FILL` | `[FILL]` | 成交回报 |
| `BIZ_CNCL` | `[CNCL]` | 撤单 |
| `BIZ_RJCT` | `[RJCT]` | 拒绝 |
| `BIZ_POSN` | `[POSN]` | 持仓变化 |
| `BIZ_ACCT` | `[ACCT]` | 账户/资金 |

### 用法

```cpp
auto* biz = hft::logger::get_biz_logger();

// 会话
LOG_BIZ(biz, BIZ_SESS, "Login success account={} broker={}", "12345", "broker_name");
LOG_BIZ(biz, BIZ_SESS, "Logout account={}", "12345");

// 下单
LOG_BIZ(biz, BIZ_ORDR, "Order id={} symbol={} side={} price={} qty={}",
        order_id, "000001.SZ", "BUY", 10.50, 1000);

// 成交
LOG_BIZ(biz, BIZ_FILL, "Fill id={} filled_qty={} filled_price={} status={}",
        order_id, 500, 10.50, "PARTIAL");

// 撤单
LOG_BIZ(biz, BIZ_CNCL, "Cancel id={} canceled_qty={}", order_id, 500);

// 拒绝
LOG_BIZ(biz, BIZ_RJCT, "Reject id={} reason={}", order_id, "insufficient_margin");

// 持仓
LOG_BIZ(biz, BIZ_POSN, "Position symbol={} qty={} avg_cost={}",
        "000001.SZ", 1000, 10.50);

// 资金
LOG_BIZ(biz, BIZ_ACCT, "Account available={} frozen={}", 1000000.50, 50000.00);
```

### 输出格式

```
2026-01-11 09:30:00.123456789 [SESS] Login success account=12345
2026-01-11 09:30:01.456789012 [ORDR] Order id=1001 symbol=000001.SZ side=BUY price=10.5 qty=1000
2026-01-11 09:30:01.789012345 [FILL] Fill id=1001 filled_qty=500 status=PARTIAL
│                             │      └── 消息内容（key=value格式）
│                             └── 业务类型（4字符）
└── 纳秒精度时间戳
```

---

## 文件结构

```
logs/
├── trading_20260111_093000.log    # 程序日志（带启动时间戳）
└── business_20260111_093000.log   # 业务日志（带启动时间戳）
```

---

## 最佳实践

### 1. 热路径日志原则

```cpp
// 错误：热路径中执行字符串拼接
LOG_DEBUG(logger, "Order: " + order.toString());  // 有额外开销

// 正确：使用延迟格式化
LOG_DEBUG(logger, "Order id={} price={} qty={}", order.id, order.price, order.qty);
```

### 2. 编译期关闭DEBUG

生产环境编译时禁用DEBUG日志，实现零运行时开销：

```cmake
# CMakeLists.txt
target_compile_definitions(your_target PRIVATE
    QUILL_COMPILE_ACTIVE_LOG_LEVEL=QUILL_LOG_LEVEL_INFO
)
```

### 3. 高频事件采样

```cpp
static int tick_count = 0;
if (++tick_count % 10000 == 0) {
    LOG_M_INFO(logger, "Tick stats: count={} avg_latency={}ns", tick_count, avg_latency);
}
```

### 4. 使用英文日志

生产环境建议使用英文，避免编码问题：

```cpp
// 推荐
LOG_BIZ(biz, BIZ_ORDR, "Order id={} symbol={} side=BUY", id, symbol);

// 不推荐（中文可能显示为hex编码）
LOG_BIZ(biz, BIZ_ORDR, "下单 id={}", id);
```

---

## API参考

### 初始化函数

| 函数 | 说明 |
|------|------|
| `hft::logger::init(config)` | 初始化程序日志器 |
| `hft::logger::init_biz(config)` | 初始化业务日志器 |
| `hft::logger::get_logger()` | 获取程序日志器 |
| `hft::logger::get_biz_logger()` | 获取业务日志器 |
| `hft::logger::flush()` | 刷新日志到磁盘 |
| `hft::logger::shutdown()` | 关闭日志系统 |

### 程序日志宏

| 宏 | 说明 |
|------|------|
| `LOG_DEBUG(logger, fmt, ...)` | DEBUG级别日志 |
| `LOG_INFO(logger, fmt, ...)` | INFO级别日志 |
| `LOG_WARNING(logger, fmt, ...)` | WARNING级别日志 |
| `LOG_ERROR(logger, fmt, ...)` | ERROR级别日志 |
| `LOG_MODULE_*(logger, module, fmt, ...)` | 带模块名的日志 |
| `LOG_M_*(logger, fmt, ...)` | 使用预定义LOG_MODULE的日志 |

### 业务日志宏

| 宏 | 说明 |
|------|------|
| `LOG_BIZ(logger, type, fmt, ...)` | 业务日志，type为BIZ_*常量 |

---

## 完整示例

```cpp
#include "logger.h"

#define LOG_MODULE MOD_ENGINE

int main() {
    // 初始化
    auto* logger = hft::logger::init();
    auto* biz = hft::logger::init_biz();

    // 程序日志
    LOG_M_INFO(logger, "Engine starting...");

    // 业务日志 - 登录
    LOG_BIZ(biz, BIZ_SESS, "Login account={}", "12345");

    // 业务日志 - 下单
    LOG_BIZ(biz, BIZ_ORDR, "Order id={} symbol={} price={} qty={}",
            1001, "000001.SZ", 10.50, 1000);

    // 业务日志 - 成交
    LOG_BIZ(biz, BIZ_FILL, "Fill id={} qty={} status=FILLED", 1001, 1000);

    // 程序日志
    LOG_M_INFO(logger, "Engine stopped");

    // 关闭
    hft::logger::shutdown();
    return 0;
}
```

**程序日志输出 (trading.log):**
```
2026-01-11 09:30:00.123456789 [INFO] [12345] [Engine] Engine starting...
2026-01-11 09:30:02.987654321 [INFO] [12345] [Engine] Engine stopped
```

**业务日志输出 (business.log):**
```
2026-01-11 09:30:00.234567890 [SESS] Login account=12345
2026-01-11 09:30:01.345678901 [ORDR] Order id=1001 symbol=000001.SZ price=10.5 qty=1000
2026-01-11 09:30:01.456789012 [FILL] Fill id=1001 qty=1000 status=FILLED
```
