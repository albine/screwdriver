#ifndef STRATEGY_ENGINE_H
#define STRATEGY_ENGINE_H

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <unordered_map>
#include <memory>
#include <variant>
#include <array>
#include "concurrentqueue.h"
#include "market_data_structs.h"
#include "strategy_base.h"

// ==========================================
// 分支预测优化宏
// ==========================================
#if defined(__GNUC__) || defined(__clang__)
    #define MD_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define MD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define MD_LIKELY(x)   (x)
    #define MD_UNLIKELY(x) (x)
#endif

// ==========================================
// 市场数据消息类型
// ==========================================
using MarketMessage = std::variant<MDStockStruct, MDOrderStruct, MDTransactionStruct>;

// ==========================================
// Symbol Hash 函数
// ==========================================
inline int stock_id_fast(const char* symbol, int shard_count) {
    uint64_t hash = 0;
    for (const char* p = symbol; *p && (p - symbol) < 40; ++p) {
        hash = hash * 31 + static_cast<unsigned char>(*p);
    }
    return static_cast<int>(hash % shard_count);
}

// ==========================================
// 策略引擎 - 基于 Moodycamel 的分片架构
// ==========================================
class StrategyEngine {
public:
    static const int SHARD_COUNT = 4;

    // 调试计数器
    mutable std::atomic<uint64_t> enqueue_order_count_{0};
    mutable std::atomic<uint64_t> enqueue_txn_count_{0};

private:
    std::vector<std::unique_ptr<moodycamel::ConcurrentQueue<MarketMessage>>> queues_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};
    std::atomic<bool> stopped_{false};

    // 策略所有权管理：Engine 拥有所有策略
    std::unordered_map<std::string, std::unique_ptr<Strategy>> owned_strategies_;

    // 策略注册表：[shard_id][symbol] -> 策略列表（快速查找用的裸指针）
    using StrategyMap = std::unordered_map<std::string, std::vector<Strategy*>>;
    std::vector<StrategyMap> registry_;

    // 共享的 thread_local token 数组（修复消息乱序bug）
    // 关键：所有 on_market_* 方法必须共享同一个 token 数组，
    // 否则同一线程的不同 token 会导致消息乱序！
    std::array<std::unique_ptr<moodycamel::ProducerToken>, SHARD_COUNT>& get_producer_tokens() {
        static thread_local std::array<std::unique_ptr<moodycamel::ProducerToken>, SHARD_COUNT> tokens;
        return tokens;
    }

public:
    StrategyEngine() : registry_(SHARD_COUNT) {
        for (int i = 0; i < SHARD_COUNT; ++i) {
            queues_.push_back(std::make_unique<moodycamel::ConcurrentQueue<MarketMessage>>(65536));
        }
    }

    ~StrategyEngine() {
        stop();
    }

    // 辅助函数：从 MarketMessage 中提取 symbol
    static const char* get_symbol(const MarketMessage& msg) {
        return std::visit([](auto&& data) -> const char* {
            return data.htscsecurityid;
        }, msg);
    }

    // 注册策略（转移所有权）
    void register_strategy(const std::string& symbol, std::unique_ptr<Strategy> strat) {
        if (!strat) {
            return;  // 忽略空指针
        }

        int shard_id = get_shard_id(symbol);
        Strategy* raw_ptr = strat.get();

        // 1. 保存所有权
        owned_strategies_[symbol] = std::move(strat);

        // 2. 注册裸指针到分片（用于快速查找）
        registry_[shard_id][symbol].push_back(raw_ptr);
    }

    // 运行时移除策略（可选功能）
    void unregister_strategy(const std::string& symbol) {
        int shard_id = get_shard_id(symbol);

        // 1. 从 registry 移除
        auto& strat_map = registry_[shard_id];
        auto it = strat_map.find(symbol);
        if (it != strat_map.end()) {
            strat_map.erase(it);
        }

        // 2. 释放所有权（自动调用析构）
        owned_strategies_.erase(symbol);
    }

    // 获取策略数量
    size_t strategy_count() const {
        return owned_strategies_.size();
    }

    // 启动引擎
    void start() {
        // 调用所有策略的 on_start()
        for (int i = 0; i < SHARD_COUNT; ++i) {
            for (auto& kv : registry_[i]) {
                for (auto* strat : kv.second) {
                    strat->on_start();
                }
            }
        }

        // 启动 worker 线程
        for (int i = 0; i < SHARD_COUNT; ++i) {
            workers_.emplace_back([this, i]() {
                this->worker_loop(i);
            });
        }
    }

    // 停止引擎
    void stop() {
        // 防止重复调用
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            return;  // 已经停止过了
        }

        running_ = false;
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }

        // 调用所有策略的 on_stop()
        for (int i = 0; i < SHARD_COUNT; ++i) {
            for (auto& kv : registry_[i]) {
                for (auto* strat : kv.second) {
                    strat->on_stop();
                }
            }
        }
    }

    // ==========================================
    // 市场数据接口 - 供外部适配器调用
    // ==========================================
    // 优化要点：
    // 1. static thread_local Token 数组（每个线程每个 shard 独立）
    // 2. MD_UNLIKELY 分支预测优化（初始化路径标记为冷路径）
    // 3. std::in_place_type 就地构造 variant（避免临时对象）
    void on_market_tick(const MDStockStruct& stock) {
        int shard_id = get_shard_id(stock.htscsecurityid);
        auto* q = queues_[shard_id].get();

        auto& tokens = get_producer_tokens();

        // 懒加载初始化（只有第一次为 true，标记为 unlikely）
        if (MD_UNLIKELY(!tokens[shard_id])) {
            tokens[shard_id] = std::make_unique<moodycamel::ProducerToken>(*q);
        }

        // 就地构造 variant 并入队（避免先构造 MDStockStruct 再 move）
        q->enqueue(*tokens[shard_id], MarketMessage{std::in_place_type<MDStockStruct>, stock});
    }

    void on_market_order(const MDOrderStruct& order) {
        enqueue_order_count_++;  // 调试计数

        int shard_id = get_shard_id(order.htscsecurityid);
        auto* q = queues_[shard_id].get();

        auto& tokens = get_producer_tokens();

        if (MD_UNLIKELY(!tokens[shard_id])) {
            tokens[shard_id] = std::make_unique<moodycamel::ProducerToken>(*q);
        }

        q->enqueue(*tokens[shard_id], MarketMessage{std::in_place_type<MDOrderStruct>, order});
    }

    void on_market_transaction(const MDTransactionStruct& transaction) {
        enqueue_txn_count_++;  // 调试计数

        int shard_id = get_shard_id(transaction.htscsecurityid);
        auto* q = queues_[shard_id].get();

        auto& tokens = get_producer_tokens();

        if (MD_UNLIKELY(!tokens[shard_id])) {
            tokens[shard_id] = std::make_unique<moodycamel::ProducerToken>(*q);
        }

        q->enqueue(*tokens[shard_id], MarketMessage{std::in_place_type<MDTransactionStruct>, transaction});
    }

