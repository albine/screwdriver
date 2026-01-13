# PriceLevelVolumeStrategy 回测指南

## 快速开始

### 1. 使用600759.SH数据回测（当前配置）

```bash
# 进入项目目录
cd /home/pc/screwdriver

# 编译（如果已编译可跳过）
cd build && make test_price_level_strategy -j4 && cd ..

# 运行回测
./build/test_price_level_strategy

# 查看结果日志
ls -lt logs/price_level_strategy_*.log | head -1 | awk '{print $NF}' | xargs cat
```

**预期输出**：
```
Statistics:
  - Orders: 254736
  - Transactions: 222715
  - Buy Trades (at preclose): 1850
  - Signal Triggered: true
```

---

## 2. 策略原理

**PriceLevelVolumeStrategy** 监控昨收价档位的挂单量变化，当主买成交力量超过平均挂单量时触发买入信号。

### 触发条件
- **前提**: 开盘价 < 昨收价
- **监控**: 昨收价档位的200ms滚动窗口
- **计算**:
  - `n` = 窗口内昨收价档位的平均挂单量
  - `delta_n` = 窗口内昨收价档位的主买成交总量
- **信号**: 当 `delta_n >= n` 时触发

### 关键参数
- `preclose_price`: 昨收价（整数，乘以10000）
- `open_price`: 开盘价（整数，乘以10000）
- 窗口大小: 200毫秒（固定）

---

## 3. 测试其他股票数据

### 步骤1：确定股票参数

从tick数据中提取昨收价和开盘价：

```bash
# 查看股票的tick数据（示例：600794.SH）
grep "PreClosePx:" test_data/MD_TICK_StockType_600794.SH.csv | head -1
grep "OpenPx:" test_data/MD_TICK_StockType_600794.SH.csv | grep "OpenPx: [1-9]" | head -1
```

或者手动从tick数据的XLSX文件中查看。

### 步骤2：修改配置文件

需要修改两个文件：

#### (1) 修改价格范围：`include/strategy_engine.h`

找到第225-226行，根据股票价格区间修改：

```cpp
// 600759.SH (3元股)
constexpr uint32_t MIN_PRICE = 30000;   // 3.00元
constexpr uint32_t MAX_PRICE = 40000;   // 4.00元

// 600794.SH (20元股)
constexpr uint32_t MIN_PRICE = 150000;  // 15.00元
constexpr uint32_t MAX_PRICE = 250000;  // 25.00元

// 603122.SH (10元股)
constexpr uint32_t MIN_PRICE = 70000;   // 7.00元
constexpr uint32_t MAX_PRICE = 110000;  // 11.00元
```

**建议**：MIN_PRICE < 跌停价，MAX_PRICE > 涨停价（涨跌停幅度约±10%）

#### (2) 修改测试参数：`test/test_price_level_strategy.cpp`

修改以下几处：

```cpp
// 第26-27行：修改昨收价和开盘价
uint32_t preclose_price = 35100;  // 改为目标股票的昨收价
uint32_t open_price = 34800;      // 改为目标股票的开盘价

// 第29行：修改策略名称
PriceLevelVolumeStrategy strategy("PriceLevel_600759.SH", ...);
                                  // 改为目标股票代码

// 第32行：修改股票代码
std::string symbol = "600759.SH";  // 改为目标股票代码

// 第59-60行：修改数据文件路径
order_file = "test_data/MD_ORDER_StockType_600759.SH.csv";
transaction_file = "test_data/MD_TRANSACTION_StockType_600759.SH.csv";
// 改为目标股票的数据文件路径
```

### 步骤3：编译并运行

```bash
cd /home/pc/screwdriver/build
make test_price_level_strategy -j4
cd ..
./build/test_price_level_strategy
```

---

## 4. 数据文件要求

### 数据格式

回测引擎需要**键值对格式**的TXT/CSV文件：

**订单数据示例**：
```
HTSCSecurityID: "600759.SH" MDDate: 20260112 MDTime: 91500010 OrderPrice: 38600 OrderQty: 14700 OrderBSFlag: 1 ...
```

**成交数据示例**：
```
HTSCSecurityID: "600759.SH" MDDate: 20260112 MDTime: 92500480 TradePrice: 34800 TradeQty: 1400 TradeBSFlag: 1 ...
```

### 关键字段说明

