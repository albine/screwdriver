#ifndef STRATEGY_ENGINE_H
#define STRATEGY_ENGINE_H

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <memory>
#include <variant>
#include <array>
#include <shared_mutex>
#include <type_traits>
#include "concurrentqueue.h"
#include "market_data_structs.h"
#include "strategy_base.h"
#include "strategy_ids.h"
#include "logger.h"

#define LOG_MODULE MOD_ENGINE

// 前向声明
class StrategyContext;

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
// 控制消息类型
// ==========================================
struct ControlMessage {
    enum class Type : uint8_t {
        ENABLE,
        DISABLE
    };
    Type type;
    uint32_t unique_id;      // 唯一 ID (stock_code << 9 | exchange << 8 | strategy_id)
    char symbol[48];         // 保留用于 shard 路由
    uint32_t param = 0;      // 通用参数（如 target_price，价格*10000的整数格式）

    static ControlMessage enable(const std::string& sym, const std::string& strat_name, uint32_t param_value = 0) {
        ControlMessage msg;
        msg.type = Type::ENABLE;
        msg.unique_id = StrategyIds::make_unique_id(sym, strat_name);
        strncpy(msg.symbol, sym.c_str(), sizeof(msg.symbol) - 1);
        msg.symbol[sizeof(msg.symbol) - 1] = '\0';
        msg.param = param_value;
        return msg;
    }

    static ControlMessage disable(const std::string& sym, const std::string& strat_name) {
        ControlMessage msg;
        msg.type = Type::DISABLE;
        msg.unique_id = StrategyIds::make_unique_id(sym, strat_name);
        strncpy(msg.symbol, sym.c_str(), sizeof(msg.symbol) - 1);
        msg.symbol[sizeof(msg.symbol) - 1] = '\0';
        msg.param = 0;
        return msg;
    }
};

// ==========================================
// 市场数据消息类型
// ==========================================
using MarketMessage = std::variant<MDStockStruct, MDOrderStruct, MDTransactionStruct, MDOrderbookStruct, ControlMessage>;

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

    // 生成策略唯一 key (数字 ID，高性能)
    static uint32_t make_strategy_key(const std::string& symbol, const std::string& strategy_name) {
        return StrategyIds::make_unique_id(symbol, strategy_name);
    }

    // 从 symbol 和 strategy_type_id 生成 key
    static uint32_t make_strategy_key(const std::string& symbol, uint8_t strategy_type_id) {
        return StrategyIds::make_unique_id(symbol, strategy_type_id);
    }

