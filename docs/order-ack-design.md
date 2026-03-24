# 下单信号与执行职责拆分总览

> 状态：草案 | 日期：2026-03-23
>
> 相关业务规则：`../business/打板策略业务规则_codex修改.txt`

## 1. 背景

旧版 `order-ack-design.md` 把两类问题混在了一起：

- engine 如何盯行情、何时发 BUY/SELL 信号
- dealer 如何处理报单、成交、持仓、异常和收盘

这会导致职责边界不清。尤其在以下场景下，engine 侧维护执行状态会迅速变复杂：

- BUY 已报但未成时又收到 SELL 信号
- 同一 symbol 可能存在多个执行通道
- 回报乱序、迟到、重复
- engine 并不掌握真实订单和持仓状态

因此，本设计改为明确分层：

- engine 负责策略判断和信号发出
- dealer 负责执行状态和持仓真相

## 2. 分层原则

| 领域 | 归属方 | 说明 |
|------|--------|------|
| 行情观察 | engine | `on_tick/on_order/on_transaction` 驱动 |
| 策略状态 | engine | 只表示“当前是否继续盯买点/卖点” |
| BUY/SELL 信号生成 | engine | 通过 `place_order(signal)` 向 dealer 发信号 |
| 报单、成交、废单、错误 | dealer | 真实执行状态只能由 dealer 判定 |
| 持仓真相 | dealer | engine 不再维护 `POSITIONED/PENDING_*` 这类执行状态 |
| 多通道汇总 | dealer | engine 不处理多 dealer 回报聚合 |
| 人工干预 | dealer | 强平、清错、复位等操作由 dealer 承担 |

## 3. 核心假设

1. 当前版本 engine 不做撤单，也不根据 dealer 回报重试买入。
2. `BUY_SIGNAL` 是一次性策略信号。若 dealer 最终拒单/未成交，是否重试由 dealer 自行决定；engine 不回到 `ACTIVE` 重试。
3. `SELL_SIGNAL` 是终止性的退出意图。即使 BUY 还未成交，dealer 也必须记住这个退出意图，不能简单丢弃。
4. engine 不以 `order_ack/reject/filled` 作为策略正确性的前提。dealer 到 engine 的回传如果需要，默认只用于观测和运维，不作为一阶段必需链路。

第 2 条是本次拆分的主要取舍。如果后续明确需要“BUY 被拒后 engine 重新回到 ACTIVE 再次触发”，那就要单独设计一个更粗粒度的 dealer -> engine 反馈协议，而不是把完整执行状态机搬回 engine。

## 4. 文档拆分

- [Engine 侧设计](./order-ack-engine-design.md)
- [Dealer 侧设计](./order-ack-dealer-design.md)

阅读顺序建议：

1. 先读 engine 文档，确认策略状态机和信号语义
2. 再读 dealer 文档，确认执行状态机和异常处理

## 5. 本次不做的事

- 不在 engine 里实现 `PENDING_BUY/POSITIONED/PENDING_SELL/ERROR_*`
- 不要求 engine 等待 dealer ack 后才能继续运行
- 不要求 engine 处理多个 dealer 的回报归并
- 不在第一阶段引入复杂的回测 ack 模拟链路

## 6. 后续扩展点

如果后续业务确实需要 engine 感知 dealer 结果，优先考虑增加“粗粒度执行状态”回传，例如：

- `has_position=true/false`
- `execution_state=idle/buy_working/holding/sell_working/error`

而不是把订单级别的所有回报细节直接灌回 engine。
