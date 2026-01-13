# 策略测试指南

## 概述

本文档介绍如何使用测试策略验证交易系统的正确性和性能。

---

## 测试策略说明

### 1. SimpleTestStrategy - 简单测试策略

**功能**：
- 监控订单簿状态
- 验证买卖价格合法性（买一价 < 卖一价）
- 统计处理的消息数量
- 计算系统吞吐量

**关键检查**：
- ✅ 买一价 < 卖一价（如果存在）
- ✅ 订单簿状态合法性
- ✅ 定期打印快照

**采样模式**：
- 每 N 条委托消息打印订单簿状态
- 每 10N 条成交消息验证订单簿合法性

---

### 2. TestStrategy - 完整测试策略

**功能**：
- 包含 SimpleTestStrategy 的所有功能
- 支持 Tick 数据监控
- 打印最终订单簿十档行情
- 详细的时间统计

**适用场景**：
- 功能验证测试
- 回归测试
- 数据质量检查

---

### 3. PerformanceStrategy - 性能测试策略

**功能**：
- 纯粹的吞吐量测试（无复杂逻辑）
- 计算平均延迟
- 压力测试

**适用场景**：
- 性能基准测试
- 容量规划
- 优化效果验证

---

## 编译和运行

### 1. 编译

```bash
cd /home/pc/screwdriver

# 使用编译脚本
./build_test.sh

# 或手动编译
g++ -std=c++17 -O3 -march=native \
    -I./include -I./src \
    test_strategy_simple.cpp \
    src/FastOrderBook.cpp src/Order.cpp src/Trade.cpp \
    src/PriceLevel.cpp src/OrderBook.cpp \
    -lpthread -o test_strategy_simple
```

### 2. 运行测试

**基本用法**：
```bash
# 默认参数（采样间隔 10000）
./test_strategy_simple

# 自定义采样间隔
./test_strategy_simple --sample 1000

# 查看帮助
./test_strategy_simple --help
```

**预期输出**：
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

