# FastOrderBook 卖一价维护错误 Bug 修复记录

## 日期
2026-01-12

## Bug 现象

测试输出显示：
- 买一价: 9.79元 (97900) ✓
- 卖一价: 8.01元 (80100) ✗

**问题**：卖一价 < 买一价，这在正常市场中不可能发生（否则会立即撮合成交）。

## 数据验证

Python 模拟脚本验证结果：
- 活跃买单数: 3675
- 活跃卖单数: 0
- 80100价格档位: 34个买单，量76000（全是买单，没有卖单）
- 正确的卖一价应该是"无"

## 根本原因分析

### 问题本质

80100 价格档位的情况：
- 该价位曾经有卖单，所以 `best_ask_idx_` 被设置为指向 80100
- 后来卖单全部成交/撤销，但该价位还有买单（买跌停价等待）
- `Level.total_volume` 不为 0，导致游标更新没有被触发

### 代码问题

1. **游标更新函数** (`update_best_ask_cursor`) 只检查 `total_volume > 0`，没有区分买卖方向：
```cpp
// 修复前
while (best_ask_idx_ <= max_idx && best_ask_idx_ != -1) {
    if (levels_[best_ask_idx_].total_volume > 0) {  // BUG: 没有区分买卖
        return;
    }
    best_ask_idx_++;
}
```

2. **游标更新触发条件** 检查的是 `total_volume == 0`，但同一档位可能有反向订单：
```cpp
// 修复前
if ((int32_t)lvl_idx == best_ask_idx_ && lvl_ptr->total_volume == 0) {
    update_best_ask_cursor();
}
```

## 修复方案

### 设计思路

从业务逻辑上，一个价格档位在稳定状态下只可能有买单或卖单其一（否则会立即撮合）。因此不需要在 Level 结构体中区分 `bid_volume` 和 `ask_volume`，而是通过检查链表头节点的方向来判断该档位属于买盘还是卖盘。

### 修改内容

#### 1. 修改 `update_best_bid_cursor()` (src/FastOrderBook.cpp:274-289)

```cpp
void FastOrderBook::update_best_bid_cursor() {
    while (best_bid_idx_ >= 0) {
        const Level& lvl = levels_[best_bid_idx_];
        // 检查该档位是否有买单（通过链表头节点的方向判断）
        if (lvl.total_volume > 0 && lvl.head_order_idx != -1) {
            const OrderNode& head = pool_.get(lvl.head_order_idx);
            if (head.side == Side::Buy) {
                return; // 找到了新的支撑位
            }
        }
        best_bid_idx_--;
    }
}
```

#### 2. 修改 `update_best_ask_cursor()` (src/FastOrderBook.cpp:291-308)

```cpp
void FastOrderBook::update_best_ask_cursor() {
    int32_t max_idx = (int32_t)levels_.size() - 1;
    while (best_ask_idx_ <= max_idx && best_ask_idx_ != -1) {
        const Level& lvl = levels_[best_ask_idx_];
        // 检查该档位是否有卖单（通过链表头节点的方向判断）
        if (lvl.total_volume > 0 && lvl.head_order_idx != -1) {
            const OrderNode& head = pool_.get(lvl.head_order_idx);
            if (head.side == Side::Sell) {
                return; // 找到了新的压力位
            }
        }
        best_ask_idx_++;
    }
    if (best_ask_idx_ > max_idx) best_ask_idx_ = -1;
}
```

#### 3. 修改游标更新触发条件 (src/FastOrderBook.cpp:202-220)

```cpp
if (node.side == Side::Buy) {
    if ((int32_t)lvl_idx == best_bid_idx_) {
        // 检查该档位是否还有买单
        bool has_bid = (lvl_ptr->head_order_idx != -1) &&
                       (pool_.get(lvl_ptr->head_order_idx).side == Side::Buy);
        if (!has_bid) {
            update_best_bid_cursor();
        }
    }
} else {
    if ((int32_t)lvl_idx == best_ask_idx_) {
        // 检查该档位是否还有卖单
        bool has_ask = (lvl_ptr->head_order_idx != -1) &&
                       (pool_.get(lvl_ptr->head_order_idx).side == Side::Sell);
        if (!has_ask) {
            update_best_ask_cursor();
        }
    }
}
```

## 验证结果

修复后测试输出：
```
=== 订单簿状态 ===
买一价: 9.79 元
买一量: 8299800
卖一价: 无

=== 内存池状态 ===
已分配节点: 7241
空闲节点: 3566
活跃订单: 3675
```

与 Python 模拟结果完全一致。

## 关于瞬态的讨论

**问题**：是否可能存在同一档位同时有买单和卖单的瞬态？

**分析**：
- 在历史数据回放中，先处理委托（on_order），后处理成交（on_trade）
- 理论上可能出现：卖单在头部（即将被成交），买单在尾部（新来的撮合订单）
- 但回放结束后，所有成交都已处理，不会存在瞬态

**结论**：检查链表头节点方向的方案在历史回放场景下是可靠的。新订单挂在链表尾部，头节点是最老的订单，代表该档位的"原始"方向。

## 其他修复

同时修复了市价单移除逻辑 (src/FastOrderBook.cpp:222-229)：
```cpp
else if (node.type == OrderType::Market) {
    auto it = std::find(market_orders_.begin(), market_orders_.end(), node_idx);
    if (it != market_orders_.end()) {
        // swap-and-pop 策略：O(1) 删除
        *it = market_orders_.back();
        market_orders_.pop_back();
    }
}
```

添加了必要的头文件：
```cpp
#include <algorithm>  // for std::find
```
