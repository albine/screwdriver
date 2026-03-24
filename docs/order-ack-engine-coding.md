# 打板策略 Engine 侧编码文档

> 基于：`order-ack-engine-design.md`
>
> 目标：将 DabanStrategy 从当前 4 状态（SLEEPING/ACTIVE/POSITIONED/DONE）改为设计中的 4 状态（SLEEPING/ACTIVE/EXIT_ARMED/DONE），修正 on_tick 保底逻辑，对齐业务规则。

## 1. 改动清单

### 1.1 需要修改的文件

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `src/strategy/DabanStrategy.h` | 修改 | 核心改动：状态机重命名 + on_tick 重构 + 收盘处理 |

### 1.2 不需要修改的文件

| 文件 | 原因 |
|------|------|
| `include/strategy_base.h` | 不新增回调，保持不变 |
| `include/strategy_engine.h` | 不新增 ControlMessage 类型 |
| `include/zmq_client.h` | 不新增 ack/reject 处理 |
| `include/zmq_protocol.h` | 不新增 Action 类型 |
| `include/trade_signal.h` | TradeSignal 结构不变 |
| `src/strategy_context.cpp` | LiveContext/BacktestContext 不变 |

补充说明：

- 当前 live 下发给 dealer 的 payload 语义保持不变，`strategy` 继续发送策略类型名（例如 `DabanStrategy`）
- 本次只改 `DabanStrategy` 的状态机和时序，不改 engine/dealer 通信字段

---

## 2. DabanStrategy.h 改动明细

### 2.1 State 枚举

**当前：**
```cpp
enum class State {
    SLEEPING,      // 所有成交<7%
    ACTIVE,        // 有成交>8%
    POSITIONED,    // BUY信号已发，监控炸板中
    DONE           // 终态
};
```

**改为：**
```cpp
enum class State {
    SLEEPING,      // 尚未激活，不允许触发 BUY
    ACTIVE,        // 已激活，允许检测并发出 BUY_SIGNAL
    EXIT_ARMED,    // BUY_SIGNAL 已发出，盯退出条件（不是持仓状态）
    DONE           // 本轮策略结束
};
```

**影响范围**：所有引用 `State::POSITIONED` 的地方改为 `State::EXIT_ARMED`。

---

### 2.2 数据成员变更

#### 2.2.1 新增

| 成员 | 类型 | 说明 |
|------|------|------|
| 无 | — | 本次不新增数据成员 |

#### 2.2.2 删除

| 成员 | 类型 | 说明 |
|------|------|------|
| 无 | — | 本次不删除数据成员 |

#### 2.2.3 修改注释

| 成员 | 当前注释 | 改为 |
|------|---------|------|
| `flow_detector_` | `炸板检测器（POSITIONED 阶段使用）` | `炸板检测器（EXIT_ARMED 阶段使用）` |
| `activate_price_` | `激活阈值价格` | `激活阈值价格（涨幅 > 8% 的价格边界）` |
| `t0_` | `首次触发激活的成交时间（旧注释）` | `首次>8%成交时间` |

#### 2.2.4 常量新增

| 成员 | 类型 | 值 | 说明 |
|------|------|-----|------|
| `MARKET_CLOSE_TIME` | `static constexpr int32_t` | `145700000` | 14:57:00.000，连续竞价结束 |
| `MARKET_OPEN_TIME` | `static constexpr int32_t` | `93000000` | 09:30:00.000，连续竞价开始 |

---

### 2.3 方法变更总览

| 方法 | 变更类型 | 说明 |
|------|---------|------|
| `on_tick()` | **重写** | 加入保底状态切换 + 修正 SLEEPING 不直接触发 + 收盘处理 |
| `on_order()` | 小改 | `POSITIONED` → `EXIT_ARMED`，加收盘检查 |
| `on_transaction()` | 小改 | `POSITIONED` → `EXIT_ARMED`，加收盘检查 |
| `check_state_transition()` | 小改 | `POSITIONED` → `EXIT_ARMED` |
| `emit_buy_signal()` | 小改 | `POSITIONED` → `EXIT_ARMED`，改日志文案 |
| `emit_buy_signal_from_tick()` | 小改 | `POSITIONED` → `EXIT_ARMED`，改日志文案 |
| `emit_sell_signal()` | 小改 | `EXIT_ARMED` 替换 `POSITIONED`，改日志文案 |
| `state_to_string()` | 小改 | `POSITIONED` → `EXIT_ARMED` |
| `handle_order()` | 不变 | 已经只在 ACTIVE 状态工作 |
| `handle_trade()` | 不变 | 无状态检查 |
| `handle_cancel()` | 不变 | 已经只在 ACTIVE 状态工作 |
| `handle_cancel_from_transaction()` | 不变 | 同上 |
| `check_close()` | **新增** | 收盘检查，统一入口 |
| `check_tick_state_switch()` | **新增** | on_tick 保底状态切换逻辑 |

