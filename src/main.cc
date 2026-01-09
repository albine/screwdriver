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
#include "include/market_data_structs.h" 

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

    // 行情数据回调 - 接收 MDStockStruct
    virtual void on_tick(const MDStockStruct& stock) {}

    // 委托数据回调 - 接收 MDOrderStruct
    virtual void on_order(const MDOrderStruct& order) {}

    // 成交数据回调 - 接收 MDTransactionStruct
    virtual void on_transaction(const MDTransactionStruct& transaction) {}

    std::string name;
};

class PrintStrategy : public Strategy {
public:
    PrintStrategy(std::string n) { name = n; }

    void on_tick(const MDStockStruct& stock) override {
        std::cout << "[Strat:" << name << "] TICK "
                  << stock.htscsecurityid << " @ " << stock.lastpx
                  << " vol:" << stock.totalvolumetrade
                  << " (Thread " << std::this_thread::get_id() << ")" << std::endl;
    }

    void on_order(const MDOrderStruct& order) override {
        std::cout << "[Strat:" << name << "] ORDER "
                  << order.htscsecurityid << " " << order.orderprice
                  << " x " << order.orderqty
                  << " (Thread " << std::this_thread::get_id() << ")" << std::endl;
    }

    void on_transaction(const MDTransactionStruct& txn) override {
        std::cout << "[Strat:" << name << "] TXN "
                  << txn.htscsecurityid << " " << txn.tradeprice
                  << " x " << txn.tradeqty
                  << " (Thread " << std::this_thread::get_id() << ")" << std::endl;
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
    // 使用 unique_ptr 存储队列
    // 1. ConcurrentQueue 不可移动/拷贝，必须存指针
    // 2. 指针指向堆内存，天然避免了数组中相邻对象的 False Sharing
    // 使用 MarketMessage variant 以保持全局顺序
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
            // 预估容量 65536，实际上 moodycamel 会动态扩容，但预设容量能减少初期分配
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
    // 行情回调：接收 MDStockStruct
    void on_market_tick(const MDStockStruct& stock) {
        int shard_id = get_shard_id(stock.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{stock});
    }

    // 委托回调：接收 MDOrderStruct
    void on_market_order(const MDOrderStruct& order) {
        int shard_id = get_shard_id(order.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{order});
    }

    // 成交回调：接收 MDTransactionStruct
    void on_market_transaction(const MDTransactionStruct& transaction) {
        int shard_id = get_shard_id(transaction.htscsecurityid);
        queues_[shard_id]->enqueue(MarketMessage{transaction});
    }

private:
    // 使用 stock_id_fast 进行 hash 路由
    int get_shard_id(const char* symbol) {
        return stock_id_fast(symbol, SHARD_COUNT);
    }

    // 重载：支持 std::string
    int get_shard_id(const std::string& symbol) {
        return stock_id_fast(symbol.c_str(), SHARD_COUNT);
    }

    // --- 消费端 (Worker) ---
    void worker_loop(int shard_id) {
        // 获取当前 Shard 对应的队列指针
        auto* q = queues_[shard_id].get();

        // 获取本地策略表
        auto& local_map = registry_[shard_id];

        // [关键优化] ConsumerToken
        // Moodycamel 允许消费者持有一个 Token，这样出队时不需要重复查找线程 ID 或锁状态
        // 对于单消费者模式 (MPSC)，这能显著提升 try_dequeue 性能
        moodycamel::ConsumerToken c_token(*q);

        MarketMessage msg;

        // 绑定 CPU 核心 (可选)
        // SetThreadAffinity(shard_id);

        while (running_) {
            // 尝试从队列获取消息
            if (q->try_dequeue(c_token, msg)) {
                // 提取 symbol
                const char* symbol = get_symbol(msg);

                // 查找策略
                auto it = local_map.find(symbol);
                if (it != local_map.end()) {
                    // 使用 std::visit 分发到对应的回调函数
                    std::visit([&](auto&& data) {
                        using T = std::decay_t<decltype(data)>;

                        for (auto* strat : it->second) {
                            if constexpr (std::is_same_v<T, MDStockStruct>) {
                                strat->on_tick(data);
                            } else if constexpr (std::is_same_v<T, MDOrderStruct>) {
                                strat->on_order(data);
                            } else if constexpr (std::is_same_v<T, MDTransactionStruct>) {
                                strat->on_transaction(data);
                            }
                        }
                    }, msg);
                }
            } else {
                // 队列空时的策略：
                // 1. _mm_pause(); // 极低延迟，100% CPU
                // 2. std::this_thread::yield(); // 平衡
                std::this_thread::yield();
            }
        }
    }
};

// ==========================================
// 5. 测试入口
// ==========================================
int main() {
    ShardedEngine engine;
    PrintStrategy s1("Trend_Follower");

    // 注册策略
    engine.register_strategy("600000.SH", &s1); // 上证指数
    engine.register_strategy("000001.SZ", &s1); // 深证成指

    engine.start();

    // 模拟 4 个并发的行情推送线程
    std::vector<std::thread> api_threads;
    for(int i=0; i<4; ++i) {
        api_threads.emplace_back([&engine, i](){
            for(int j=0; j<50; ++j) {
                // 模拟股票行情数据
                MDStockStruct stock{};
                std::strncpy(stock.htscsecurityid, "600000.SH", sizeof(stock.htscsecurityid) - 1);
                stock.mddate = 20260109;
                stock.mdtime = 93000 + j;
                stock.lastpx = 3000000 + j * 1000;  // 价格：30.00 + j * 0.01
                stock.totalvolumetrade = 1000000 + j * 100;
                engine.on_market_tick(stock);

                // 模拟委托数据
                MDOrderStruct order{};
                std::strncpy(order.htscsecurityid, "600000.SH", sizeof(order.htscsecurityid) - 1);
                order.mddate = 20260109;
                order.mdtime = 93000 + j;
                order.orderprice = 3000000 + j * 1000;
                order.orderqty = 100 + j;
                engine.on_market_order(order);

                // 模拟成交数据
                MDTransactionStruct txn{};
                std::strncpy(txn.htscsecurityid, "000001.SZ", sizeof(txn.htscsecurityid) - 1);
                txn.mddate = 20260109;
                txn.mdtime = 93000 + j;
                txn.tradeprice = 2000000 + j * 500;
                txn.tradeqty = 200 + j;
                txn.trademoney = txn.tradeprice * txn.tradeqty;
                engine.on_market_transaction(txn);

                std::this_thread::sleep_for(std::chrono::microseconds(100)); // 模拟高速行情
            }
        });
    }

    for(auto& t : api_threads) t.join();

    // 给一点时间让消费者处理完
    std::this_thread::sleep_for(std::chrono::seconds(1));
    engine.stop();

    std::cout << "\n测试完成！处理了股票行情、委托和成交三种数据类型。" << std::endl;

    return 0;
}