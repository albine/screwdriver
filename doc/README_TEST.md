# 策略测试快速指南

## 快速开始

### 1. 编译

**推荐使用 CMake 编译**（自动处理 quill 库依赖）：
```bash
cd /home/pc/screwdriver
./build_cmake.sh

# 或手动使用 CMake
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make test_strategy_simple -j4
```

**首次编译会自动下载 quill 库**（需要网络连接，约 5-10 秒）

### 2. 运行测试

```bash
# 默认参数 - 每 10000 条消息打印一次十档盘口
./build/test_strategy_simple

# 更频繁的采样 - 每 1000 条消息打印一次
./build/test_strategy_simple --sample 1000

# 更少的输出 - 每 50000 条消息打印一次
./build/test_strategy_simple --sample 50000

# 查看帮助
./build/test_strategy_simple --help
```

---

## 输出示例

### 启动信息
```
=== 策略测试程序 ===
股票: 002603.SZ
委托文件: test_data/MD_ORDER_StockType_002603.SZ.csv
成交文件: test_data/MD_TRANSACTION_StockType_002603.SZ.csv
采样间隔: 10000

启动策略引擎...
加载历史数据...
数据加载完成，总事件数: 86708
开始回放数据...
```

### 订单簿快照（每 N 条消息打印一次）
```
============================================================
订单簿快照 #10000 (ApplSeqNum: 123456)
============================================================

--- 卖盘 (Ask) ---
  卖10: 9.8500 元       12000
  卖 9: 9.8400 元       15000
  卖 8: 9.8300 元       18000
  卖 7: 9.8200 元       20000
  卖 6: 9.8100 元       25000
  卖 5: 9.8000 元       30000
  卖 4: 9.7950 元       35000
  卖 3: 9.7900 元       40000
  卖 2: 9.7850 元       50000
  卖 1: 9.7800 元       60000
----------------------------------------
--- 买盘 (Bid) ---
  买 1: 9.7750 元       55000
  买 2: 9.7700 元       45000
  买 3: 9.7650 元       40000
  买 4: 9.7600 元       35000
  买 5: 9.7550 元       30000
  买 6: 9.7500 元       25000
  买 7: 9.7450 元       20000
  买 8: 9.7400 元       18000
  买 9: 9.7350 元       15000
  买10: 9.7300 元       12000

价差: 0.0050 元 (5.12 bps)
============================================================
```

### 异常检测
如果订单簿状态异常，会显示警告：
```
⚠️  WARNING: 买一价 >= 卖一价！订单簿状态异常！
```

### 验证信息
```
✅ Validation passed at Txn #10000
✅ Validation passed at Txn #20000
```

### 最终统计
```
等待处理完成...
回放完成，耗时: 245 ms
停止策略引擎...

==================================================
=== SimpleTest 统计报告 ===
委托消息: 55480
成交消息: 31228
总消息数: 86708
运行时间: 245 ms
吞吐量: 353910 msg/s

==================================================

✅ 测试完成！
```

---

## 关键特性

### 1. 十档盘口显示
- ✅ 每次采样自动显示买卖十档
- ✅ 卖盘倒序显示（价格从高到低）
- ✅ 买盘正序显示（价格从高到低）
- ✅ 自动计算买卖价差（绝对值和基点 bps）

### 2. 状态验证
- ✅ 自动检测买一价 >= 卖一价的异常情况
- ✅ 定期验证订单簿合法性
- ✅ 实时显示验证结果

### 3. 性能统计
- ✅ 消息处理数量（委托、成交）
- ✅ 运行时间
- ✅ 吞吐量（msg/s）

---

## 调整采样频率

采样频率影响：
- **更高频率** (`--sample 1000`): 更多输出，更容易调试，但会降低性能
- **更低频率** (`--sample 100000`): 更少输出，更高性能，适合压力测试
- **默认频率** (`--sample 10000`): 平衡输出和性能

### 性能对比

| 采样间隔 | 输出量 | 吞吐量 | 适用场景 |
|----------|--------|--------|---------|
| 1,000 | 高 | ~300K msg/s | 功能调试 |
| 10,000 | 中 | ~350K msg/s | 日常测试 |
| 50,000 | 低 | ~400K msg/s | 性能验证 |
| 无输出* | 无 | ~450K msg/s | 压力测试 |

*修改代码注释掉所有输出

---

## 数据文件要求

测试程序需要以下文件（放在 `test_data/` 目录下）：

1. **委托文件**: `MD_ORDER_StockType_002603.SZ.csv`
2. **成交文件**: `MD_TRANSACTION_StockType_002603.SZ.csv`

### 文件格式
```
HTSCSecurityID: "002603.SZ" MDDate: 20251114 MDTime: 92500010 ...
```

---

## 故障排查

### 问题 1: 找不到数据文件

**错误**:
```
❌ 加载委托文件失败: test_data/MD_ORDER_StockType_002603.SZ.csv
```

**解决**:
```bash
# 检查文件是否存在
ls -la test_data/

# 确保文件名正确
# 确保路径相对于可执行文件
```

### 问题 2: 订单簿异常

**警告**:
```
⚠️  WARNING: 买一价 >= 卖一价！订单簿状态异常！
```

**可能原因**:
1. 数据质量问题（交易所原始数据有问题）
2. 订单簿实现 bug
3. 撤单处理逻辑错误

**排查步骤**:
1. 记录异常出现时的 ApplSeqNum
2. 检查该序号前后的数据
3. 使用 Python 脚本验证相同数据

### 问题 3: 编译失败

**错误**: 找不到头文件

**解决**:
```bash
# 检查头文件路径
ls -la include/strategy_engine.h
ls -la include/backtest_adapter.h

# 重新运行编译脚本
./build_test.sh
```

---

## 高级用法

### 1. 测试多个股票

修改 `test_strategy_simple.cpp`:
```cpp
// 注册多个策略实例
SimpleTestStrategy strategy1("Test_002603", 10000);
SimpleTestStrategy strategy2("Test_600000", 10000);

engine.register_strategy("002603.SZ", &strategy1);
engine.register_strategy("600000.SH", &strategy2);
```

### 2. 自定义验证逻辑

在 `on_order` 或 `on_transaction` 中添加：
```cpp
void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
    order_count_++;

    // 自定义检查
    if (order.orderprice > 200000) {  // 超过 20 元
        std::cerr << "异常高价: " << order.orderprice / 10000.0 << std::endl;
    }

    // ... 其他逻辑
}
```

### 3. 性能基准测试

```bash
# 编译 Release 版本
g++ -std=c++17 -O3 -march=native -DNDEBUG \
    -I./include -I./src \
    test_strategy_simple.cpp \
    src/FastOrderBook.cpp src/Order.cpp src/Trade.cpp \
    src/PriceLevel.cpp src/OrderBook.cpp \
    -lpthread -o test_strategy_simple_release

# 运行性能测试（无输出）
./test_strategy_simple_release --sample 1000000
```

---

## 相关文档

- [测试指南](doc/testing_guide.md) - 详细测试文档
- [架构文档](doc/architecture_refactoring.md) - 系统架构
- [性能优化](doc/performance_optimizations.md) - 优化技术

---

**更新时间**: 2026-01-12
**维护者**: Screwdriver Trading System Team
