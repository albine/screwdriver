# 数据清洗脚本说明

从国泰安逐笔合并数据清洗得到华泰逐笔委托和成交数据。

## 脚本列表

| 脚本 | 功能 |
|------|------|
| `split_tick_data.py` | 拆分国泰安逐笔合并数据为委托和成交两个文件 |
| `verify_split.py` | 验证拆分结果的正确性 |

## 使用方法

### 拆分数据

```bash
# 默认处理 test_data/603122-SSEL2_Tick_20251031.xlsx
python3 split_tick_data.py

# 指定输入文件
python3 split_tick_data.py test_data/其他文件.xlsx

# 指定输入文件和输出目录
python3 split_tick_data.py test_data/其他文件.xlsx output_dir
```

输出文件命名规则：
- `MD_ORDER_StockType_{证券代码}.SH.xlsx` - 逐笔委托
- `MD_TRANSACTION_StockType_{证券代码}.SH.xlsx` - 逐笔成交

### 验证拆分结果

```bash
# 默认验证603122
python3 verify_split.py

# 指定文件
python3 verify_split.py 源文件.xlsx ORDER文件.xlsx TRANSACTION文件.xlsx
```

## 字段映射规则

### 逐笔委托 (TickType = A/D → MD_ORDER)

| 国泰安字段 | 华泰字段 | 转换规则 |
|------------|----------|----------|
| 6Symbol证券代码 | HTSCSecurityID | 加`.SH`后缀 |
| 7TickTime逐笔时间 | MDTime | 直接使用 |
| 固定值 | securityIDSource | `XSHG` |
| 固定值 | securityType | `StockType` |
| 4RecID逐笔序号 | OrderIndex | 直接使用 |
| 12TickType类型 | OrderType | A=2, D=10 |
| 8TickPrice逐笔价格 | OrderPrice | ×10000 |
| 9TickVolume逐笔数量 | OrderQty | 直接使用 |
| 11TickBSFlag逐笔标识 | OrderBSFlag | B=1, S=2 |
| 5Channel逐笔通道 | ChannelNo | 直接使用 |
| 13/14OrderNO | OrderNO | B取Buy, S取Sell |
| 4RecID逐笔序号 | ApplSeqNum | 直接使用 |
| 固定值 | DataMultiplePowerOf10 | `4` |
| 10TradeAmount | TradedQty | 直接使用 |

### 逐笔成交 (TickType = T → MD_TRANSACTION)

| 国泰安字段 | 华泰字段 | 转换规则 |
|------------|----------|----------|
| 6Symbol证券代码 | HTSCSecurityID | 加`.SH`后缀 |
| 7TickTime逐笔时间 | MDTime | 直接使用 |
| 固定值 | securityIDSource | `XSHG` |
| 固定值 | securityType | `StockType` |
| 4RecID逐笔序号 | TradeIndex | 直接使用 |
| 13BuyOrderNO买方订单 | TradeBuyNo | 直接使用 |
| 14SellOrderNO卖方订单 | TradeSellNo | 直接使用 |
| 11TickBSFlag逐笔标识 | TradeBSFlag | B=1, S=2, N=0 |
| 8TickPrice逐笔价格 | TradePrice | ×10000 |
| 9TickVolume逐笔数量 | TradeQty | 直接使用 |
| 10TradeAmount | TradeMoney | ×10000 |
| 4RecID逐笔序号 | ApplSeqNum | 直接使用 |
| 5Channel逐笔通道 | ChannelNo | 直接使用 |
| 固定值 | DataMultiplePowerOf10 | `4` |

### 过滤规则

- **TickType = S** (交易状态信息如OCALL/TRADE/CLOSE等): 跳过不导出

## 依赖

```bash
pip install pandas openpyxl
```
