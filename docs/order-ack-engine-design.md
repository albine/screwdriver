# 打板策略 Engine 侧设计

> 状态：草案 | 日期：2026-03-23
>
> 相关总览：`order-ack-design.md`
> 相关业务规则：`../business/打板策略业务规则_codex修改.txt`

## 1. 目标

engine 只负责两件事：

- 盯行情，判断何时出现买点和卖点
- 向 dealer 发出 `BUY_SIGNAL` / `SELL_SIGNAL`

engine 不再负责维护订单状态、成交状态或持仓真相。

## 2. 职责范围

### 2.1 engine 负责

- 维护策略级状态
- 执行 `SLEEPING ↔ ACTIVE` 切换
- 在满足条件时发出一次 `BUY_SIGNAL`
- 发出买入信号后继续盯炸板条件
- 在满足条件时发出一次 `SELL_SIGNAL`
- 收盘后结束本轮策略生命周期

### 2.2 engine 不负责

- 判断“是否已报”
- 判断“是否已成”
- 判断“当前账户是否有仓位”
- 判断“是否需要撤单/补单/重试”
- 聚合多个执行通道的回报
- 处理人工复位、异常订单、清错

这些都归 dealer。

## 3. 状态机

engine 侧只保留策略状态，不再使用 `POSITIONED/PENDING_*` 这类执行状态名。

```
SLEEPING ←→ ACTIVE ──→ EXIT_ARMED ──→ DONE

收盘（14:57）：
  SLEEPING / ACTIVE / EXIT_ARMED → DONE
```

状态说明：

| 状态 | 含义 |
|------|------|
| SLEEPING | 尚未激活，不允许触发 BUY |
| ACTIVE | 已激活，允许检测并发出 BUY_SIGNAL |
| EXIT_ARMED | BUY_SIGNAL 已发出，开始盯退出条件；这不是持仓状态 |
| DONE | 本轮策略结束，不再处理任何信号触发 |

### 3.1 为什么用 `EXIT_ARMED`

`EXIT_ARMED` 只表示：

- engine 已经完成本轮买入信号判断
- 从现在开始只关心退出条件

它不表示：

- dealer 一定已报
- dealer 一定已成
- 当前一定有持仓

这样可以避免把执行状态误塞进策略状态。

## 4. 规则细化

### 4.1 通用规则

- `09:30:00.000` 之前不允许发 BUY 或 SELL
- `14:57:00.000` 及以后统一进入 `DONE`（连续竞价结束，收盘集合竞价开始）
- `DONE` 状态忽略所有行情回调

### 4.2 `SLEEPING ↔ ACTIVE`

两条路径并行：

| 路径 | 触发源 | SLEEPING → ACTIVE | ACTIVE → SLEEPING |
|------|--------|-------------------|-------------------|
| 逐笔 | `on_order/on_transaction` | 10ms 窗口内有成交价 > 8% | 10ms 窗口内所有成交价 < 7% |
| 快照保底 | `on_tick` | 卖一价涨幅 > 8%，或卖一价为 0 且买一价 == 涨停价 | 卖一价涨幅 < 7%（卖一价为 0 时不触发休眠） |

其中涨幅 = 卖一价 / 昨收价 - 1。卖一价为 0 表示无卖单（可能已涨停封死），此时用买一价 == 涨停价作为激活条件。

约束：

- `SLEEPING` 状态禁止直接触发 BUY
- `on_tick` 保底必须先把状态切到 `ACTIVE`，再检查保底买入条件
- 一旦进入 `EXIT_ARMED`，不再参与 `SLEEPING ↔ ACTIVE`

> **代码修正项**：当前代码（`DabanStrategy.h:181`）允许从 `SLEEPING` 直接调用 `emit_buy_signal_from_tick()`，绕过了 `SLEEPING → ACTIVE` 的必经路径，违反规则 2.2。实现时必须修正为：先检查保底切换条件将状态切到 `ACTIVE`，再检查触发条件。

### 4.3 `ACTIVE -> EXIT_ARMED`

触发条件：

- `on_order/on_transaction` 的现有买入触发逻辑命中
- 或 `on_tick` 的保底买入逻辑命中

动作：

1. 构造 `TradeSignal{side=BUY}`
2. 调用 `place_order(signal)`
3. 状态切到 `EXIT_ARMED`

注意：

- BUY_SIGNAL 发出后，engine 不再发第二个 BUY_SIGNAL
- engine 不等待 dealer ack 才进入 `EXIT_ARMED`

### 4.4 `EXIT_ARMED -> DONE`

触发条件：

- `on_order/on_transaction` 的炸板卖出逻辑命中
- 或 `on_tick` 的保底卖出逻辑命中
- 或收盘

动作：

1. 如果是卖点触发，构造 `TradeSignal{side=SELL}` 并调用 `place_order(signal)`
2. 状态切到 `DONE`

这里的 `DONE` 表示：

- engine 本轮信号已经发完
- 后续是否真的卖出、是否需要人工接管，交给 dealer

## 5. 各回调职责

### 5.1 `on_tick`

负责：

- 价格初始化
- 9:30 前过滤
- `SLEEPING ↔ ACTIVE` 保底切换
- `ACTIVE` 状态下的保底 BUY 触发
- `EXIT_ARMED` 状态下的保底 SELL 触发
- 收盘结束

不负责：

- 订单状态更新
- 异常复位

### 5.2 `on_order/on_transaction`

负责：

- 逐笔驱动的 `SLEEPING ↔ ACTIVE`
- `ACTIVE` 状态下的主买触发逻辑
- `EXIT_ARMED` 状态下的炸板检测
- 收盘结束

## 6. 输出接口

engine 对 dealer 的唯一强依赖输出仍然是 `TradeSignal`。

当前 live 路径继续沿用现有 payload 语义：

| 字段 | 含义 |
|------|------|
| `symbol` | 股票代码 |
| `side` | `BUY` 或 `SELL` |
| `price` | 触发时参考价 |
| `strategy` | 策略类型名（例如 `DabanStrategy`） |

`TradeSignal` 结构体本身仍可继续保留 `trigger_time / strategy_name / strategy_type_id` 等字段，但本阶段 live 下发给 dealer 的 payload 不扩充，`place_order()` 这个名字也暂时保留；它在职责语义上更接近“向 dealer 发执行意图”，而不是“engine 自己开始维护订单状态”。

## 7. 对代码实现的约束

本阶段 engine 不应引入以下机制：

- `on_order_ack()`
- `on_order_reject()`
- `on_order_filled()`
- `PENDING_BUY / POSITIONED / PENDING_SELL / ERROR_*`
- 基于 dealer 回报的人工复位状态机

如果这些逻辑出现，说明执行状态又被带回 engine 了。

## 8. 已知取舍

这种拆分意味着：

- BUY_SIGNAL 发出后，如果 dealer 最终拒单，engine 不会退回 `ACTIVE` 再次重试
- SELL_SIGNAL 发出后，engine 直接结束本轮，不关心 dealer 是否真正卖出

这是有意为之。执行层的一致性必须由 dealer 负责，否则 engine 会重新背上订单状态机。

如果未来确认业务需要“买单被拒后策略继续重试”，必须单独设计一条粗粒度 dealer -> engine 状态回传链路。