---

## 3. 逐方法实现步骤

### 3.1 `check_close(int32_t current_time)` — 新增

收盘检查的统一入口，在 on_tick / on_order / on_transaction 中调用。

```
输入：current_time (MDTime)
返回：bool（true = 已进入 DONE，调用方应立即 return）

步骤：
1. if current_time < MARKET_CLOSE_TIME → return false
2. if state_ 已经是 DONE → return true
3. 记录日志：LOG_M_INFO("DabanStrategy CLOSE: {} | {} -> DONE", symbol, state_to_string(state_))
4. state_ = State::DONE
5. return true
```

### 3.2 `check_tick_state_switch(const MDStockStruct& stock)` — 新增

on_tick 中 SLEEPING ↔ ACTIVE 的保底切换逻辑。仅在 SLEEPING 或 ACTIVE 状态下调用。

```
输入：stock (MDStockStruct)
返回：void

步骤：
1. 取 ask1_price = stock.sellpricequeue[0]
2. 取 bid1_price = stock.buypricequeue[0]
3. if ask1_price > 0 且 prev_close_ > 0:
     计算 ask1_gain = (double)(ask1_price - prev_close_) / prev_close_
     if state_ == SLEEPING 且 ask1_gain > activate_threshold_:
       调用 activate(stock.mdtime)
     else if state_ == ACTIVE 且 ask1_gain < sleep_threshold_:
       调用 sleep()
4. else if ask1_price == 0:
     // 无卖单，可能涨停封死
     if state_ == SLEEPING 且 bid1_price == limit_up_price_ 且 bid1_price > 0:
       调用 activate(stock.mdtime)
     // 卖一价为 0 时不触发休眠（可能是涨停封死）
```

### 3.3 `on_tick()` — 重写

当前代码的问题：
- 允许从 SLEEPING 直接触发 BUY（违反规则 2.2）
- 没有 on_tick 保底状态切换
- 没有收盘处理

改后步骤：

```
1. if !is_enabled() → return
2. tick_count_++
3. if state_ == DONE → return
4. 价格初始化（保持不变）
5. if stock.mdtime < MARKET_OPEN_TIME → return
6. if check_close(stock.mdtime) → return
7. if state_ == SLEEPING || state_ == ACTIVE:
     a. 调用 check_tick_state_switch(stock)  ← 保底状态切换
     b. if state_ == ACTIVE 且 limit_up_price_ > 0 且 !signal_triggered_:
          保底买入条件检查（现有三个条件不变）
          命中则 emit_buy_signal_from_tick(stock.mdtime)
     // 注意：步骤 a 可能刚把 SLEEPING 切成 ACTIVE，步骤 b 就能触发
     // 但 SLEEPING 不会直接进入步骤 b（因为判断的是 state_ == ACTIVE）
8. else if state_ == EXIT_ARMED 且 limit_up_price_ > 0:
     保底卖出检测（现有三个条件不变）
     命中则 emit_sell_signal(stock.mdtime)
```

关键修正：步骤 7b 只检查 `state_ == ACTIVE`（不再包含 SLEEPING）。

### 3.4 `on_order()` — 小改

```
推荐顺序：
1. if !is_enabled() → return
2. current_time = order.mdtime
3. if state_ == DONE → return
4. if current_time < MARKET_OPEN_TIME → return
5. if check_close(current_time) → return
6. if prev_close_ == 0 → return
7. if delayed → return
8. order_count_++
9. 所有 State::POSITIONED → State::EXIT_ARMED（2 处）
   - EXIT_ARMED 阶段继续喂 flow_detector_
   - 命中后继续 emit_sell_signal(current_time)
```

其余逻辑不变。

### 3.5 `on_transaction()` — 小改

与 on_order 对称：

```
推荐顺序：
1. if !is_enabled() → return
2. current_time = txn.mdtime
3. if state_ == DONE → return
4. if current_time < MARKET_OPEN_TIME → return
5. if check_close(current_time) → return
6. if prev_close_ == 0 → return
7. if delayed → return
8. transaction_count_++
9. 所有 State::POSITIONED → State::EXIT_ARMED（2 处）
   - EXIT_ARMED 阶段继续喂 flow_detector_
   - 命中后继续 emit_sell_signal(current_time)
```

### 3.6 `check_state_transition()` — 小改

```
改动点：
1. case State::POSITIONED → case State::EXIT_ARMED
2. 注释改为 "BUY 已发出，不回退"
```

