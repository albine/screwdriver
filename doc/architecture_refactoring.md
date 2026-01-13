# 架构重构文档：实盘引擎与策略引擎分离

## 1. 概述

本次重构将原有的单体交易系统拆分为**策略引擎**和**数据适配器**两个独立模块，实现了实盘交易和策略回测的统一框架。

### 1.1 重构目标

- **关注点分离**：将市场数据接入、策略执行、订单簿管理等功能解耦
- **代码复用**：策略引擎在实盘和回测环境中完全复用
- **易于扩展**：支持多种数据源（实盘、历史回放、模拟数据等）
- **架构清晰**：明确的接口定义和职责划分

### 1.2 重构前后对比

**重构前**：
```
main.cpp (单体架构)
├── ShardedEngine (策略引擎)
├── HistoryDataReplayer (回测数据)
└── 策略逻辑
```

**重构后**：
```
策略引擎层 (Strategy Engine)
    ↓
适配器层 (Adapters)
    ├── LiveMarketAdapter (实盘)
    └── BacktestAdapter (回测)
    ↓
数据源层 (Data Sources)
    ├── InsightHandle/MDC Gateway (实盘数据)
    └── HistoryDataReplayer (历史数据)
```

---

## 2. 核心组件

### 2.1 策略引擎 (StrategyEngine)

**文件位置**：`include/strategy_engine.h`

**职责**：
- 管理多个交易策略的注册和调度
- 维护每个股票的实时订单簿（FastOrderBook）
- 使用分片架构（4个Shard）实现高性能并发处理
- 提供统一的市场数据接口

**核心特性**：
- **无锁队列**：基于 moodycamel::ConcurrentQueue 实现高性能消息传递
- **线程局部存储**：每个Shard独立的对象池和订单簿
- **Symbol Hash分片**：根据证券代码自动路由到对应Shard
- **零拷贝设计**：使用 std::variant 避免动态内存分配

**关键接口**：
```cpp
class StrategyEngine {
public:
    // 注册策略到指定证券
    void register_strategy(const std::string& symbol, Strategy* strat);

    // 启动/停止引擎
    void start();
    void stop();

    // 市场数据接口（由适配器调用）
    void on_market_tick(const MDStockStruct& stock);
    void on_market_order(const MDOrderStruct& order);
    void on_market_transaction(const MDTransactionStruct& transaction);
};
```

**策略基类**：
```cpp
class Strategy {
public:
    virtual void on_tick(const MDStockStruct& stock) {}
    virtual void on_order(const MDOrderStruct& order, const FastOrderBook& book) {}
    virtual void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) {}

    std::string name;
};
```

---

### 2.2 实盘数据适配器 (LiveMarketAdapter)

**文件位置**：`include/live_market_adapter.h`

**职责**：
- 继承自 `fastfish/src/InsightHandle.h`
- 接收 MDC Gateway 推送的实盘市场数据（protobuf格式）
- 将数据转换为策略引擎使用的 C++ 结构体
- 无缝对接 `OnMarketData` 回调

**数据转换流程**：
```
MDC Gateway
    ↓
protobuf::MarketData
    ↓
LiveMarketAdapter::OnMarketData()
    ↓
convert_to_stock/order/transaction()
    ↓
MDStockStruct / MDOrderStruct / MDTransactionStruct
    ↓
StrategyEngine::on_market_*()
```

**关键代码**：
```cpp
class LiveMarketAdapter : public InsightHandle {
private:
    StrategyEngine* engine_;

public:
    void OnMarketData(const com::htsc::mdc::insight::model::MarketData& data) override {
        // 调用父类保存数据（可选）
        InsightHandle::OnMarketData(data);

        // 转换并转发到策略引擎
        switch (data.marketdatatype()) {
            case MD_TICK:
                engine_->on_market_tick(convert_to_stock(data.mdstock()));
                break;
            case MD_ORDER:
                engine_->on_market_order(convert_to_order(data.mdorder()));
                break;
            case MD_TRANSACTION:
                engine_->on_market_transaction(convert_to_transaction(data.mdtransaction()));
                break;
        }
    }
};
```

