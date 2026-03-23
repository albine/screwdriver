# 下单确认机制设计方案

> 状态：草案 | 日期：2026-03-23

## 1. 问题描述

当前 engine 侧 `place_order` 是 fire-and-forget：

```
DabanStrategy.emit_buy_signal()
  → Strategy.place_order()
    → LiveContext.place_order()
      → ZmqClient.send_place_order()   // zmq_send() 后就结束，不等回复
```

策略发出 BUY 信号后立即进入 `POSITIONED` 状态，开始监控炸板。但实际上：
- ZMQ 发送可能失败（网络断开、dealer 未启动）
- Dealer 可能拒单（资金不足、风控拦截、重复下单）
- Dealer 可能下单成功但未成交（排队中）

**后果**：策略以为已持仓，触发炸板后发出 SELL 信号，但实际无持仓 → 空卖 / dealer 端报错。

## 2. 当前架构概览

```
┌──────────────────────┐          ZMQ (DEALER-ROUTER)          ┌──────────────┐
│  Engine (C++)        │  ──── place_order (fire & forget) ──→ │  Dealer (Py)  │
│                      │                                       │              │
│  DabanStrategy       │  ← (当前无任何回复通道用于下单确认) ←  │              │
│    SLEEPING          │                                       │              │
│    ACTIVE            │                                       │              │
│    POSITIONED ← 问题 │                                       │              │
│    DONE              │                                       │              │
└──────────────────────┘                                       └──────────────┘
```

关键约束：
- Engine 使用 lock-free 队列 + shard worker 架构，策略回调在 worker 线程执行
- ZMQ recv 在独立线程（`recv_loop`），不能直接调用策略方法
- 已有 `ControlMessage` 机制通过队列向 worker 投递消息（用于 enable/disable）
- 打板策略时间敏感，不能阻塞等待确认

## 3. 设计方案

### 3.1 总体思路

**异步确认，不阻塞行情处理。**

```
┌──────────────────────┐          ZMQ                          ┌──────────────┐
│  Engine              │  ── place_order ──────────────────→   │  Dealer       │
│                      │                                       │              │
│                      │  ← order_ack / order_reject ←──────   │              │
│                      │                                       │              │
│  ZMQ recv_loop       │                                       │              │
│    ↓ 解析回复                                                │              │
│  ControlMessage::ORDER_ACK / ORDER_REJECT                    │              │
│    ↓ 投入 shard 队列                                         │              │
│  Worker 线程                                                 │              │
│    ↓ 调用 strategy.on_order_response()                       │              │
│  策略状态转换                                                │              │
└──────────────────────┘                                       └──────────────┘
```

复用已有的 `ControlMessage` → shard 队列 → worker 回调链路，零额外线程。

### 3.2 新增状态：PENDING_BUY / PENDING_SELL

DabanStrategy 状态机改为：

```
SLEEPING → ACTIVE → PENDING_BUY → POSITIONED → PENDING_SELL → DONE
                        ↓ (reject/timeout)       ↓ (reject/timeout)
                     ACTIVE (可重试)          POSITIONED (可重试)
```

| 状态 | 含义 | 行为 |
|------|------|------|
| SLEEPING | 等待激活条件 | 正常处理行情 |
| ACTIVE | 有>=8%成交，累计涨停委托 | 检测触发条件 |
| **PENDING_BUY** | BUY 信号已发送，等待确认 | 不再发新 BUY；不启动炸板检测；超时可回退 |
| POSITIONED | 已确认持仓 | 炸板检测 |
| **PENDING_SELL** | SELL 信号已发送，等待确认 | 不再发新 SELL；超时可重试 |
| DONE | 终态 | 忽略所有行情 |

### 3.3 ZMQ 协议扩展

#### 3.3.1 新增 Action 类型

```cpp
// zmq_protocol.h
namespace Action {
    // ... 已有 ...
    constexpr const char* ORDER_ACK    = "order_ack";     // 下单确认
    constexpr const char* ORDER_REJECT = "order_reject";  // 下单拒绝
}
```

#### 3.3.2 Dealer → Engine 回复格式

```json
{
  "req_id": "order_ack_1",
  "payload": {
    "action": "order_ack",
    "orig_req_id": "order_42",
    "symbol": "600550",
    "side": "buy",
    "strategy": "DabanStrategy",
    "status": "accepted",
    "message": "排队中"
  }
}
```

