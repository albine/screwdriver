#ifndef ZMQ_CLIENT_H
#define ZMQ_CLIENT_H

#include <zmq.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include "logger.h"
#include "zmq_protocol.h"
#include "strategy_engine.h"
#include "strategy_factory.h"
#include "utils/symbol_utils.h"

// 定义本文件的日志模块
#define LOG_MODULE "ZMQ"

using json = nlohmann::json;

class ZmqClient {
public:
    explicit ZmqClient(const std::string& endpoint1 = "tcp://localhost:13380",
                       const std::string& endpoint2 = "tcp://localhost:13381")
        : running_(false), context_(nullptr), engine_(nullptr) {
        // 初始化两个连接配置
        dealers_[0].endpoint = endpoint1;
        dealers_[0].index = 1;
        dealers_[1].endpoint = endpoint2;
        dealers_[1].index = 2;
    }

    // 设置策略引擎引用（用于动态策略管理）
    void set_engine(StrategyEngine* engine) {
        engine_ = engine;
    }

    ~ZmqClient() {
        stop();
    }

    bool start() {
        if (running_) return true;

        // 捕获 ZMQ 初始化错误，避免程序崩溃
        try {
            context_ = zmq_ctx_new();
        } catch (...) {
            LOG_M_ERROR("ZMQ context creation threw exception");
            return false;
        }

        if (!context_) {
            LOG_M_ERROR("Failed to create ZMQ context: {}", zmq_strerror(errno));
            return false;
        }

        running_ = true;

        // 启动第一个 DEALER（主连接）
        if (!start_dealer(dealers_[0])) {
            running_ = false;
            zmq_ctx_destroy(context_);
            context_ = nullptr;
            return false;
        }

        // 启动心跳线程（只对主连接）
        heartbeat_thread_ = std::thread(&ZmqClient::heartbeat_loop, this);

        // 启动第二个 DEALER
        if (!start_dealer(dealers_[1])) {
            LOG_M_WARNING("DEALER2 failed to start, continuing without it");
        }

        LOG_M_INFO("ZMQ client started with heartbeat every 60s");
        return true;
    }

    void stop() {
        if (!running_) return;

        running_ = false;

        // 等待所有线程结束
        for (auto& dealer : dealers_) {
            if (dealer.recv_thread.joinable()) {
                dealer.recv_thread.join();
            }
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }

        // 关闭所有 socket
        for (auto& dealer : dealers_) {
            if (dealer.socket) {
                zmq_close(dealer.socket);
                dealer.socket = nullptr;
            }
        }

        if (context_) {
            zmq_ctx_destroy(context_);
            context_ = nullptr;
        }

        LOG_M_INFO("ZMQ client stopped");
    }

    // 发送消息（通过主连接）
    bool send(const std::string& req_id, const json& payload) {
        return send_via(dealers_[0], req_id, payload);
    }

    bool is_running() const { return running_; }

    // 发送下单消息到 ROUTER（股票代码去掉后缀）
    // 根据 symbol 查找对应的 dealer 发送（确保使用 add_hot_stock_ht 时的同一通道）
    void send_place_order(const std::string& symbol, double price) {
        static int order_seq = 0;

        // 查找该 symbol 应该使用的通道
        int dealer_index = 1;  // 默认使用 DEALER1
        {
            std::shared_lock lock(symbol_dealer_mutex_);
            auto it = symbol_dealer_map_.find(symbol);
            if (it != symbol_dealer_map_.end()) {
                dealer_index = it->second;
            }
        }

        // 使用对应的 dealer 发送（index 1/2 对应数组下标 0/1）
        DealerConnection& dealer = dealers_[dealer_index - 1];

        std::string symbol_no_suffix = symbol_utils::strip_suffix(symbol);
        json payload;
        payload["action"] = zmq_ht_proto::Action::PLACE_ORDER;
        payload["symbol"] = symbol_no_suffix;
        payload["price"] = price;
        payload["side"] = "buy";
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string req_id = "order_" + std::to_string(++order_seq);
        send_via(dealer, req_id, payload);
        LOG_M_INFO("Sent place_order: symbol={}, price={}, side=buy, dealer={}", symbol_no_suffix, price, dealer_index);
    }

private:
    // DEALER 连接封装
    struct DealerConnection {
        std::string endpoint;
        std::string identity;
        void* socket = nullptr;
        std::mutex send_mutex;
        std::thread recv_thread;
        int index = 0;  // 用于日志区分（1 或 2）
    };

