# 上海市场"乱序"问题修复报告

## 问题描述

### 现象
实盘运行时，日志中出现大量错误信息：
```
[ERR!] [OrderBook] Shanghai trade: tradebuyno should not exist when bsflag is Buy
[ERR!] [OrderBook] Shanghai trade: tradesellno should not exist when bsflag is Sell
```

### 问题背景
上海市场的成交处理逻辑假设：
- 当 `bsflag=Buy`（买方主动成交）时，`tradebuyno` 对应的委托不应该存在于订单簿中
- 当 `bsflag=Sell`（卖方主动成交）时，`tradesellno` 对应的委托不应该存在于订单簿中

如果这些委托存在，说明可能发生了"乱序"——委托先于成交到达。

## 问题分析

### 第一步：增强日志

原始日志信息过于简单，无法定位问题。修改 `src/FastOrderBook.cpp` 增加详细日志：

```cpp
// 修改前
LOG_M_ERROR(logger, "Shanghai trade: tradebuyno should not exist when bsflag is Buy");

// 修改后
auto it = order_index_.find(txn.tradebuyno);
if (it != order_index_.end()) {
    const OrderNode& order = pool_.get(it->second);
    LOG_M_ERROR(logger,
        "Shanghai out-of-order: tradebuyno exists when bsflag=Buy | "
        "txn: seq={}, security={}, buyno={}, sellno={}, price={}, qty={}, type={} | "
        "order: seq={}, price={}, vol={}, side={}, type={}",
        txn.applseqnum, txn.htscsecurityid, txn.tradebuyno, txn.tradesellno,
        txn.tradeprice, txn.tradeqty, txn.tradetype,
        order.seq, order.sort_price, order.volume,
        static_cast<int>(order.side), static_cast<int>(order.type));
}
```

### 第二步：分析日志数据

统计出现问题的证券代码：
```bash
grep -oE "security=[0-9]+\.[A-Z]+" live_trading.log | sed 's/security=//' | sort | uniq -c | sort -rn
```

结果：
| 代码前缀 | 类型 | 出现次数 |
|---------|------|---------|
| 204xxx.SH | 国债逆回购 | 926 |
| 110xxx.SH | 可转债 | 191 |
| 113xxx.SH | 可转债 | 多个 |
| 118xxx.SH | 可交债 | 多个 |

### 第三步：确认根本原因

**关键发现：所有出现问题的证券都不是股票！**

- 股票代码：600xxx/601xxx/603xxx（上海）、000xxx/002xxx（深圳）
- 问题代码：110xxx/113xxx/118xxx（可转债）、204xxx（国债逆回购）

**结论**：这不是真正的"乱序"问题，而是**非股票品种的交易机制不同**，不适用股票的成交处理逻辑。

### 第四步：检查订阅逻辑

查看 `src/main.cpp` 中的订阅代码：
```cpp
security_source_shg->set_securitytype(StockType);  // 已指定只订阅股票
security_source_she->set_securitytype(StockType);
```

**发现**：订阅时已经指定了 `StockType`，但服务器仍然推送了非股票数据。这说明服务器端的分类可能将可转债等归入了 StockType。

## 修复方案

### 方案：在 Adapter 层添加过滤

修改 `include/live_market_adapter.h`，在数据进入策略引擎前过滤非股票类型：

```cpp
case MD_ORDER: {
    if (data.has_mdorder()) {
        const auto& pb_order = data.mdorder();
        // 过滤非股票类型 (StockType = 2)
        if (pb_order.securitytype() != StockType) break;
        MDOrderStruct order;
        convert_to_order_fast(pb_order, order);
        engine_->on_market_order(order);
    }
    break;
}

case MD_TRANSACTION: {
    if (data.has_mdtransaction()) {
        const auto& pb_txn = data.mdtransaction();
        // 过滤非股票类型 (StockType = 2)
        if (pb_txn.securitytype() != StockType) break;
        MDTransactionStruct transaction;
        convert_to_transaction_fast(pb_txn, transaction);
        engine_->on_market_transaction(transaction);
    }
    break;
}
```

### 相关修改

1. **`include/market_data_structs.h`**：MDOrderStruct 添加 `securitytype` 字段
2. **`include/live_market_adapter.h`**：
   - `convert_to_order_fast()` 添加 securitytype 转换
   - `OnMarketData()` 添加 securitytype 过滤逻辑

## 性能影响

- 过滤检查在数据转换之前执行，避免无用的内存操作
- `securitytype()` 是简单的 int getter，开销可忽略
- 过滤掉非股票数据后，减少了后续处理的无效负载

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `include/market_data_structs.h` | MDOrderStruct 添加 securitytype 字段 |
| `include/live_market_adapter.h` | 添加 securitytype 过滤逻辑 |
| `src/FastOrderBook.cpp` | 增强乱序检测的日志输出 |

## 后续建议

1. **联系行情服务商**：确认 StockType 的具体包含范围，询问是否有更精确的订阅方式
2. **监控日志**：观察过滤后是否还有"乱序"错误出现
3. **考虑支持其他品种**：如果需要交易可转债等，需要针对性地实现其成交处理逻辑

## 时间线

- 2026-01-13：发现问题，增强日志
- 2026-01-13：分析确认为非股票品种导致
- 2026-01-13：添加 securitytype 过滤修复
