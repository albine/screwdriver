# OrderBook 测试指南

## 概述

本文档介绍如何使用历史数据对 FastOrderBook 进行回放测试，验证订单簿的正确性。

## 目录结构

```
screwdriver/
├── src/
│   ├── FastOrderBook.h          # 订单簿头文件
│   └── FastOrderBook.cpp        # 订单簿实现
├── test_fastorderbook.cpp       # 测试程序
├── test_data_cleaned/           # 测试数据目录
│   ├── MD_ORDER_StockType_603122.SH.txt
│   └── MD_TRANSACTION_StockType_603122.SH.txt
└── script/
    └── simulate_orderbook.py    # Python 模拟脚本 (用于对比验证)
```

## 数据准备

### 1. 数据格式

测试使用两个数据文件：

**委托文件 (MD_ORDER)** - Tab 分隔：
```
securityid  applseqnum  side  ordtype  orderprice  orderqty  ...
603122      1           1     2        97800       1000      ...
```

字段说明：
- `securityid`: 证券代码
- `applseqnum`: 委托序号 (唯一标识)
- `side`: 买卖方向 (1=买, 2=卖)
- `ordtype`: 订单类型 (1=限价, 2=市价, 3=本方最优)
- `orderprice`: 委托价格 (单位: 0.0001元，如 97800 = 9.78元)
- `orderqty`: 委托数量

**成交文件 (MD_TRANSACTION)** - Tab 分隔：
```
securityid  tradetype  tradebuyno  tradesellno  tradeprice  tradeqty  ...
603122      0          1           2            97800       500       ...
```

字段说明：
- `tradetype`: 成交类型 (0=成交, 1/2/5/6/7/8=撤单)
- `tradebuyno`: 买方委托号 (撤单时为0表示卖撤)
- `tradesellno`: 卖方委托号 (撤单时为0表示买撤)
- `tradeprice`: 成交价格
- `tradeqty`: 成交/撤单数量

### 2. 数据来源

原始数据通常为 Excel 格式，使用转换脚本处理：

```bash
# 将 Excel 转换为 txt 格式
python3 xlsx_to_replayer_format.py
```

## 编译

### 使用 CMake 构建

```bash
cd /home/dy/screwdriver
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
```

### 手动编译 (可选)

```bash
g++ -std=c++17 -O2 -I include -I src \
    test_fastorderbook.cpp src/FastOrderBook.cpp \
    -o test_fastorderbook -lpthread
```

## 运行测试

### 1. 执行测试程序

```bash
cd /home/dy/screwdriver
./build/test_fastorderbook
```

### 2. 预期输出

```
=== FastOrderBook 历史数据回放测试 ===

加载数据文件...
总事件数: 86708

开始回放...

=== 回放完成 ===
耗时: 71 ms

=== 处理统计 ===
委托消息: 55480 (成功: 55480)
成交消息: 31228 (成功: 31228)

=== 订单簿状态 (买卖十档) ===

--- 卖盘 ---
  (空)
------------
--- 买盘 ---
  买1: 9.79 元  8299800
  买2: 9.78 元  13400
  买3: 9.77 元  14500
  ...

=== 内存池状态 ===
已分配节点: 7241
空闲节点: 3566
活跃订单: 3675
```

## 验证正确性

### 1. 使用 Python 脚本对比

```bash
cd /home/dy/screwdriver
python3 script/simulate_orderbook.py
```

Python 脚本会输出：
- 活跃买单数/卖单数
- 买卖盘价格分布
- 各档位订单数和总量

### 2. 对比检查项

| 检查项 | 说明 |
|--------|------|
| 活跃订单数 | FastOrderBook 与 Python 应一致 |
| 买一价/量 | 价格和数量应完全匹配 |
| 卖一价/量 | 价格和数量应完全匹配 |
| 买卖十档 | 所有档位价格和数量应一致 |

### 3. 常见问题排查

**问题：卖一价 < 买一价**
- 原因：游标更新逻辑没有区分买卖方向
- 检查：`update_best_ask_cursor()` 是否检查订单方向

**问题：活跃订单数不匹配**
- 原因：成交/撤单处理逻辑有误
- 检查：`on_trade()` 和 `cancel_order()` 的实现

**问题：某档位量不匹配**
- 原因：量的增减计算有误
- 检查：`add_node_to_level()` 和 `remove_node_from_level()`

## 测试不同股票

### 1. 准备新数据

将新的 Excel 数据放入 `test_data_cleaned/` 目录，命名格式：
- `MD_ORDER_StockType_<股票代码>.<市场>.txt`
- `MD_TRANSACTION_StockType_<股票代码>.<市场>.txt`

### 2. 修改测试参数

编辑 `test_fastorderbook.cpp`：

```cpp
// 根据股票调整价格范围
const uint32_t MIN_PRICE = 70000;   // 最低价 * 10000
const uint32_t MAX_PRICE = 110000;  // 最高价 * 10000
const uint32_t STOCK_CODE = 603122; // 股票代码

// 修改数据文件路径
replayer.load_order_file("test_data_cleaned/MD_ORDER_StockType_<新股票>.txt");
replayer.load_transaction_file("test_data_cleaned/MD_TRANSACTION_StockType_<新股票>.txt");
```

### 3. 重新编译运行

```bash
cd /home/dy/screwdriver/build
make -j4
cd ..
./build/test_fastorderbook
```

## 性能测试

### 1. Release 模式编译

```bash
cd /home/dy/screwdriver/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

### 2. 性能指标

典型性能 (603122, 86708 事件)：
- Debug 模式: ~70-90 ms
- Release 模式: ~20-30 ms
- 吞吐量: ~100万+ 事件/秒

### 3. 内存使用

- 预分配订单池: 200000 节点
- 单节点大小: 32 字节 (对齐)
- Level 数组: (MAX_PRICE - MIN_PRICE) * 16 字节

## API 参考

### 核心接口

```cpp
// 处理委托
bool on_order(const MDOrderStruct& order);

// 处理成交
bool on_transaction(const MDTransactionStruct& txn);

// 获取买一/卖一价
std::optional<uint32_t> get_best_bid() const;
std::optional<uint32_t> get_best_ask() const;

// 获取指定价格档位的量
uint64_t get_volume_at_price(uint32_t price) const;

// 获取买卖N档数据 (价格, 量)
std::vector<std::pair<uint32_t, uint64_t>> get_bid_levels(int n) const;
std::vector<std::pair<uint32_t, uint64_t>> get_ask_levels(int n) const;
```

### 使用示例

```cpp
#include "FastOrderBook.h"
#include "ObjectPool.h"

// 初始化
ObjectPool<OrderNode> pool(200000);
FastOrderBook orderbook(603122, pool, 70000, 110000);

// 处理数据
orderbook.on_order(order);
orderbook.on_transaction(txn);

// 查询买卖十档
auto bids = orderbook.get_bid_levels(10);
auto asks = orderbook.get_ask_levels(10);

for (auto& [price, volume] : bids) {
    std::cout << price / 10000.0 << " 元: " << volume << std::endl;
}
```