```json
{
  "req_id": "order_reject_1",
  "payload": {
    "action": "order_reject",
    "orig_req_id": "order_42",
    "symbol": "600550",
    "side": "buy",
    "strategy": "DabanStrategy",
    "reason": "insufficient_funds",
    "message": "可用资金不足"
  }
}
```

### 3.4 ControlMessage 扩展

```cpp
struct ControlMessage {
    enum class Type : uint8_t {
        ENABLE,
        DISABLE,
        ORDER_ACK,       // 新增
        ORDER_REJECT     // 新增
    };
    Type type;
    uint32_t unique_id;
    char symbol[48];
    uint32_t param = 0;
    char side;               // 新增：'B' = buy, 'S' = sell
    char reason[64];         // 新增：拒绝原因（ORDER_REJECT 时使用）

    // 新增工厂方法
    static ControlMessage order_ack(const std::string& sym, const std::string& strat_name, char side);
    static ControlMessage order_reject(const std::string& sym, const std::string& strat_name, char side, const std::string& reason);
};
```

### 3.5 回调链路

#### ZmqClient 收到回复后：

```cpp
// zmq_client.h - handle_command() 新增分支
else if (action == zmq_ht_proto::Action::ORDER_ACK) {
    std::string symbol = symbol_utils::normalize_symbol(payload.value("symbol", ""));
    std::string strategy = payload.value("strategy", "");
    std::string side = payload.value("side", "");
    char side_char = (side == "sell") ? 'S' : 'B';

    if (engine_) {
        engine_->send_control_message(
            ControlMessage::order_ack(symbol, strategy, side_char));
    }
    LOG_BIZ(BIZ_ORDR, "ORDER_ACK | {} | {} | side={}", symbol, strategy, side);
}
else if (action == zmq_ht_proto::Action::ORDER_REJECT) {
    // 类似，构造 ORDER_REJECT ControlMessage
}
```

#### Strategy 基类新增回调：

```cpp
// strategy_base.h
class Strategy {
public:
    // ... 已有 ...
    virtual void on_order_ack(char side) {}       // 下单确认
    virtual void on_order_reject(char side, const char* reason) {}  // 下单拒绝
};
```

#### StrategyEngine worker 线程分发：

```cpp
// strategy_engine.h - 处理 ControlMessage 的地方
if (ctrl.type == ControlMessage::Type::ORDER_ACK) {
    strat->on_order_ack(ctrl.side);
} else if (ctrl.type == ControlMessage::Type::ORDER_REJECT) {
    strat->on_order_reject(ctrl.side, ctrl.reason);
}
```

### 3.6 DabanStrategy 状态转换实现

```cpp
// === BUY 发送 ===
void emit_buy_signal(...) {
    // 改：不直接进 POSITIONED，而是进 PENDING_BUY
    state_ = State::PENDING_BUY;
    pending_time_ = current_time;   // 记录发送时间，用于超时
    place_order(signal);
}

// === 确认回调 ===
void on_order_ack(char side) override {
    if (side == 'B' && state_ == State::PENDING_BUY) {
        state_ = State::POSITIONED;
        LOG_M_INFO("DabanStrategy BUY CONFIRMED: {}", symbol);
    }
    else if (side == 'S' && state_ == State::PENDING_SELL) {
        state_ = State::DONE;
        LOG_M_INFO("DabanStrategy SELL CONFIRMED: {}", symbol);
    }
}

void on_order_reject(char side, const char* reason) override {
    if (side == 'B' && state_ == State::PENDING_BUY) {
        state_ = State::ACTIVE;    // 回退，允许重新触发
        signal_triggered_ = false;
        LOG_M_WARNING("DabanStrategy BUY REJECTED: {} | reason={}", symbol, reason);
    }
    else if (side == 'S' && state_ == State::PENDING_SELL) {
        state_ = State::POSITIONED;  // 回退，允许重试卖出
        LOG_M_WARNING("DabanStrategy SELL REJECTED: {} | reason={}", symbol, reason);
    }
}

// === 超时处理（在 on_tick / on_order 中检查）===
void check_pending_timeout(int32_t current_time) {
    if (state_ == State::PENDING_BUY &&
        time_util::calculate_time_diff_ms(pending_time_, current_time) > PENDING_TIMEOUT_MS) {
        // 超时未确认，回退
        state_ = State::ACTIVE;
        signal_triggered_ = false;
        LOG_M_WARNING("DabanStrategy BUY TIMEOUT: {} | pending_time={}", symbol, pending_time_);
    }
    if (state_ == State::PENDING_SELL &&
        time_util::calculate_time_diff_ms(pending_time_, current_time) > PENDING_TIMEOUT_MS) {
        // SELL 超时，回退 POSITIONED，可重试
        state_ = State::POSITIONED;
        LOG_M_WARNING("DabanStrategy SELL TIMEOUT: {} | pending_time={}", symbol, pending_time_);
    }
}
```

