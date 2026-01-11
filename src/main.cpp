#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <functional>
#include <unordered_map>
#include <cstring>
#include <memory> // for unique_ptr
#include <variant> // for std::variant

// 引入 moodycamel 头文件
// 实际编译时请确保该文件在 include 路径下
#include "concurrentqueue.h"

// 引入市场数据结构
#include "market_data_structs.h"
#include "history_data_replayer.h"
#include "../src/FastOrderBook.h"

// 引入日志模块
#include "logger.h" 

// ==========================================
// 1. 市场数据消息类型
// ==========================================
// 使用 variant 支持三种市场数据类型，同时保持全局顺序
using MarketMessage = std::variant<MDStockStruct, MDOrderStruct, MDTransactionStruct>;

// ==========================================
// 2. Symbol Hash 函数
// ==========================================
// 用户自定义的快速 hash 函数，用于 symbol 到 shard 的路由
inline int stock_id_fast(const char* symbol, int shard_count) {
    // 简单但高效的 hash 实现
    uint64_t hash = 0;
    for (const char* p = symbol; *p && (p - symbol) < 40; ++p) {
        hash = hash * 31 + static_cast<unsigned char>(*p);
    }
    return static_cast<int>(hash % shard_count);
}

// ==========================================
// 3. 策略基类
// ==========================================
class Strategy {
public:
    virtual ~Strategy() = default;

    // 行情数据回调
    virtual void on_tick(const MDStockStruct& stock) {}

    // 委托数据回调 - 接收 MDOrderStruct 和当前的 OrderBook
    virtual void on_order(const MDOrderStruct& order, const FastOrderBook& book) {}

    // 成交数据回调 - 接收 MDTransactionStruct 和当前的 OrderBook
    virtual void on_transaction(const MDTransactionStruct& transaction, const FastOrderBook& book) {}

    std::string name;
};

class PrintStrategy : public Strategy {
public:
    PrintStrategy(std::string n) { name = n; }

    void on_tick(const MDStockStruct& stock) override {
        // Simple tick print
    }

    void on_order(const MDOrderStruct& order, const FastOrderBook& book) override {
        // 示例：每 1000 个订单打印一次最优报价
        if (order.applseqnum % 1000 == 0) {
             auto bid = book.get_best_bid();
             auto ask = book.get_best_ask();
             std::cout << "[Strat:" << name << "] OrderSeq:" << order.applseqnum 
                       << " BestBid:" << (bid ? std::to_string(*bid) : "None")
                       << " BestAsk:" << (ask ? std::to_string(*ask) : "None")
                       << std::endl;
        }
    }

    void on_transaction(const MDTransactionStruct& txn, const FastOrderBook& book) override {
        // Transaction logic
    }
};

// ==========================================
// 4. 基于 Moodycamel 的分片交易引擎
// ==========================================
class ShardedEngine {
public:
    // 假设我们将全市场切分为 4 个 Shard
    static const int SHARD_COUNT = 4;

private:
    std::vector<std::unique_ptr<moodycamel::ConcurrentQueue<MarketMessage>>> queues_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};

    // 策略注册表：[shard_id][symbol] -> 策略列表
    using StrategyMap = std::unordered_map<std::string, std::vector<Strategy*>>;
    std::vector<StrategyMap> registry_;