private:
    int get_shard_id(const char* symbol) {
        return stock_id_fast(symbol, SHARD_COUNT);
    }

    int get_shard_id(const std::string& symbol) {
        return stock_id_fast(symbol.c_str(), SHARD_COUNT);
    }

    // Worker 线程循环
    void worker_loop(int shard_id) {
        auto* q = queues_[shard_id].get();
        auto& local_strat_map = registry_[shard_id];

        // 线程局部对象池
        ObjectPool<OrderNode> local_pool(200000);

        // 本地订单簿管理
        std::unordered_map<std::string, std::unique_ptr<FastOrderBook>> books;

        moodycamel::ConsumerToken c_token(*q);
        MarketMessage msg;

        while (running_) {
            if (q->try_dequeue(c_token, msg)) {
                const char* symbol = get_symbol(msg);
                std::string sym_str(symbol);

                // 处理消息并通知策略
                auto strat_it = local_strat_map.find(sym_str);
                bool has_strats = (strat_it != local_strat_map.end());

                std::visit([&](auto&& data) {
                    using T = std::decay_t<decltype(data)>;

                    if constexpr (std::is_same_v<T, MDStockStruct>) {
                        // 如果还没有 OrderBook，使用 MDStockStruct 的 minpx 和 maxpx 创建
                        auto book_it = books.find(sym_str);
                        if (MD_UNLIKELY(book_it == books.end())) {
                            // 确保价格范围有效
                            uint32_t min_price = static_cast<uint32_t>(data.minpx);
                            uint32_t max_price = static_cast<uint32_t>(data.maxpx);

                            if (min_price > 0 && max_price > min_price) {
                                auto new_book = std::make_unique<FastOrderBook>(
                                    0,
                                    local_pool,
                                    min_price,
                                    max_price
                                );
                                books[sym_str] = std::move(new_book);
                            }
                            // 如果价格范围无效，暂不创建 OrderBook，等待有效数据
                        }

                        if (has_strats) {
                            for (auto* strat : strat_it->second) strat->on_tick(data);
                        }
                    }
                    else if constexpr (std::is_same_v<T, MDOrderStruct>) {
                        auto book_it = books.find(sym_str);
                        if (MD_LIKELY(book_it != books.end())) {
                            book_it->second->on_order(data);
                            if (has_strats) {
                                for (auto* strat : strat_it->second) strat->on_order(data, *book_it->second);
                            }
                        }
                        // 如果没有 OrderBook，忽略此消息（应该先收到 MDStockStruct）
                    }
                    else if constexpr (std::is_same_v<T, MDTransactionStruct>) {
                        auto book_it = books.find(sym_str);
                        if (MD_LIKELY(book_it != books.end())) {
                            book_it->second->on_transaction(data);
                            if (has_strats) {
                                for (auto* strat : strat_it->second) strat->on_transaction(data, *book_it->second);
                            }
                        }
                        // 如果没有 OrderBook，忽略此消息（应该先收到 MDStockStruct）
                    }
                }, msg);

            } else {
                std::this_thread::yield();
            }
        }
    }
};

#endif // STRATEGY_ENGINE_H