超时参数建议：`PENDING_TIMEOUT_MS = 2000`（2 秒）。打板排队通常几秒内有回复，2 秒足够覆盖网络延迟。

### 3.7 PENDING 期间的行为

| 回调 | PENDING_BUY 期间 | PENDING_SELL 期间 |
|------|------------------|-------------------|
| on_tick | 检查超时；**不触发**保底买入/炸板检测 | 检查超时；**不触发**新的卖出 |
| on_order | 检查超时；继续累计涨停委托（不浪费数据） | 检查超时；继续喂数据给 flow_detector |
| on_transaction | 检查超时；继续记录成交 | 检查超时；继续喂数据给 flow_detector |

PENDING 期间**继续处理行情数据**，只是不产生新的交易信号。这样一旦超时回退，策略数据窗口是连续的。

## 4. 其他策略的影响

| 策略 | 是否需要改 | 原因 |
|------|-----------|------|
| DabanStrategy | 是 | 核心需求 |
| LimitUpBreakSellStrategy | 是（推荐） | 同样有 SELL 信号，应加确认 |
| BreakoutPriceVolumeStrategy | 是（推荐） | 同上 |
| 其他策略 | 可选 | 基类默认 `on_order_ack/reject` 为空，不改不影响 |

## 5. Dealer 端改动（Python）

Dealer 端在处理 `place_order` 消息后，需要回复：

```python
# 收到 place_order 后
try:
    result = execute_order(symbol, price, side, strategy)
    reply = {
        "req_id": f"order_ack_{seq}",
        "payload": {
            "action": "order_ack",
            "orig_req_id": msg["req_id"],
            "symbol": symbol,
            "side": side,
            "strategy": strategy,
            "status": "accepted"
        }
    }
except Exception as e:
    reply = {
        "req_id": f"order_reject_{seq}",
        "payload": {
            "action": "order_reject",
            "orig_req_id": msg["req_id"],
            "symbol": symbol,
            "side": side,
            "strategy": strategy,
            "reason": str(e)
        }
    }
router_socket.send_multipart([identity, json.dumps(reply).encode()])
```

## 6. 第一层：Dealer 端防御（无论是否实现本方案都应做）

在 dealer 端加防御性检查，作为独立于本方案的兜底措施：

```python
def handle_place_order(msg):
    symbol = msg["symbol"]
    side = msg["side"]

    if side == "sell" and not has_position(symbol):
        log.warning(f"SELL ignored: no position for {symbol}")
        send_reject(msg, reason="no_position")
        return

    if side == "buy" and has_position(symbol):
        log.warning(f"BUY ignored: already has position for {symbol}")
        send_reject(msg, reason="already_positioned")
        return

    # 正常下单...
```

## 7. 改动清单

| 文件 | 改动内容 | 复杂度 |
|------|---------|--------|
| `include/zmq_protocol.h` | 新增 `ORDER_ACK`, `ORDER_REJECT` Action | 低 |
| `include/strategy_engine.h` | `ControlMessage` 新增类型和字段；worker 分发逻辑 | 中 |
| `include/strategy_base.h` | 新增 `on_order_ack()`, `on_order_reject()` 虚函数 | 低 |
| `include/zmq_client.h` | `handle_command` 新增 ack/reject 处理分支 | 低 |
| `src/strategy/DabanStrategy.h` | 新增 PENDING 状态 + 回调实现 + 超时检查 | 中 |
| `src/strategy/LimitUpBreakSellStrategy.h` | 类似 PENDING_SELL 逻辑 | 中 |
| Dealer 端 (Python) | 回复 ack/reject + 防御性检查 | 低 |

## 8. 测试策略

1. **回测模式不受影响**：`BacktestContext.place_order` 只写日志，不走 ZMQ，不会收到 ack/reject。PENDING 状态需要特殊处理——回测模式下 `place_order` 后直接调用 `on_order_ack` 模拟确认。
2. **单元测试**：模拟 ack/reject/timeout 三种场景，验证状态转换。
3. **集成测试**：实盘模式下观察 BIZ 日志中的 `ORDER_ACK` / `ORDER_REJECT` 记录。

---

*本文档用于与团队讨论，确认方向后再进入实施阶段。*
