# LimitUpBreakSellStrategy - 涨停开板卖出策略

## 概述

对持有多仓的涨停板股票进行封单流出监控。当检测到封单快速流出（开板信号）时，发出卖出信号，降低卖出后回封的概率。

策略 ID: `7`
策略类型名: `LimitUpBreakSellStrategy`
实现文件: `src/strategy/LimitUpBreakSellStrategy.h`

## 状态机

```
INACTIVE ──[time >= start_time AND lastpx == maxpx]──> MONITORING
MONITORING ──[flow_condition 触发]──> TRIGGERED (terminal)
MONITORING ──[lastpx < maxpx (backup)]──> TRIGGERED (terminal)
```

- **INACTIVE**: start_time 前 / 未涨停 / 价格未初始化
- **MONITORING**: 确认涨停后，逐笔追踪封单流出
- **TRIGGERED**: 卖出信号已发，终态

## 可配置参数

通过 `add_strategy` 的 `params` JSON 传入，所有参数均可选：

| 参数 | JSON key | 类型 | 默认值 | 说明 |
|------|----------|------|--------|------|
| 流出阈值 | `threshold` | uint32 | 60 | 窗口内流出量 > 封单量 × N% 时触发 |
| 窗口长度 | `window_ms` | int32 | 300 | 滑动窗口长度 (毫秒) |
| 封单量下限 | `min_bid` | uint64 | 1000 | 涨停价买委托量 ≤ 此值直接触发 |
| 监控开始时间 | `start_time` | string/int | "13:30" | 支持 "HH:MM" 或 MDTime 整数 |

### ZMQ 添加示例

```json
{
  "payload": {
    "action": "add_strategy",
    "symbol": "600550",
    "strategy": "LimitUpBreakSellStrategy",
    "params": {
      "threshold": 50,
      "window_ms": 200,
      "min_bid": 500,
      "start_time": "13:00"
    }
  }
}
```

不传 `params` 则全部使用默认值。

### 回测配置

`config/backtest.conf`:
```
600550,LimitUpBreakSellStrategy
```

## 触发条件

每次 `on_order` / `on_transaction` 处理涨停价相关事件后，检查：

```
current_bid = OrderBook 涨停价买委托量
flow_sum = 窗口内 (撤单量 + 成交量) 之和

触发 = current_bid ≤ min_bid
     OR flow_sum × 100 > current_bid × threshold
```

## 核心数据结构

### 涨停价买单追踪

```cpp
std::unordered_map<uint64_t, uint64_t> limit_up_bid_orders_;  // order_id → volume
```

- 买单 at 涨停价 → 记录 `order_id → volume`
- 撤单/成交 → 查 order_id 匹配，命中则加入 flow_window
- order_id: SZ 用 `orderindex`，SH 用 `orderno`（与 FastOrderBook 一致）

### 滑动窗口

```cpp
struct FlowEvent { int32_t time; uint64_t volume; };
std::deque<FlowEvent> flow_window_;
```

每次检查时，清理超出 `window_ms` 的旧条目，累加剩余条目的 volume 得到 `flow_sum`。

## OrderBook 同步

### 问题

策略在 INACTIVE 状态下不追踪订单。进入 MONITORING 时 `limit_up_bid_orders_` 为空，导致：
- 已有买单的撤单/成交无法匹配，flow_sum 偏小
- 40%/60% 阈值触发延迟

动态添加（ZMQ `add_strategy`）也有同样问题——添加前的订单未被追踪。

### 方案

`FastOrderBook` 新增 `for_each_bid_order_at_price` 模板接口：

```cpp
template<typename Fn>
void for_each_bid_order_at_price(uint32_t price, Fn&& fn) const;
// fn(uint64_t seq, uint32_t volume)
```

- 遍历指定价格档位的买单链表（Level.bid_head_idx → next_idx → ...）
- 零分配、可内联、const 方法，不影响热路径
- 仅在策略初始化时调用一次

策略在进入 MONITORING 后，第一次收到 `on_order` 或 `on_transaction` 时调用 `sync_from_book()`，从 OrderBook 快照初始化 `limit_up_bid_orders_`。

## 卖出信号

信号通过 `place_order(TradeSignal)` 发出，经 ZMQ 路由到 Dealer：

```json
{
  "payload": {
    "action": "place_order",
    "symbol": "600550",
    "price": 12.50,
    "side": "sell",
    "strategy": "LimitUpBreakSellStrategy"
  }
}
```

- `side`: "sell"（通过 `strategy_context.cpp` 根据 `TradeSignal::Side` 转换）
- `quantity`: 不传，Dealer 端根据持仓自行决定

## 修改的文件清单

| 文件 | 改动 |
|------|------|
| `src/strategy/LimitUpBreakSellStrategy.h` | 新建 - 策略主体 |
| `src/FastOrderBook.h` | 新增 `TICK_SIZE` 常量 + `for_each_bid_order_at_price` 模板 |
| `src/FastOrderBook.cpp` | `TICK_SIZE` 改为引用类常量 |
| `include/strategy_ids.h` | 添加 `LIMIT_UP_BREAK_SELL = 7` |
| `src/main.cpp` | include + 工厂注册（透传 params）|
| `include/zmq_client.h` | `send_place_order` 增加 side 参数；`parse_strategy_params` 支持 JSON 透传 |
| `src/strategy_context.cpp` | `LiveContext::place_order` 传递 side |

## 测试脚本

| 脚本 | 用途 |
|------|------|
| `test/test_limit_up_break.py` | 单只股票策略模拟测试 |
| `test/test_limit_up_break_multi.py` | 多股票、多封板段分析 |
| `test/test_limit_up_break_auto.py` | 自动化端到端测试（ClickHouse 选股 → 下载 → 分析） |

### 自动化测试用法

```bash
# 分析 20260310 所有涨停开板股票
python test/test_limit_up_break_auto.py 20260310

# 多日期，最短封板120秒，最多10只
python test/test_limit_up_break_auto.py 20260309 20260310 --min-seal 120 --max-stocks 10
```