### 3.7 `emit_buy_signal(int32_t, const FastOrderBook&)` — 小改

```
改动点：
1. state_ = State::POSITIONED → state_ = State::EXIT_ARMED
2. 日志 "-> POSITIONED" → "-> EXIT_ARMED"
```

### 3.8 `emit_buy_signal_from_tick(int32_t)` — 小改

```
改动点：
1. state_ = State::POSITIONED → state_ = State::EXIT_ARMED
2. 日志 "-> POSITIONED" → "-> EXIT_ARMED"
```

### 3.9 `emit_sell_signal(int32_t)` — 小改

```
改动点：
1. if (state_ != State::POSITIONED) → if (state_ != State::EXIT_ARMED)
2. 其余不变（state_ = State::DONE 保持）
```

### 3.10 `state_to_string()` — 小改

```
改动点：
1. case State::POSITIONED → case State::EXIT_ARMED
2. return "POSITIONED" → return "EXIT_ARMED"
```

### 3.11 类头注释 — 更新

```
改动点：
1. 状态机注释：
   SLEEPING → ACTIVE → POSITIONED → DONE
   改为：
   SLEEPING → ACTIVE → EXIT_ARMED → DONE
2. 激活条件注释统一为：> 8%
3. 新增说明：EXIT_ARMED 不是持仓状态，只表示 BUY 信号已发出
```

---

## 4. 改动影响分析

### 4.1 行为变化

| 场景 | 改前行为 | 改后行为 |
|------|---------|---------|
| SLEEPING + on_tick 保底条件命中 | 直接发 BUY | 先切 ACTIVE，再检查触发，符合条件才发 BUY |
| 卖一价涨幅 > 8% 但买入条件未命中 | 不切状态 | SLEEPING → ACTIVE（保底切换），等逐笔触发 |
| 卖一价为 0 + 买一价 == 涨停价 | 保底买入条件 3 命中（从 SLEEPING） | 先切 ACTIVE，再由条件 3 触发 |
| 14:57 后收到行情 | 继续处理到 15:00 | 立即进 DONE |

### 4.2 不变的行为

| 场景 | 行为 |
|------|------|
| 逐笔路径 SLEEPING ↔ ACTIVE 切换 | 不变（check_state_transition 逻辑未改） |
| ACTIVE 状态逐笔买入触发 (n > m/2) | 不变 |
| EXIT_ARMED 逐笔炸板检测 | 不变（仅状态名 POSITIONED → EXIT_ARMED） |
| EXIT_ARMED tick 保底卖出 | 不变（仅状态名） |
| 集合竞价过滤 (< 9:30) | 不变 |
| 激活阈值严格大于 8% | 不变（当前代码已是 `>`，本次只是统一文档/注释口径） |

---

## 5. 编码顺序

建议按以下顺序修改，每步编译验证：

1. **State 枚举 + state_to_string**：POSITIONED → EXIT_ARMED，全文替换
2. **新增常量**：MARKET_CLOSE_TIME, MARKET_OPEN_TIME
3. **新增 check_close()**：收盘检查方法
4. **新增 check_tick_state_switch()**：on_tick 保底状态切换
5. **重写 on_tick()**：整合保底切换 + 修正 SLEEPING 触发 + 收盘
6. **小改 on_order() / on_transaction()**：加收盘检查 + 状态名替换
7. **小改 emit_buy/sell 方法**：状态名 + 日志
8. **更新类头注释**
9. **编译验证**：`./build.sh engine`
10. **回测验证**：`python run_backtest.py YYYYMMDD`，对比改前改后日志

---

## 6. 测试检查点

| 检查项 | 方法 |
|--------|------|
| SLEEPING 不直接触发 BUY | 以下任一方式即可：1. 日志顺序检查：保底 BUY 前必须先出现 `STATE_CHANGE: SLEEPING -> ACTIVE`；2. 代码检查：`on_tick()` 的保底 BUY 分支只在 `state_ == ACTIVE` 下执行；3. 回测 case：从 SLEEPING 起步的保底 BUY 不允许绕过 `check_tick_state_switch()` |
| on_tick 保底切换生效 | 回测日志中应出现 "STATE_CHANGE: SLEEPING -> ACTIVE"（由 on_tick 驱动） |
| 14:57 后进 DONE | 回测日志中应出现 "CLOSE: ... -> DONE" 且时间 >= 14:57 |
| 阈值 > 8%（不含 8%） | 准备一个恰好 8% 涨幅的股票，确认仍然不激活；这项主要用于确认文档/注释与现有代码一致 |
| EXIT_ARMED 炸板检测正常 | 对比改前改后的 SELL 触发时间和条件一致 |