[Order #10000] ApplSeqNum: 123456 | BestBid: 9.78 | BestAsk: 9.79
[Order #20000] ApplSeqNum: 234567 | BestBid: 9.77 | BestAsk: 9.80
✅ Validation passed at Txn #10000
✅ Validation passed at Txn #20000
...

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

## 测试数据准备

### 1. 数据格式要求

**委托文件** (CSV 格式):
```
HTSCSecurityID: "002603.SZ" MDDate: 20251114 MDTime: 92500010 ...
```

**成交文件** (CSV 格式):
```
HTSCSecurityID: "002603.SZ" MDDate: 20251114 MDTime: 92500015 ...
```

### 2. 数据位置

```
screwdriver/
└── test_data/
    ├── MD_ORDER_StockType_002603.SZ.csv
    └── MD_TRANSACTION_StockType_002603.SZ.csv
```

### 3. 获取测试数据

- 使用现有的 `xlsx_to_replayer_format.py` 转换 Excel 数据
- 或使用真实的市场数据（需要从 MDC Gateway 保存）

---

## 验证方法

### 1. 功能验证

**检查点 1：订单簿合法性**
```
✅ PASS: 买一价 < 卖一价
❌ FAIL: ⚠️ WARNING: Bid >= Ask!  (需要调查)
```

**检查点 2：消息处理数量**
```
委托消息数 + 成交消息数 = 总事件数
```

**检查点 3：无错误输出**
```
❌ ERROR: Invalid book at Txn #12345  (严重问题)
```

---

### 2. 性能验证

**指标对比**：

| 指标 | 目标值 | 实际值 | 状态 |
|------|--------|--------|------|
| 吞吐量 | > 200K msg/s | 353K msg/s | ✅ |
| 平均延迟 | < 5 μs/msg | 2.8 μs/msg | ✅ |
| 总耗时 | < 500 ms | 245 ms | ✅ |

**性能基准** (002603.SZ, 86708 events):
- **Debug 模式**: 800-1000 ms
- **Release 模式**: 200-300 ms
- **优化后**: < 250 ms

---

### 3. 压力测试

**修改采样间隔进行压力测试**：
```bash
# 低频采样（减少 I/O 开销）
./test_strategy_simple --sample 100000

# 无采样（最大吞吐量）
# 修改代码，注释掉所有 std::cout
```

**预期结果**：
- 吞吐量应该提升 20-30%
- CPU 利用率应该保持在 70% 以下

---

## 常见问题排查

### 问题 1：编译失败

**错误**：`strategy_engine.h: No such file or directory`

**解决方案**：
```bash
# 检查 include 路径
ls -la include/strategy_engine.h

# 确保所有头文件都在 include/ 目录下
```

---

### 问题 2：找不到数据文件

**错误**：`❌ 加载委托文件失败: test_data/MD_ORDER_...`

**解决方案**：
```bash
# 检查文件是否存在
ls -la test_data/

# 确保文件名正确
# 确保路径正确（相对于可执行文件）
```

---

### 问题 3：订单簿状态异常

**错误**：`⚠️ WARNING: Bid >= Ask!`

**可能原因**：
1. 数据质量问题（交易所数据本身有问题）
2. 订单簿实现 bug
3. 撤单处理逻辑错误

**排查步骤**：
1. 定位出错的 ApplSeqNum
2. 检查该序号前后的委托和成交数据
3. 使用 Python 脚本验证相同数据

---

### 问题 4：性能不达标

**现象**：吞吐量 < 200K msg/s

**排查步骤**：
1. 检查编译选项：`-O3 -march=native`
2. 检查采样间隔：是否频繁打印
3. 检查 CPU 频率：`cat /proc/cpuinfo | grep MHz`
4. 使用 perf 分析瓶颈：`perf record -g ./test_strategy_simple`

---

## 扩展测试

### 1. 多股票测试

修改 `test_strategy_simple.cpp`：
```cpp
// 注册多个股票
engine.register_strategy("002603.SZ", &strategy1);
engine.register_strategy("600000.SH", &strategy2);
engine.register_strategy("000001.SZ", &strategy3);

// 加载多个数据文件
adapter.load_order_file("test_data/MD_ORDER_002603.SZ.csv");
adapter.load_order_file("test_data/MD_ORDER_600000.SH.csv");
adapter.load_order_file("test_data/MD_ORDER_000001.SZ.csv");
```

---

### 2. 自定义验证逻辑

继承 `Strategy` 实现自己的验证策略：
```cpp
class MyValidationStrategy : public Strategy {
public:
    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        // 自定义验证逻辑
        if (order.orderprice > 100000) {
            std::cerr << "异常高价: " << order.orderprice / 10000.0 << std::endl;
        }

        // 检查订单簿深度
        auto bids = book.get_bid_levels(10);
        if (bids.size() < 5) {
            std::cout << "⚠️ 买盘深度不足 5 档" << std::endl;
        }
    }
};
```

---

### 3. 集成到 CI/CD

**GitHub Actions 示例**：
```yaml
name: OrderBook Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2

      - name: Build
        run: ./build_test.sh

      - name: Run Tests
        run: |
          ./test_strategy_simple --sample 50000
          # 检查退出码
          if [ $? -eq 0 ]; then
            echo "✅ Tests passed"
          else
            echo "❌ Tests failed"
            exit 1
          fi

      - name: Performance Check
        run: |
          # 运行性能测试，检查吞吐量是否达标
          ./test_strategy_simple --sample 100000 > perf.log
          grep "吞吐量" perf.log | awk '{if ($2 < 200000) exit 1}'
```

---

## 参考文档

- [架构重构文档](architecture_refactoring.md) - 系统架构说明
- [性能优化文档](performance_optimizations.md) - 性能优化技术
- [OrderBook 测试指南](orderbook_testing_guide.md) - OrderBook 单元测试

---

**文档版本**: v1.0
**最后更新**: 2026-01-12
**维护者**: Screwdriver Trading System Team