---

### 2.3 回测数据适配器 (BacktestAdapter)

**文件位置**：`include/backtest_adapter.h`

**职责**：
- 封装 `HistoryDataReplayer`
- 从 CSV 文件加载历史市场数据
- 按时间顺序回放订单和成交数据
- 将数据转发到策略引擎

**数据回放流程**：
```
CSV 文件
    ↓
HistoryDataReplayer::load_*_file()
    ↓
按时间排序、分片
    ↓
多线程并发回放
    ↓
BacktestAdapter callback
    ↓
StrategyEngine::on_market_*()
```

**关键代码**：
```cpp
class BacktestAdapter {
private:
    HistoryDataReplayer replayer_;
    StrategyEngine* engine_;

public:
    BacktestAdapter(StrategyEngine* engine, int shard_count = 4)
        : replayer_(shard_count), engine_(engine) {

        // 设置回调，将数据转发到引擎
        replayer_.set_order_callback([this](const MDOrderStruct& order) {
            engine_->on_market_order(order);
        });

        replayer_.set_transaction_callback([this](const MDTransactionStruct& txn) {
            engine_->on_market_transaction(txn);
        });
    }

    bool load_order_file(const std::string& filepath);
    bool load_transaction_file(const std::string& filepath);
    void replay();
};
```

---

## 3. 运行模式

### 3.1 回测模式 (Backtest Mode)

**用途**：策略开发、参数优化、历史回测

**启动命令**：
```bash
./main backtest
```

**工作流程**：
```cpp
void run_backtest_mode() {
    // 1. 初始化日志
    auto* logger = hft::logger::init(log_config);

    // 2. 创建策略引擎
    StrategyEngine engine;
    PrintStrategy strategy("BacktestStrat");

    // 3. 注册策略
    engine.register_strategy("002603.SZ", &strategy);

    // 4. 启动引擎
    engine.start();

    // 5. 创建回测适配器
    BacktestAdapter adapter(&engine, StrategyEngine::SHARD_COUNT);

    // 6. 加载历史数据
    adapter.load_order_file("test_data/MD_ORDER_StockType_002603.SZ.csv");
    adapter.load_transaction_file("test_data/MD_TRANSACTION_StockType_002603.SZ.csv");

    // 7. 回放数据
    adapter.replay();

    // 8. 等待处理完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 9. 停止引擎
    engine.stop();
}
```

**特点**：
- 快速回放：不受实时限制，可全速处理历史数据
- 可重复性：相同输入产生相同结果
- 日志输出：`logs/backtest.log`

---

### 3.2 实盘模式 (Live Mode)

**用途**：实盘交易、实时监控、纸上交易

**启动命令**：
```bash
./main live
```

**工作流程**：
```cpp
void run_live_mode() {
    // 1. 初始化日志
    auto* logger = hft::logger::init(log_config);

    // 2. 创建策略引擎
    StrategyEngine engine;
    PrintStrategy strategy("LiveStrat");

    // 3. 注册策略
    engine.register_strategy("002603.SZ", &strategy);

    // 4. 启动引擎
    engine.start();

    // 5. 创建实盘适配器
    LiveMarketAdapter adapter("market_data", &engine);

    // 6. 创建并配置 MDC 客户端
    ClientInterface* client = MdcClientFactory::create();
    client->SetMessageHandle(&adapter);

    // 7. 登录 Gateway
    LoginField login;
    login.Username = "your_username";
    login.Password = "your_password";
    login.MarketGatewayAddress = "tcp://gateway_address:port";
    client->Login(login);

    // 8. 订阅市场数据
    MDSubscribeRequest request;
    request.set_taskid("task_001");
    request.add_htscsecurityidlist("002603.SZ");
    request.add_marketdatatypelist(MD_TICK);
    request.add_marketdatatypelist(MD_ORDER);
    request.add_marketdatatypelist(MD_TRANSACTION);
    client->Subscribe(request);

    // 9. 持续运行
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
```

