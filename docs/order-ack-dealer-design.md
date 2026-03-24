# 打板策略 Dealer 侧执行设计

> 状态：草案 | 日期：2026-03-23
>
> 相关总览：`order-ack-design.md`
> 相关业务规则：`../business/打板策略业务规则_codex修改.txt`

## 1. 目标

dealer 是执行真相的唯一维护方。

dealer 负责：

- 接收 engine 发来的 `BUY_SIGNAL` / `SELL_SIGNAL`
- 决定何时下单、何时忽略、何时排队、何时报错
- 维护订单状态、成交状态、持仓状态
- 在异常和收盘场景下接管后续动作

engine 不参与这些决策。

## 2. 核心原则

### 2.1 dealer authoritative

以下事实只能由 dealer 判定：

- 是否已向柜台/券商发单
- 是否已报
- 是否已成
- 是否仍有未完成挂单
- 当前是否有持仓
- 是否需要人工处理

### 2.2 `SELL_SIGNAL` 不能简单丢弃

这是本设计最关键的要求。

如果 BUY_SIGNAL 发出后，BUY 还未最终成交，期间 engine 又发来了 SELL_SIGNAL，dealer 不能仅因为“当前还没仓位”就把 SELL_SIGNAL 直接忽略。

dealer 必须把这个 SELL_SIGNAL 记为一个终止性的退出意图，后续至少要做到二选一：

- BUY 最终成交后立即卖出
- BUY 最终失败后结束本轮周期

否则会出现：

1. engine 发 BUY_SIGNAL
2. engine 之后发 SELL_SIGNAL
3. dealer 因为当时没仓位忽略 SELL
4. BUY 稍后成交
5. 仓位留在手里，无退出动作

### 2.3 多执行通道问题在 dealer 内部解决

如果存在多个实际执行通道、多个券商连接或多个 dealer worker：

- engine 只面对一个逻辑上的 dealer
- dealer 内部自己做去重、路由和汇总
- engine 不感知“哪个 dealer 回了什么”

## 3. 输入契约

dealer 从 engine 接收的只有信号，不是完整执行状态机。

### 3.1 BUY_SIGNAL

语义：

- “engine 认为现在应该开始做多这只股票”

它不保证：

- 一定能买到
- 一定已经有仓位
- 之后不会很快出现退出信号

### 3.2 SELL_SIGNAL

语义：

- “engine 认为本轮应该退出，后续不再产生新的交易信号”

它是终止性的退出意图。

即使此时 BUY 尚未成交，dealer 也必须记住这个意图。

## 4. dealer 侧状态机

dealer 侧维护执行状态，而不是 engine。

推荐状态机：

```
IDLE
  └─ BUY_SIGNAL ─→ BUY_WORKING

BUY_WORKING
  ├─ BUY 已成 ─→ HOLDING
  ├─ BUY 废单/错误/超时 ─→ DONE_NO_FILL
  └─ SELL_SIGNAL ─→ BUY_WORKING_EXIT_PENDING

BUY_WORKING_EXIT_PENDING
  ├─ BUY 已成 ─→ SELL_WORKING
  ├─ BUY 废单/错误/超时 ─→ DONE_NO_FILL
  └─ 收盘 ─→ ERROR

HOLDING
  ├─ SELL_SIGNAL ─→ SELL_WORKING
  └─ 收盘 ─→ EOD_HANDOFF

SELL_WORKING
  ├─ SELL 已成 ─→ DONE
  ├─ SELL 废单/错误/超时 ─→ ERROR
  └─ 收盘 ─→ ERROR
```

状态说明：

| 状态 | 含义 |
|------|------|
| `IDLE` | 本轮周期未开始，无挂单、无仓位 |
| `BUY_WORKING` | BUY 已发出，等待结果 |
| `BUY_WORKING_EXIT_PENDING` | BUY 仍在路上，但已收到 SELL_SIGNAL，后续必须退出或结束 |
| `HOLDING` | 已确认有仓位，等待卖出信号 |
| `SELL_WORKING` | SELL 已发出，等待卖出完成 |
| `DONE_NO_FILL` | 本轮结束，但未形成持仓 |
| `DONE` | 本轮正常结束，已卖出完成 |
| `EOD_HANDOFF` | 收盘仍有仓位，转给日终流程 |
| `ERROR` | dealer 无法自动完成，需要人工介入 |

## 5. 信号处理规则

### 5.1 收到 `BUY_SIGNAL`