| 字段 | 说明 | 取值 |
|------|------|------|
| HTSCSecurityID | 股票代码 | 如"600759.SH" |
| MDDate | 交易日期 | YYYYMMDD格式 |
| MDTime | 交易时间 | HHMMSSMMM格式（毫秒） |
| OrderPrice | 委托价格 | 整数，乘以10000 |
| TradePrice | 成交价格 | 整数，乘以10000 |
| OrderBSFlag | 委托方向 | 1=买，2=卖 |
| TradeBSFlag | 成交方向 | 1=主买，2=主卖 |

**价格转换**：
- 3.51元 → 35100
- 13.20元 → 132000

### 数据文件位置

- **test_data/**: CSV格式原始数据（回测引擎直接使用）
- **test_data_cleaned/**: XLSX格式清洗数据（用于数据检查）

---

## 5. 结果分析

### 控制台输出

```
===========================================
Price Level Volume Strategy Test
===========================================
Strategy configured:
  Preclose Price: 35100 (3.51元)
  Open Price: 34800 (3.48元)
...
Statistics:
  - Orders: 254736
  - Transactions: 222715
  - Buy Trades (at preclose): 1850
  - Signal Triggered: true
===========================================
```

### 日志文件

日志位置：`logs/price_level_strategy_YYYYMMDD_HHMMSS.log`

**查看最新日志**：
```bash
ls -lt logs/price_level_strategy_*.log | head -1 | awk '{print $NF}' | xargs cat
```

**查看信号触发详情**：
```bash
grep "BUY SIGNAL" logs/price_level_strategy_*.log
```

**示例输出**：
```
[SIGNAL] BUY SIGNAL | Time=09:30:03.500 | Price=35100(3.51元) |
         n(avg_volume)=498483 | delta_n(buy_trades)=502700 |
         current_volume=304000 | window_size=202
```

### 关键指标解读

| 指标 | 说明 | 判断标准 |
|------|------|----------|
| Orders | 处理的订单数 | 应接近数据文件中的订单总数 |
| Transactions | 处理的成交数 | 应接近数据文件中的成交总数 |
| Buy Trades (at preclose) | 昨收价档位的主买成交次数 | 越多说明该档位交易活跃 |
| Signal Triggered | 是否触发信号 | true=触发，false=未触发 |
| n (avg_volume) | 窗口内平均挂单量 | 反映昨收价档位的流动性 |
| delta_n (buy_trades) | 窗口内主买成交量 | 反映主买力量强度 |
| current_volume | 触发时的挂单量 | 当前档位的待成交量 |
| window_size | 窗口快照数 | 200ms内的事件数量 |

---

## 6. 常见问题

### Q1: 编译报错 "No such file or directory"

**原因**：缺少依赖库或头文件路径不正确。

**解决**：
```bash
cd /home/pc/screwdriver/build
cmake ..
make test_price_level_strategy -j4
```

### Q2: 运行时报错 "Failed to load order/transaction file"

**原因**：数据文件路径错误或文件不存在。

**解决**：
1. 检查数据文件是否存在：
   ```bash
   ls -lh test_data/MD_ORDER_StockType_600759.SH.csv
   ls -lh test_data/MD_TRANSACTION_StockType_600759.SH.csv
   ```

2. 确保在项目根目录运行：
   ```bash
   cd /home/pc/screwdriver
   ./build/test_price_level_strategy
   ```

3. 或使用绝对路径运行：
   ```bash
   /home/pc/screwdriver/build/test_price_level_strategy
   ```

### Q3: Signal Triggered = false，策略未触发

**可能原因**：

1. **开盘价 >= 昨收价**：不满足监控条件
   - 检查：日志中应有 "Monitoring condition NOT met"
   - 解决：选择开盘价<昨收价的股票或日期

2. **昨收价档位交易不活跃**：主买成交量太少
   - 检查：`Buy Trades (at preclose)` 数量
   - 解决：选择更活跃的股票或档位

3. **触发条件从未满足**：delta_n < n 始终成立
   - 检查：日志中没有 "BUY SIGNAL"
   - 解决：调整策略参数或选择其他数据

### Q4: 价格范围越界警告

**症状**：日志中出现大量 "price out of range" 警告。

**原因**：`strategy_engine.h` 中的价格范围设置不正确。

**解决**：根据股票实际价格调整 MIN_PRICE 和 MAX_PRICE（见步骤2）。

### Q5: 如何验证回测结果的正确性？

**方法1：检查数据处理量**
```bash
# 统计订单数据行数
wc -l test_data/MD_ORDER_StockType_600759.SH.csv

# 统计成交数据行数
wc -l test_data/MD_TRANSACTION_StockType_600759.SH.csv

# 对比日志中的Orders和Transactions数量
```

**方法2：手动验证信号触发**
```bash
# 查找信号触发时间窗口的成交数据
grep "MDTime: 9300" test_data/MD_TRANSACTION_StockType_600759.SH.csv | \
    awk '/MDTime: 93003[0-9]{3}/ && /TradePrice: 35100/ && /TradeBSFlag: 1/'
```

---

## 7. 现有测试数据

| 股票代码 | 订单数据 | 成交数据 | 状态 |
|---------|---------|---------|------|
| 600759.SH | 254,736条 | 222,715条 | ✅ 已测试（信号触发） |
| 603122.SH | 55,480条 | 31,228条 | ✅ 已测试（信号未触发） |
| 600794.SH | XLSX格式 | XLSX格式 | ⚠️ 需转换 |
| 002603.SZ | XLSX格式 | XLSX格式 | ⚠️ 需转换 |

**数据转换**（如需要）：
```bash
# XLSX → CSV（回测格式）
python script/xlsx_to_replayer_format.py 600794.SH
```

---

## 8. 快速测试脚本

创建一键测试脚本：

```bash
#!/bin/bash
# 文件: run_backtest.sh

STOCK_CODE=${1:-"600759.SH"}
BUILD_DIR="build"

echo "=========================================="
echo "回测股票: $STOCK_CODE"
echo "=========================================="

# 编译
echo "编译测试程序..."
cd $BUILD_DIR && make test_price_level_strategy -j4 && cd .. || exit 1

# 运行
echo "运行回测..."
./$BUILD_DIR/test_price_level_strategy

# 查看结果
echo ""
echo "=========================================="
echo "最新日志文件:"
echo "=========================================="
LATEST_LOG=$(ls -t logs/price_level_strategy_*.log | head -1)
echo "日志路径: $LATEST_LOG"
echo ""
cat "$LATEST_LOG" | grep -A 10 "Statistics:"

echo ""
echo "信号触发详情:"
grep "BUY SIGNAL" "$LATEST_LOG" || echo "未触发信号"
```

**使用方法**：
```bash
chmod +x run_backtest.sh
./run_backtest.sh 600759.SH
```

---

## 9. 参数配置速查表

### 600759.SH (金陵饭店)
```cpp
uint32_t preclose_price = 35100;  // 3.51元
uint32_t open_price = 34800;      // 3.48元
constexpr uint32_t MIN_PRICE = 30000;   // 3.00元
constexpr uint32_t MAX_PRICE = 40000;   // 4.00元
order_file = "test_data/MD_ORDER_StockType_600759.SH.csv";
transaction_file = "test_data/MD_TRANSACTION_StockType_600759.SH.csv";
```

### 603122.SH (示例)
```cpp
uint32_t preclose_price = 132000;  // 13.20元
uint32_t open_price = 130000;      // 13.00元
constexpr uint32_t MIN_PRICE = 70000;   // 7.00元
constexpr uint32_t MAX_PRICE = 110000;  // 11.00元
order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.txt";
transaction_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt";
```

---

## 10. 进阶使用

### 批量回测多只股票

```bash
#!/bin/bash
# 文件: batch_backtest.sh

STOCKS=("600759.SH" "603122.SH" "600794.SH")

for STOCK in "${STOCKS[@]}"; do
    echo "=========================================="
    echo "回测 $STOCK"
    echo "=========================================="

    # 这里需要手动修改配置文件或使用配置文件模板
    # 然后编译运行

    ./build/test_price_level_strategy

    # 保存日志
    LATEST_LOG=$(ls -t logs/price_level_strategy_*.log | head -1)
    cp "$LATEST_LOG" "results/${STOCK}_backtest.log"
done
```

### 导出结果到CSV

```bash
# 提取所有回测结果
for log in logs/price_level_strategy_*.log; do
    echo -n "$(basename $log),"
    grep "Orders:" $log | awk -F': ' '{print $2}' | tr '\n' ','
    grep "Transactions:" $log | awk -F': ' '{print $2}' | tr '\n' ','
    grep "Signal Triggered:" $log | awk -F': ' '{print $2}'
done > backtest_results.csv
```

---

## 联系与反馈

- 项目路径: `/home/pc/screwdriver`
- 策略实现: `src/PriceLevelVolumeStrategy.h`
- 测试程序: `test/test_price_level_strategy.cpp`
- 日志目录: `logs/`

有问题请查看日志文件获取详细调试信息。