    // 启动单个 DEALER 连接
    bool start_dealer(DealerConnection& dealer) {
        dealer.socket = zmq_socket(context_, ZMQ_DEALER);
        if (!dealer.socket) {
            LOG_M_ERROR("Failed to create DEALER{} socket: {}", dealer.index, zmq_strerror(errno));
            return false;
        }

        // 设置 identity
        dealer.identity = "trading-engine-" + std::to_string(dealer.index) + "-" + std::to_string(getpid());
        zmq_setsockopt(dealer.socket, ZMQ_IDENTITY, dealer.identity.c_str(), dealer.identity.size());

        // 设置接收超时
        int recv_timeout = 1000;
        zmq_setsockopt(dealer.socket, ZMQ_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // 连接到 ROUTER 服务端
        if (zmq_connect(dealer.socket, dealer.endpoint.c_str()) != 0) {
            LOG_M_ERROR("Failed to connect DEALER{} to {}: {}", dealer.index, dealer.endpoint, zmq_strerror(errno));
            zmq_close(dealer.socket);
            dealer.socket = nullptr;
            return false;
        }

        LOG_M_INFO("ZMQ DEALER{} connected to {}", dealer.index, dealer.endpoint);

        // 发送注册消息
        send_register(dealer);

        // 启动接收线程
        dealer.recv_thread = std::thread(&ZmqClient::recv_loop, this, std::ref(dealer));
        return true;
    }

    // 通用发送方法
    bool send_via(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        if (!dealer.socket || !running_) return false;

        json msg;
        msg["req_id"] = req_id;
        msg["payload"] = payload;
        std::string data = msg.dump();

        std::lock_guard<std::mutex> lock(dealer.send_mutex);
        int rc = zmq_send(dealer.socket, data.c_str(), data.size(), 0);
        if (rc >= 0) {
            LOG_M_INFO("Sent message via DEALER{}: req_id={}, size={}", dealer.index, req_id, data.size());
        } else {
            LOG_M_ERROR("Failed to send message via DEALER{}: {}", dealer.index, zmq_strerror(errno));
        }
        return rc >= 0;
    }

    // 发送注册消息
    void send_register(DealerConnection& dealer) {
        json payload;
        payload["action"] = zmq_ht_proto::Action::REGISTER;
        payload["client"] = dealer.identity;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        send_via(dealer, "register_0", payload);
        LOG_M_INFO("Sent register message to ROUTER (DEALER{})", dealer.index);
    }

    // 发送响应消息
    void send_response(DealerConnection& dealer, const std::string& req_id, const std::string& action, const json& data) {
        json payload;
        payload["action"] = action;
        payload["data"] = data;
        payload["data"]["req_id"] = req_id;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string response_req_id = action + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        send_via(dealer, response_req_id, payload);
    }

    // 接收循环
    void recv_loop(DealerConnection& dealer) {
        char buffer[65536];

        while (running_) {
            int rc = zmq_recv(dealer.socket, buffer, sizeof(buffer) - 1, 0);
            if (rc < 0) {
                if (errno == EAGAIN) continue;
                if (!running_) break;
                continue;
            }

            if (rc > 0) {
                buffer[rc] = '\0';
                process_message(dealer, buffer, rc);
            }
        }
    }

    void heartbeat_loop() {
        int heartbeat_count = 0;
        while (running_) {
            // 每60秒发送一次心跳
            for (int i = 0; i < 600 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) break;

            json payload;
            payload["action"] = zmq_ht_proto::Action::HEARTBEAT;
            payload["client"] = dealers_[0].identity;
            payload["seq"] = ++heartbeat_count;
            payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            std::string req_id = "hb_" + std::to_string(heartbeat_count);
            send_via(dealers_[0], req_id, payload);
        }
    }

    void process_message(DealerConnection& dealer, const char* data, size_t len) {
        try {
            json msg = json::parse(std::string(data, len));

            std::string req_id;
            json payload;

            if (msg.contains("req_id")) {
                req_id = msg["req_id"].get<std::string>();
            }
            if (msg.contains("payload")) {
                payload = msg["payload"];
            }

            handle_command(dealer, req_id, payload);

        } catch (const json::exception& e) {
            LOG_M_ERROR("Failed to parse ZMQ message (DEALER{}): {}", dealer.index, e.what());
        }
    }

    // 处理外部命令
    void handle_command(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        std::string action = zmq_ht_proto::get_action(payload);

        if (action.empty()) {
            LOG_M_WARNING("Message missing 'action' field (DEALER{}), req_id={}, payload={}",
                         dealer.index, req_id, payload.dump());
            return;
        }

        // 心跳消息静默忽略
        if (action == zmq_ht_proto::Action::HEARTBEAT) {
            return;
        }

        LOG_M_INFO("Received command (DEALER{}): action={}, req_id={}", dealer.index, action, req_id);

        if (action == "ack") {
            std::string status = payload.value("status", "");
            std::string message = payload.value("message", "");
            LOG_M_INFO("DEALER{} registered: status={}, message={}", dealer.index, status, message);

        } else if (action == "add_hot_stock" || action == "add_hot_stock_ht") {
            handle_add_hot_stock_ht(dealer, req_id, payload);

        } else if (action == "remove_hot_stock" || action == "remove_hot_stock_ht") {
            handle_remove_hot_stock_ht(dealer, req_id, payload);

        } else if (action == zmq_ht_proto::Action::ADD_STRATEGY) {
            handle_add_strategy(dealer, req_id, payload);

        } else if (action == zmq_ht_proto::Action::REMOVE_STRATEGY) {
            handle_remove_strategy(dealer, req_id, payload);

        } else if (action == zmq_ht_proto::Action::LIST_STRATEGIES) {
            handle_list_strategies(dealer, req_id);

        } else if (action == zmq_ht_proto::Action::ENABLE_STRATEGY) {
            handle_enable_strategy(dealer, req_id, payload);

        } else if (action == zmq_ht_proto::Action::DISABLE_STRATEGY) {
            handle_disable_strategy(dealer, req_id, payload);

        } else {
            LOG_M_WARNING("Unknown action (DEALER{}): {}", dealer.index, action);
        }
    }

    // ==========================================
    // 策略管理命令处理
    // ==========================================

    // 添加策略的结果
    struct AddStrategyResult {
        bool success = false;
        std::string error_msg;
        std::string available_strategies;  // 当策略类型不存在时填充
        std::string symbol_internal;       // 带后缀的 symbol
        std::string strategy_name;
        std::string params_str;
    };

    // 核心添加策略逻辑（不发送响应）
    AddStrategyResult try_add_strategy(const std::string& symbol_raw,
                                       const std::string& strategy_name,
                                       const json& params) {
        AddStrategyResult result;
        result.strategy_name = strategy_name;

        if (!engine_) {
            result.error_msg = "Engine not initialized";
            return result;
        }

        if (symbol_raw.empty()) {
            result.error_msg = "Missing symbol";
            return result;
        }

        result.symbol_internal = symbol_utils::normalize_symbol(symbol_raw);

        if (engine_->has_strategy(result.symbol_internal, strategy_name)) {
            result.error_msg = "Strategy already exists: " + result.symbol_internal + ":" + strategy_name;
            return result;
        }

        auto& factory = StrategyFactory::instance();
        if (!factory.has_strategy(strategy_name)) {
            auto available = factory.get_registered_strategies();
            for (const auto& s : available) result.available_strategies += s + " ";
            result.error_msg = "Unknown strategy: " + strategy_name;
            return result;
        }

        result.params_str = parse_strategy_params(params);

        try {
            auto strategy = factory.create(strategy_name, result.symbol_internal, result.params_str);
            if (engine_->register_strategy_runtime(result.symbol_internal, std::move(strategy))) {
                result.success = true;
            } else {
                result.error_msg = "Failed to register strategy";
            }
        } catch (const std::exception& e) {
            result.error_msg = e.what();
        }

        return result;
    }

    void handle_add_strategy(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        auto msg = zmq_ht_proto::AddStrategyMsg::from_json(payload);
        auto result = try_add_strategy(msg.symbol, msg.strategy_name, msg.params);

        if (result.success) {
            LOG_M_INFO("ADD_STRATEGY: added {}:{} (params={})", result.symbol_internal, result.strategy_name, result.params_str);
            send_response(dealer, req_id, "success", {
                {"message", "Strategy added"},
                {"symbol", result.symbol_internal},
                {"strategy", result.strategy_name},
                {"params", result.params_str}
            });
        } else {
            LOG_M_WARNING("ADD_STRATEGY: {}", result.error_msg);
            json err_data = {{"message", result.error_msg}};
            if (!result.available_strategies.empty()) {
                err_data["available"] = result.available_strategies;
            }
            send_response(dealer, req_id, "error", err_data);
        }
    }

    // 移除策略的结果
    struct RemoveStrategyResult {
        bool success = false;
        std::string error_msg;
        std::string symbol_internal;  // 带后缀的 symbol
        std::string strategy_name;
    };

    // 核心移除策略逻辑（不发送响应）
    RemoveStrategyResult try_remove_strategy(const std::string& symbol_raw,
                                             const std::string& strategy_name) {
        RemoveStrategyResult result;
        result.strategy_name = strategy_name;

        if (!engine_) {
            result.error_msg = "Engine not initialized";
            return result;
        }

        if (symbol_raw.empty()) {
            result.error_msg = "Missing symbol";
            return result;
        }

        if (strategy_name.empty()) {
            result.error_msg = "Missing strategy name";
            return result;
        }

        result.symbol_internal = symbol_utils::normalize_symbol(symbol_raw);

        if (!engine_->has_strategy(result.symbol_internal, strategy_name)) {
            result.error_msg = "Strategy not found: " + result.symbol_internal + ":" + strategy_name;
            return result;
        }

        engine_->unregister_strategy(result.symbol_internal, strategy_name);
        result.success = true;
        return result;
    }

    void handle_remove_strategy(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        auto msg = zmq_ht_proto::RemoveStrategyMsg::from_json(payload);
        auto result = try_remove_strategy(msg.symbol, msg.strategy_name);

        if (result.success) {
            LOG_M_INFO("REMOVE_STRATEGY: removed {}:{}", result.symbol_internal, result.strategy_name);
            send_response(dealer, req_id, "success", {
                {"message", "Strategy removed"},
                {"symbol", result.symbol_internal},
                {"strategy", result.strategy_name}
            });
        } else {
            LOG_M_WARNING("REMOVE_STRATEGY: {}", result.error_msg);
            send_response(dealer, req_id, "error", {{"message", result.error_msg}});
        }
    }

    void handle_list_strategies(DealerConnection& dealer, const std::string& req_id) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot list strategies");
            send_response(dealer, req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto list = engine_->get_strategy_list();
        json strategies = json::array();
        for (const auto& s : list) {
            strategies.push_back(s);
        }

        LOG_M_INFO("LIST_STRATEGIES: {} strategies", list.size());
        send_response(dealer, req_id, "success", {
            {"count", list.size()},
            {"strategies", strategies}
        });
    }

    void handle_enable_strategy(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot enable strategy");
            send_response(dealer, req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            LOG_M_WARNING("ENABLE_STRATEGY: missing symbol");
            send_response(dealer, req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        if (strategy_name.empty()) {
            LOG_M_WARNING("ENABLE_STRATEGY: missing strategy name");
            send_response(dealer, req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        symbol = symbol_utils::normalize_symbol(symbol);

        if (engine_->enable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("ENABLE_STRATEGY: enabled {}:{}", symbol, strategy_name);
            send_response(dealer, req_id, "success", {
                {"message", "Strategy enabled"},
                {"symbol", symbol},
                {"strategy", strategy_name}
            });
        } else {
            LOG_M_WARNING("ENABLE_STRATEGY: strategy not found {}:{}", symbol, strategy_name);
            send_response(dealer, req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    void handle_disable_strategy(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot disable strategy");
            send_response(dealer, req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            LOG_M_WARNING("DISABLE_STRATEGY: missing symbol");
            send_response(dealer, req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        if (strategy_name.empty()) {
            LOG_M_WARNING("DISABLE_STRATEGY: missing strategy name");
            send_response(dealer, req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        symbol = symbol_utils::normalize_symbol(symbol);

        if (engine_->disable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("DISABLE_STRATEGY: disabled {}:{}", symbol, strategy_name);
            send_response(dealer, req_id, "success", {
                {"message", "Strategy disabled"},
                {"symbol", symbol},
                {"strategy", strategy_name}
            });
        } else {
            LOG_M_WARNING("DISABLE_STRATEGY: strategy not found {}:{}", symbol, strategy_name);
            send_response(dealer, req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    // ==========================================
    // add_hot_stock_ht / remove_hot_stock_ht 处理
    // ==========================================

    void handle_add_hot_stock_ht(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        // 消息格式: { "action": "add_hot_stock", "symbol": "600000", "strategy": "BreakoutPriceVolumeStrategy", "params": {"breakout_price": 12.50} }
        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "BreakoutPriceVolumeStrategy");
        json params = payload.contains("params") ? payload["params"] : json{};

        auto result = try_add_strategy(symbol, strategy_name, params);
        std::string symbol_resp = symbol_utils::strip_suffix(result.symbol_internal);

        if (result.success) {
            // 记录 symbol 来源通道（用于下单时路由）
            {
                std::unique_lock lock(symbol_dealer_mutex_);
                symbol_dealer_map_[result.symbol_internal] = dealer.index;
            }
            LOG_M_INFO("add_hot_stock: added {}:{}, params={}, dealer={}", result.symbol_internal, result.strategy_name, result.params_str, dealer.index);
            send_response(dealer, req_id, "add_hot_stock_resp", {
                {"success", true},
                {"symbol", symbol_resp},
                {"strategy", result.strategy_name},
                {"params", result.params_str}
            });
        } else {
            LOG_M_WARNING("add_hot_stock: {}", result.error_msg);
            json err_data = {{"success", false}, {"msg", result.error_msg}};
            if (!result.available_strategies.empty()) {
                err_data["available"] = result.available_strategies;
            }
            send_response(dealer, req_id, "add_hot_stock_resp", err_data);
        }
    }

    void handle_remove_hot_stock_ht(DealerConnection& dealer, const std::string& req_id, const json& payload) {
        // 消息格式: { "action": "remove_hot_stock", "symbol": "600000" }
        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "BreakoutPriceVolumeStrategy");

        auto result = try_remove_strategy(symbol, strategy_name);
        std::string symbol_resp = symbol_utils::strip_suffix(result.symbol_internal);

        if (result.success) {
            // 移除 symbol 来源记录
            {
                std::unique_lock lock(symbol_dealer_mutex_);
                symbol_dealer_map_.erase(result.symbol_internal);
            }
            LOG_M_INFO("remove_hot_stock: removed {}:{}", result.symbol_internal, result.strategy_name);
            send_response(dealer, req_id, "remove_hot_stock_resp", {
                {"success", true},
                {"symbol", symbol_resp},
                {"strategy", result.strategy_name}
            });
        } else {
            LOG_M_WARNING("remove_hot_stock: {}", result.error_msg);
            send_response(dealer, req_id, "remove_hot_stock_resp", {
                {"success", false},
                {"msg", result.error_msg}
            });
        }
    }

    // ==========================================
    // 辅助方法
    // ==========================================

    // 解析策略参数：支持字符串、数值或对象格式
    std::string parse_strategy_params(const json& params) {
        if (params.is_string()) {
            return params.get<std::string>();
        } else if (params.is_number()) {
            return std::to_string(params.get<int64_t>());
        } else if (params.is_object() && !params.empty() && params.contains("breakout_price")) {
            auto& bp = params["breakout_price"];
            if (bp.is_number()) {
                return std::to_string(bp.get<int64_t>());
            } else if (bp.is_string()) {
                return bp.get<std::string>();
            }
        }
        return "";
    }

    std::array<DealerConnection, 2> dealers_;
    std::atomic<bool> running_;
    void* context_;
    std::thread heartbeat_thread_;
    StrategyEngine* engine_;

    // symbol → dealer 映射（用于按通道路由下单消息）
    std::unordered_map<std::string, int> symbol_dealer_map_;  // symbol(带后缀) → dealer index
    mutable std::shared_mutex symbol_dealer_mutex_;           // 读写锁
};

#undef LOG_MODULE

#endif // ZMQ_CLIENT_H