**特点**：
- 实时数据：接收交易所实时推送
- 连接管理：自动重连、登录状态监控
- 日志输出：`logs/live_trading.log`

---

## 4. 文件结构

```
screwdriver/
├── include/
│   ├── strategy_engine.h          # 策略引擎（新增）
│   ├── live_market_adapter.h      # 实盘适配器（新增）
│   ├── backtest_adapter.h         # 回测适配器（新增）
│   ├── history_data_replayer.h    # 历史数据回放器（已有）
│   ├── market_data_structs.h      # 市场数据结构体（已有）
│   ├── logger.h                   # 日志系统（已有）
│   └── concurrentqueue.h          # 无锁队列（已有）
│
├── src/
│   ├── main.cpp                   # 主程序（重构）
│   ├── FastOrderBook.cpp          # 快速订单簿（已有）
│   ├── Order.cpp                  # 订单实现（已有）
│   ├── Trade.cpp                  # 成交实现（已有）
│   └── ...
│
├── fastfish/
│   ├── include/mdc_gateway_client/ # MDC Gateway 客户端库
│   └── src/InsightHandle.h        # 实盘数据处理器
│
├── doc/
│   └── architecture_refactoring.md # 本文档（新增）
│
└── test_data/
    ├── MD_ORDER_StockType_002603.SZ.csv
    └── MD_TRANSACTION_StockType_002603.SZ.csv
```

---

## 5. 关键设计决策

### 5.1 为什么使用适配器模式？

**问题**：实盘数据（protobuf）和历史数据（CSV）格式不同，但策略逻辑应该保持一致。

**解决方案**：引入适配器层，统一数据接口：
```
不同数据源 → 适配器 → 统一的 MDStruct → 策略引擎
```

**优势**：
- 策略代码无需关心数据来源
- 可以轻松添加新的数据源（如模拟器、WebSocket等）
- 实盘和回测结果一致性高

---

### 5.2 为什么保留 InsightHandle 继承？

**原因**：
1. **兼容性**：fastfish MDC Gateway 要求消息处理器继承 `MessageHandle`
2. **功能复用**：`InsightHandle` 已实现数据保存、错误处理等功能
3. **最小改动**：只需重写 `OnMarketData` 方法即可对接

**实现**：
```cpp
class LiveMarketAdapter : public InsightHandle {
    void OnMarketData(...) override {
        InsightHandle::OnMarketData(...);  // 可选：保留原有功能
        engine_->on_market_*(...);          // 新增：转发到策略引擎
    }
};
```

---

### 5.3 为什么引擎和适配器分离？

**单一职责原则**：
- **StrategyEngine**：负责策略调度、订单簿管理、高性能处理
- **Adapter**：负责数据转换、协议对接、外部系统交互

**可测试性**：
- 引擎可以独立测试（注入mock数据）
- 适配器可以独立测试（验证数据转换）

**复用性**：
- 同一个引擎实例可以对接多个数据源
- 不同交易系统可以复用相同的适配器

---

## 6. 性能考虑

### 6.1 零拷贝设计

**问题**：市场数据频繁拷贝会影响性能。

**解决方案**：
- 使用 `std::variant<MDStockStruct, MDOrderStruct, MDTransactionStruct>` 避免虚函数和堆分配
- 消息在队列中直接存储结构体，无需额外内存分配

### 6.2 分片架构

**问题**：单线程处理所有市场数据会成为瓶颈。

**解决方案**：
- 4个Shard，每个Shard独立处理一组证券
- 每个Shard有独立的：队列、线程、对象池、订单簿
- 证券通过哈希函数路由到固定Shard，保证顺序性

### 6.3 对象池

**问题**：频繁的 new/delete 会导致内存碎片和性能下降。

**解决方案**：
- 每个Shard维护线程局部对象池（200,000个OrderNode预分配）
- 订单节点从池中获取和归还，避免系统内存分配