public:
    ShardedEngine() : registry_(SHARD_COUNT) {
        // 初始化队列
        for (int i = 0; i < SHARD_COUNT; ++i) {
            queues_.push_back(std::make_unique<moodycamel::ConcurrentQueue<MarketMessage>>(65536));
        }
    }

    // 辅助函数：从 MarketMessage 中提取 symbol
    static const char* get_symbol(const MarketMessage& msg) {
        return std::visit([](auto&& data) -> const char* {
            return data.htscsecurityid;  // 所有三种结构都有此字段
        }, msg);
    }

    // --- 初始化 ---
    void register_strategy(std::string symbol, Strategy* strat) {
        int shard_id = get_shard_id(symbol);
        registry_[shard_id][symbol].push_back(strat);
    }

    void start() {
        for (int i = 0; i < SHARD_COUNT; ++i) {
            workers_.emplace_back([this, i]() {
                this->worker_loop(i);
            });
        }
    }

    void stop() {
        running_ = false;
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // --- 生产端 (极度热路径) ---
    void on_market_tick(const MDStockStruct& stock) {
        int shard_id = get_shard_id(stock.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{stock});
    }

    void on_market_order(const MDOrderStruct& order) {
        int shard_id = get_shard_id(order.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{order});
    }

    void on_market_transaction(const MDTransactionStruct& transaction) {
        int shard_id = get_shard_id(transaction.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{transaction});
    }

private:
    int get_shard_id(const char* symbol) {
        return stock_id_fast(symbol, SHARD_COUNT);
    }

    int get_shard_id(const std::string& symbol) {
        return stock_id_fast(symbol.c_str(), SHARD_COUNT);
    }

    // --- 消费端 (Worker) ---
    void worker_loop(int shard_id) {
        auto* q = queues_[shard_id].get();
        auto& local_strat_map = registry_[shard_id];

        // 1. 线程局部对象池 (Thread Local Object Pool)
        // 每个 Shard 独享一个 Pool，无锁且高效
        ObjectPool<OrderNode> local_pool(200000); 

        // 2. 本地订单簿管理
        // Symbol -> OrderBook (unique_ptr to manage lifetime)
        std::unordered_map<std::string, std::unique_ptr<FastOrderBook>> books;

        moodycamel::ConsumerToken c_token(*q);
        MarketMessage msg;

        while (running_) {
            if (q->try_dequeue(c_token, msg)) {
                const char* symbol = get_symbol(msg);
                std::string sym_str(symbol);

                // 获取或创建 OrderBook
                FastOrderBook* book = nullptr;
                auto book_it = books.find(sym_str);
                if (book_it == books.end()) {
                    // 默认价格范围 0 - 1,000,000 (0.00 - 100.00)
                    // 实际生产中应根据昨收价或 Tick 动态调整
                    auto new_book = std::make_unique<FastOrderBook>(0, local_pool, 0, 1000000);
                    book = new_book.get();
                    books[sym_str] = std::move(new_book);
                } else {
                    book = book_it->second.get();
                }

                // 处理消息并通知策略
                auto strat_it = local_strat_map.find(sym_str);
                bool has_strats = (strat_it != local_strat_map.end());

                std::visit([&](auto&& data) {
                    using T = std::decay_t<decltype(data)>;

                    if constexpr (std::is_same_v<T, MDStockStruct>) {
                        // Tick 数据通常不直接更新逐笔重建的 OrderBook，除非用于校验
                        if (has_strats) {
                            for (auto* strat : strat_it->second) strat->on_tick(data);
                        }
                    } 
                    else if constexpr (std::is_same_v<T, MDOrderStruct>) {
                        book->on_order(data);
                        if (has_strats) {
                            for (auto* strat : strat_it->second) strat->on_order(data, *book);
                        }
                    } 
                    else if constexpr (std::is_same_v<T, MDTransactionStruct>) {
                        book->on_transaction(data);
                        if (has_strats) {
                            for (auto* strat : strat_it->second) strat->on_transaction(data, *book);
                        }
                    }
                }, msg);

            } else {
                std::this_thread::yield();
            }
        }
    }
};

// ==========================================
// 5. 测试入口
// ==========================================
int main() {
    // 初始化日志系统
    hft::logger::LogConfig log_config;
    log_config.log_dir = "logs";
    log_config.log_file = "trading.log";
    log_config.console_output = true;
    log_config.use_rdtsc = true;
    auto* logger = hft::logger::init(log_config);

    ShardedEngine engine;
    PrintStrategy strategy("DemoStrat");

    // 注册策略
    std::string symbol = "002603.SZ";
    engine.register_strategy(symbol, &strategy);

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Starting engine...");
    engine.start();

    // 回放数据
    HistoryDataReplayer replayer(1);
    LOG_MODULE_INFO(logger, MOD_ENGINE, "Loading data...");

    // 注意：确保 test_data 目录下有对应文件
    replayer.load_order_file("test_data/MD_ORDER_StockType_002603.SZ.csv");
    replayer.load_transaction_file("test_data/MD_TRANSACTION_StockType_002603.SZ.csv");

    replayer.set_order_callback([&](const MDOrderStruct& order) {
        engine.on_market_order(order);
    });
    replayer.set_transaction_callback([&](const MDTransactionStruct& txn) {
        engine.on_market_transaction(txn);
    });

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Replaying {} events...", replayer.event_count());
    replayer.replay();

    // 等待队列排空 (简单的 sleep 演示，生产环境应用 latch 或 callback)
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_MODULE_INFO(logger, MOD_ENGINE, "Stopping engine...");
    engine.stop();

    // 关闭日志系统
    hft::logger::shutdown();
    return 0;
}