private:
    std::vector<std::unique_ptr<moodycamel::ConcurrentQueue<MarketMessage>>> queues_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};
    std::atomic<bool> stopped_{false};

    // 策略所有权管理：key = unique_id (uint32_t)
    std::unordered_map<uint32_t, std::unique_ptr<Strategy>> owned_strategies_;

    // 策略注册表：[shard_id][symbol] -> 策略列表（快速查找用的裸指针）
    // 注意：这里的 key 仍然是 symbol（用于市场数据路由）
    using StrategyMap = std::unordered_map<std::string, std::vector<Strategy*>>;
    std::vector<StrategyMap> registry_;

    // 读写锁保护 registry_（支持运行时动态添加/删除策略）
    mutable std::shared_mutex registry_mutex_;

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
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, ControlMessage>) {
                return data.symbol;
            } else {
                return data.htscsecurityid;
            }
        }, msg);
    }

    // 注册策略（转移所有权）- 启动前调用
    // symbol: 股票代码（如 "600000.SH"）
    // strat: 策略实例（需要设置 strat->name 和 strat->strategy_type_id）
    void register_strategy(const std::string& symbol, std::unique_ptr<Strategy> strat) {
        if (!strat) {
            return;  // 忽略空指针
        }

        int shard_id = get_shard_id(symbol);
        Strategy* raw_ptr = strat.get();
        uint32_t key = make_strategy_key(symbol, strat->strategy_type_id);

        // 1. 保存所有权（使用数字 key）
        owned_strategies_[key] = std::move(strat);

        // 2. 注册裸指针到分片（按 symbol 路由市场数据）
        registry_[shard_id][symbol].push_back(raw_ptr);
    }

    // ==========================================
    // 运行时动态策略管理（引擎启动后调用）
    // ==========================================

    // 运行时注册策略（线程安全）
    // symbol: 股票代码
    // strat: 策略实例（需要设置 strat->name 和 strat->strategy_type_id）
    bool register_strategy_runtime(const std::string& symbol, std::unique_ptr<Strategy> strat) {
        if (!strat) {
            return false;
        }

        uint32_t key = make_strategy_key(symbol, strat->strategy_type_id);

        // 写锁
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        // 检查是否已存在（使用数字 key）
        if (owned_strategies_.find(key) != owned_strategies_.end()) {
            return false;  // 已存在
        }

        int shard_id = get_shard_id(symbol);
        Strategy* raw_ptr = strat.get();

        // 保存所有权（使用数字 key）
        owned_strategies_[key] = std::move(strat);

        // 注册裸指针到分片（按 symbol 路由）
        registry_[shard_id][symbol].push_back(raw_ptr);

        // 释放锁后调用 on_start()
        lock.unlock();
        raw_ptr->on_start();

        return true;
    }

    // 检查策略是否存在（使用数字 key）
    bool has_strategy(const std::string& symbol, const std::string& strategy_name) const {
        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
        uint32_t key = make_strategy_key(symbol, strategy_name);
        return owned_strategies_.find(key) != owned_strategies_.end();
    }

    // 检查某个 symbol 是否有任意策略
    bool has_any_strategy(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
        int shard_id = stock_id_fast(symbol.c_str(), SHARD_COUNT);
        auto it = registry_[shard_id].find(symbol);
        return it != registry_[shard_id].end() && !it->second.empty();
    }

    // 获取策略列表（返回 "symbol:strategy_name" 格式）
    std::vector<std::string> get_strategy_list() const {
        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
        std::vector<std::string> result;
        result.reserve(owned_strategies_.size());
        for (const auto& kv : owned_strategies_) {
            // 从数字 key 解析出 symbol 和 strategy_name
            uint32_t stock_code;
            bool is_shanghai;
            uint8_t strategy_id;
            StrategyIds::parse_unique_id(kv.first, stock_code, is_shanghai, strategy_id);
            std::string symbol = StrategyIds::unique_id_to_symbol(kv.first);
            const char* strat_name = StrategyIds::id_to_name(strategy_id);
            result.push_back(symbol + ":" + strat_name);
        }
        return result;
    }

    // 运行时移除策略（线程安全）
    void unregister_strategy(const std::string& symbol, const std::string& strategy_name) {
        uint32_t key = make_strategy_key(symbol, strategy_name);

        std::unique_lock<std::shared_mutex> lock(registry_mutex_);

        auto it = owned_strategies_.find(key);
        if (it == owned_strategies_.end()) {
            return;  // 不存在
        }

        // 先调用 on_stop()（需要释放锁避免死锁）
        Strategy* strat = it->second.get();
        lock.unlock();
        if (strat) {
            strat->on_stop();
        }
        lock.lock();

        // 从 registry 移除（需要找到并删除对应的策略指针）
        int shard_id = get_shard_id(symbol);
        auto& strat_vec = registry_[shard_id][symbol];
        strat_vec.erase(
            std::remove(strat_vec.begin(), strat_vec.end(), strat),
            strat_vec.end()
        );

        // 如果该 symbol 没有策略了，删除整个 entry
        if (strat_vec.empty()) {
            registry_[shard_id].erase(symbol);
        }

        // 释放所有权
        owned_strategies_.erase(key);
    }

    // 获取策略数量
    size_t strategy_count() const {
        return owned_strategies_.size();
    }

    // 为所有策略设置上下文
    void set_context_for_all_strategies(StrategyContext* ctx) {
        std::shared_lock<std::shared_mutex> lock(registry_mutex_);
        for (auto& kv : owned_strategies_) {
            kv.second->set_context(ctx);
        }
    }

    // ==========================================
    // 策略启用/禁用控制
    // ==========================================

    // 发送控制消息到对应的 Worker 队列
    void send_control_message(const ControlMessage& ctrl) {
        int shard_id = get_shard_id(ctrl.symbol);
        auto* q = queues_[shard_id].get();
        q->enqueue(MarketMessage{std::in_place_type<ControlMessage>, ctrl});
    }

    // 启用策略（可选传入 param，如 target_price）
    bool enable_strategy(const std::string& symbol, const std::string& strategy_name, uint32_t param = 0) {
        uint32_t key = make_strategy_key(symbol, strategy_name);
        {
            std::shared_lock<std::shared_mutex> lock(registry_mutex_);
            if (owned_strategies_.find(key) == owned_strategies_.end()) {
                return false;
            }
        }
        send_control_message(ControlMessage::enable(symbol, strategy_name, param));
        return true;
    }

    // 禁用策略
    bool disable_strategy(const std::string& symbol, const std::string& strategy_name) {
        uint32_t key = make_strategy_key(symbol, strategy_name);
        {
            std::shared_lock<std::shared_mutex> lock(registry_mutex_);
            if (owned_strategies_.find(key) == owned_strategies_.end()) {
                return false;
            }
        }
        send_control_message(ControlMessage::disable(symbol, strategy_name));
        return true;
    }

    // 启动引擎
    void start() {
        // 调用所有策略的 on_start() 并统计数量
        std::map<uint8_t, int> strategy_counts;  // key: strategy_type_id
        int total = 0;

        for (int i = 0; i < SHARD_COUNT; ++i) {
            for (auto& kv : registry_[i]) {
                for (auto* strat : kv.second) {
                    strat->on_start();
                    strategy_counts[strat->strategy_type_id]++;
                    total++;
                }
            }
        }

        // 打印汇总日志
        LOG_M_INFO("Initialized {} strategies:", total);
        for (const auto& [id, count] : strategy_counts) {
            LOG_M_INFO("  - {}: {} instances", StrategyIds::id_to_name(id), count);
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

    void on_market_orderbook_snapshot(const MDOrderbookStruct& snapshot) {
        int shard_id = get_shard_id(snapshot.htscsecurityid);
        auto* q = queues_[shard_id].get();

        auto& tokens = get_producer_tokens();

        if (MD_UNLIKELY(!tokens[shard_id])) {
            tokens[shard_id] = std::make_unique<moodycamel::ProducerToken>(*q);
        }

        q->enqueue(*tokens[shard_id], MarketMessage{std::in_place_type<MDOrderbookStruct>, snapshot});
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

                // 读锁查找策略（复制指针列表，快速释放锁）
                std::vector<Strategy*> strats;
                {
                    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
                    auto& local_strat_map = registry_[shard_id];
                    auto strat_it = local_strat_map.find(sym_str);
                    if (strat_it != local_strat_map.end()) {
                        strats = strat_it->second;  // 复制指针列表
                    }
                }
                bool has_strats = !strats.empty();

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
                            for (auto* strat : strats) strat->on_tick(data);
                        }
                    }
                    else if constexpr (std::is_same_v<T, MDOrderStruct>) {
                        auto book_it = books.find(sym_str);
                        if (MD_LIKELY(book_it != books.end())) {
                            book_it->second->on_order(data);
                            if (has_strats) {
                                for (auto* strat : strats) strat->on_order(data, *book_it->second);
                            }
                        }
                        // 如果没有 OrderBook，忽略此消息（应该先收到 MDStockStruct）
                    }
                    else if constexpr (std::is_same_v<T, MDTransactionStruct>) {
                        auto book_it = books.find(sym_str);
                        if (MD_LIKELY(book_it != books.end())) {
                            book_it->second->on_transaction(data);
                            if (has_strats) {
                                for (auto* strat : strats) strat->on_transaction(data, *book_it->second);
                            }
                        }
                        // 如果没有 OrderBook，忽略此消息（应该先收到 MDStockStruct）
                    }
                    else if constexpr (std::is_same_v<T, MDOrderbookStruct>) {
                        // OrderBook 快照不需要本地 OrderBook，直接调用策略回调
                        if (has_strats) {
                            for (auto* strat : strats) strat->on_orderbook_snapshot(data);
                        }
                    }
                    else if constexpr (std::is_same_v<T, ControlMessage>) {
                        // 处理控制消息 - 调用策略的 on_control_message()
                        if (has_strats) {
                            for (auto* strat : strats) {
                                strat->on_control_message(data);
                            }
                        }
                    }
                }, msg);

            } else {
                std::this_thread::yield();
            }
        }
    }
};

#undef LOG_MODULE
#endif // STRATEGY_ENGINE_H