---

## 7. 扩展指南

### 7.1 添加新的策略

```cpp
// 1. 继承 Strategy 基类
class MyStrategy : public Strategy {
public:
    MyStrategy() { name = "MyStrategy"; }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        // 实现你的策略逻辑
        auto best_bid = book.get_best_bid();
        auto best_ask = book.get_best_ask();

        if (best_bid && best_ask) {
            double spread = (*best_ask - *best_bid) / 10000.0;  // 转换为价格
            if (spread < 0.01) {
                // 触发交易信号
            }
        }
    }
};

// 2. 在 main.cpp 中注册
MyStrategy strategy;
engine.register_strategy("600000.SH", &strategy);
```

### 7.2 添加新的数据源

```cpp
// 例如：WebSocket 数据源适配器
class WebSocketAdapter {
private:
    StrategyEngine* engine_;
    WebSocketClient ws_client_;

public:
    WebSocketAdapter(StrategyEngine* engine) : engine_(engine) {
        ws_client_.on_message([this](const std::string& msg) {
            // 解析 WebSocket 消息
            auto order = parse_order_from_json(msg);
            // 转发到引擎
            engine_->on_market_order(order);
        });
    }

    void connect(const std::string& url) {
        ws_client_.connect(url);
    }
};
```

### 7.3 添加新的数据类型

如果需要支持新的市场数据类型（如盘口快照、K线等）：

1. 在 `market_data_structs.h` 中定义新结构体
2. 在 `MarketMessage` variant 中添加新类型
3. 在 `Strategy` 基类中添加对应回调
4. 在适配器中实现数据转换

---

## 8. 注意事项

### 8.1 线程安全

- **策略实例**：不要在多个Shard间共享策略实例
- **全局变量**：避免在策略中使用全局可变状态
- **日志系统**：logger 是线程安全的，可以放心使用

### 8.2 内存管理

- **策略对象**：由用户管理生命周期，引擎只持有指针
- **订单簿**：引擎内部管理，使用 `unique_ptr` 自动释放
- **对象池**：随线程结束自动销毁

### 8.3 实盘配置

在生产环境使用实盘模式前，务必配置：
- 正确的 Gateway 地址
- 有效的用户名和密码
- 订阅列表（避免订阅不需要的数据）
- 日志级别和存储路径

---

## 9. 常见问题

**Q: 如何同时订阅多个股票？**
```cpp
std::vector<std::string> symbols = {"002603.SZ", "600000.SH", "000001.SZ"};
for (const auto& symbol : symbols) {
    engine.register_strategy(symbol, &strategy);
    request.add_htscsecurityidlist(symbol);
}
```

**Q: 回测模式下如何加速？**
- 减少不必要的日志输出
- 关闭控制台输出：`log_config.console_output = false`
- 增加Shard数量（需要修改 `SHARD_COUNT`）

**Q: 如何保存策略信号？**
- 在策略的 `on_*` 方法中调用日志宏
- 或实现自定义的信号记录器

**Q: 实盘模式下如何优雅退出？**
- 捕获 SIGINT 信号
- 调用 `engine.stop()` 等待队列清空
- 调用 `client->Logout()` 断开连接

---

## 10. 总结

本次重构成功实现了交易系统的模块化和解耦，主要成果：

✅ **策略引擎独立**：可在不同环境中复用
✅ **适配器模式**：统一了实盘和回测数据接口
✅ **高性能设计**：保留了原有的分片和无锁队列架构
✅ **易于扩展**：清晰的接口定义，方便添加新功能
✅ **代码清晰**：职责明确，降低维护成本

下一步可以考虑：
- 添加更多数据源适配器（模拟交易、纸上交易等）
- 实现策略参数配置系统
- 添加性能监控和统计模块
- 实现交易信号的持久化存储

---

**文档版本**：v1.0
**最后更新**：2026-01-12
**作者**：Screwdriver Trading System Team