| 当前状态 | 处理 |
|---------|------|
| `IDLE` | 创建新周期，发 BUY 单，进入 `BUY_WORKING` |
| `BUY_WORKING` / `BUY_WORKING_EXIT_PENDING` | 视为重复 BUY，忽略并记日志 |
| `HOLDING` / `SELL_WORKING` | 视为重复 BUY，忽略并记日志 |
| `DONE` / `DONE_NO_FILL` / `EOD_HANDOFF` / `ERROR` | 不应再收到；默认拒绝并记日志 |

### 5.2 收到 `SELL_SIGNAL`

| 当前状态 | 处理 |
|---------|------|
| `IDLE` | 忽略并记日志，本轮无可退出对象 |
| `BUY_WORKING` | 不丢弃信号，置 `exit_on_fill=true`，进入 `BUY_WORKING_EXIT_PENDING` |
| `BUY_WORKING_EXIT_PENDING` | 重复 SELL，忽略 |
| `HOLDING` | 发 SELL 单，进入 `SELL_WORKING` |
| `SELL_WORKING` | 重复 SELL，忽略 |
| `DONE` / `DONE_NO_FILL` / `EOD_HANDOFF` / `ERROR` | 默认忽略并记日志 |

## 6. 订单回报规则

### 6.1 BUY 回报

#### BUY 已报

当前版本的业务口径是：

- BUY 已报可以被视为“后续退出流程已允许启动”

但 dealer 内部仍建议区分：

- `已报但未成`
- `已成`

原因是：

- 你可以在业务上接受“已报就允许出现 SELL 意图”
- 但你未必能在执行上立即发出真实 SELL 单

默认建议：

- BUY 已报：保持 `BUY_WORKING` 或 `BUY_WORKING_EXIT_PENDING`
- BUY 已成：若无退出意图，进 `HOLDING`；若已有退出意图，立即进 `SELL_WORKING`

如果某个柜台规则允许“已报阶段即可安全处理后续退出”，那属于 dealer 内部优化，不影响 engine 契约。

#### BUY 废单 / 错误 / 超时

| 当前状态 | 处理 |
|---------|------|
| `BUY_WORKING` | 进入 `DONE_NO_FILL` |
| `BUY_WORKING_EXIT_PENDING` | 进入 `DONE_NO_FILL` |

注意：在 engine/dealer 拆分模式下，BUY 失败后默认不要求 engine 重试。

### 6.2 SELL 回报

#### SELL 已成

- `SELL_WORKING -> DONE`

#### SELL 废单 / 错误 / 超时

- `SELL_WORKING -> ERROR`

这是 dealer 侧必须兜底的问题，不能再回推给 engine。

## 7. 收盘规则

收盘后 engine 会结束自己的策略周期，但 dealer 仍要对真实执行状态负责。

推荐规则：

| 当前状态 | 收盘动作 |
|---------|---------|
| `IDLE` | 无动作 |
| `BUY_WORKING` | 根据柜台能力取消或继续跟踪；若结果不明，进 `ERROR` |
| `BUY_WORKING_EXIT_PENDING` | 优先取消 BUY；若状态不明，进 `ERROR` |
| `HOLDING` | 进入 `EOD_HANDOFF`，交给日终持仓流程 |
| `SELL_WORKING` | 若未成交且风险未解除，进 `ERROR` |
| `DONE` / `DONE_NO_FILL` / `EOD_HANDOFF` / `ERROR` | 保持不变 |

## 8. 人工操作

dealer 至少要提供以下人工能力：

- 查询某个 symbol 的执行状态
- 查询未完成挂单
- 查询真实持仓
- 强制取消挂单
- 强制卖出
- 把 `ERROR` 周期标记为已处理

这些能力属于 dealer 运维面，不应依赖 engine 状态回滚。

## 9. 对 engine 的可选回传

dealer 可以回传遥测，但默认不作为策略正确性的必需链路。

推荐只做以下可选信息：

- 执行日志
- 告警日志
- 周期状态快照
- 人工处理事件

如果未来要回传给 engine，优先回传粗粒度状态，例如：

- `cycle_state=idle/buy_working/holding/sell_working/error`
- `has_position=true/false`

不建议第一阶段把所有订单回报逐笔回灌给 engine。

## 10. 与当前实现的关系

当前 `TradeSignal` 和 `place_order()` 可以继续复用：

- engine 仍然调用 `place_order(signal)`
- dealer 仍然接收 `symbol/side/price/strategy`，其中 `strategy` 是策略类型名（例如 `DabanStrategy`）

变化只在语义边界：

- engine 发送的是交易意图
- dealer 维护的才是执行状态
