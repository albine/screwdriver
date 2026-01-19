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
        : endpoint_(endpoint1), endpoint2_(endpoint2),
          running_(false), context_(nullptr), socket_(nullptr),
          socket2_(nullptr), engine_(nullptr) {}

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
        // ZMQ 在某些环境下可能因 signaler 问题导致 abort
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

        socket_ = zmq_socket(context_, ZMQ_DEALER);
        if (!socket_) {
            LOG_M_ERROR("Failed to create ZMQ socket");
            zmq_ctx_destroy(context_);
            context_ = nullptr;
            return false;
        }

        // 设置socket身份标识（可选）
        std::string identity = "trading-engine-" + std::to_string(getpid());
        zmq_setsockopt(socket_, ZMQ_IDENTITY, identity.c_str(), identity.size());

        // 设置接收超时（毫秒）
        int recv_timeout = 1000;
        zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // 连接到 ROUTER 服务端
        if (zmq_connect(socket_, endpoint_.c_str()) != 0) {
            LOG_M_ERROR("Failed to connect to {}", endpoint_);
            zmq_close(socket_);
            zmq_ctx_destroy(context_);
            socket_ = nullptr;
            context_ = nullptr;
            return false;
        }

        LOG_M_INFO("ZMQ DEALER connected to {}", endpoint_);

        running_ = true;
        identity_ = identity;

        // 立即发送注册消息，让 ROUTER 知道我们的 identity
        send_register();

        recv_thread_ = std::thread(&ZmqClient::recv_loop, this);
        heartbeat_thread_ = std::thread(&ZmqClient::heartbeat_loop, this);

        // 启动第二个 DEALER（连接到 13381）
        if (!start_dealer2()) {
            LOG_M_WARNING("DEALER2 failed to start, continuing without it");
        }

        LOG_M_INFO("ZMQ client started with heartbeat every 60s");

        return true;
    }

    // 启动第二个 DEALER（连接到 13381 ROUTER）
    bool start_dealer2() {
        socket2_ = zmq_socket(context_, ZMQ_DEALER);
        if (!socket2_) {
            LOG_M_ERROR("Failed to create DEALER2 socket: {}", zmq_strerror(errno));
            return false;
        }

        // 设置 identity（区分两个 DEALER）
        std::string identity2 = "trading-engine-2-" + std::to_string(getpid());
        zmq_setsockopt(socket2_, ZMQ_IDENTITY, identity2.c_str(), identity2.size());
        identity2_ = identity2;

        // 设置接收超时
        int recv_timeout = 1000;
        zmq_setsockopt(socket2_, ZMQ_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // 连接到 ROUTER 服务端
        if (zmq_connect(socket2_, endpoint2_.c_str()) != 0) {
            LOG_M_ERROR("Failed to connect DEALER2 to {}: {}", endpoint2_, zmq_strerror(errno));
            zmq_close(socket2_);
            socket2_ = nullptr;
            return false;
        }

        LOG_M_INFO("ZMQ DEALER2 connected to {}", endpoint2_);

        // 发送注册消息
        send_register2();

        // 启动 DEALER2 接收线程
        recv_thread2_ = std::thread(&ZmqClient::dealer2_recv_loop, this);
        return true;
    }

    void stop() {
        if (!running_) return;

        running_ = false;
        if (recv_thread_.joinable()) {
            recv_thread_.join();
        }
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        if (recv_thread2_.joinable()) {
            recv_thread2_.join();
        }

        if (socket_) {
            zmq_close(socket_);
            socket_ = nullptr;
        }
        if (socket2_) {
            zmq_close(socket2_);
            socket2_ = nullptr;
        }
        if (context_) {
            zmq_ctx_destroy(context_);
            context_ = nullptr;
        }

        LOG_M_INFO("ZMQ client stopped");
    }

    // 发送消息（DEALER模式，直接发送，无需手动加空帧）
    // 线程安全：多个策略线程可能同时调用
    bool send(const std::string& req_id, const json& payload) {
        if (!socket_ || !running_) return false;

        json msg;
        msg["req_id"] = req_id;
        msg["payload"] = payload;
        std::string data = msg.dump();

        // 加锁保护 zmq_send（ZMQ socket 非线程安全）
        std::lock_guard<std::mutex> lock(send_mutex_);
        int rc = zmq_send(socket_, data.c_str(), data.size(), 0);
        if (rc >= 0) {
            LOG_M_INFO("Sent message: req_id={}, size={}", req_id, data.size());
        } else {
            LOG_M_ERROR("Failed to send message: {}", zmq_strerror(errno));
        }
        return rc >= 0;
    }

    bool is_running() const { return running_; }

    // 发送下单消息到 ROUTER（股票代码去掉后缀）
    void send_place_order(const std::string& symbol, double price) {
        static int order_seq = 0;
        std::string symbol_no_suffix = symbol_utils::strip_suffix(symbol);
        json payload;
        payload["action"] = zmq_ht_proto::Action::PLACE_ORDER;
        payload["symbol"] = symbol_no_suffix;
        payload["price"] = price;
        payload["side"] = "buy";
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string req_id = "order_" + std::to_string(++order_seq);
        send(req_id, payload);
        LOG_M_INFO("Sent place_order: symbol={}, price={}, side=buy", symbol_no_suffix, price);
    }

private:
    void recv_loop() {
        char buffer[65536];

        while (running_) {
            // DEALER 直接接收数据帧
            int rc = zmq_recv(socket_, buffer, sizeof(buffer) - 1, 0);
            if (rc < 0) {
                if (errno == EAGAIN) continue;  // 超时，继续循环
                break;
            }

            if (rc > 0) {
                buffer[rc] = '\0';
                process_message(buffer, rc);
            }
        }
    }

    void heartbeat_loop() {
        int heartbeat_count = 0;
        while (running_) {
            // 每10秒发送一次心跳
            for (int i = 0; i < 600 && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running_) break;

            json payload;
            payload["action"] = zmq_ht_proto::Action::HEARTBEAT;
            payload["client"] = identity_;
            payload["seq"] = ++heartbeat_count;
            payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            std::string req_id = "hb_" + std::to_string(heartbeat_count);
            send(req_id, payload);
        }
    }

    // DEALER2 接收循环（使用统一的 process_message 处理）
    void dealer2_recv_loop() {
        char buffer[65536];

        while (running_) {
            int rc = zmq_recv(socket2_, buffer, sizeof(buffer) - 1, 0);
            if (rc < 0) {
                if (errno == EAGAIN) continue;  // 超时，继续循环
                if (!running_) break;
                continue;
            }

            if (rc > 0) {
                buffer[rc] = '\0';
                // 使用与 recv_loop 相同的消息处理逻辑，但使用 socket2 发送响应
                process_message2(buffer, rc);
            }
        }
    }

    // 处理 DEALER2 收到的消息（与 process_message 相同，但响应通过 socket2 发送）
    void process_message2(const char* data, size_t len) {
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

            // 处理命令（使用 socket2 发送响应）
            handle_command2(req_id, payload);

        } catch (const json::exception& e) {
            LOG_M_ERROR("Failed to parse ZMQ DEALER2 message: {}", e.what());
        }
    }

    // 发送注册消息，让 ROUTER 知道我们的 identity
    void send_register() {
        json payload;
        payload["action"] = zmq_ht_proto::Action::REGISTER;
        payload["client"] = identity_;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        send("register_0", payload);
        LOG_M_INFO("Sent register message to ROUTER (endpoint1)");
    }

    // 发送注册消息到第二个 ROUTER
    void send_register2() {
        json payload;
        payload["action"] = zmq_ht_proto::Action::REGISTER;
        payload["client"] = identity2_;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        send2("register2_0", payload);
        LOG_M_INFO("Sent register message to ROUTER (endpoint2)");
    }

    // 在 socket2 上发送消息（用于响应 DEALER2 收到的请求）
    bool send2(const std::string& req_id, const json& payload) {
        if (!socket2_ || !running_) return false;

        json msg;
        msg["req_id"] = req_id;
        msg["payload"] = payload;
        std::string data = msg.dump();

        std::lock_guard<std::mutex> lock(send2_mutex_);
        int rc = zmq_send(socket2_, data.c_str(), data.size(), 0);
        if (rc >= 0) {
            LOG_M_INFO("Sent message via socket2: req_id={}, size={}", req_id, data.size());
        } else {
            LOG_M_ERROR("Failed to send message via socket2: {}", zmq_strerror(errno));
        }
        return rc >= 0;
    }

    void process_message(const char* data, size_t len) {
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

            // 处理命令
            handle_command(req_id, payload);

        } catch (const json::exception& e) {
            LOG_M_ERROR("Failed to parse ZMQ message: {}", e.what());
        }
    }

    // 处理外部命令
    void handle_command(const std::string& req_id, const json& payload) {
        std::string action = zmq_ht_proto::get_action(payload);

        if (action.empty()) {
            LOG_M_WARNING("Message missing 'action' field, req_id={}, payload={}", req_id, payload.dump());
            return;
        }

        // 心跳消息静默忽略
        if (action == zmq_ht_proto::Action::HEARTBEAT) {
            return;
        }

        LOG_M_INFO("Received command: action={}, req_id={}", action, req_id);

        if (action == "ack") {
            // 注册成功响应
            std::string status = payload.value("status", "");
            std::string message = payload.value("message", "");
            LOG_M_INFO("DEALER1 registered: status={}, message={}", status, message);

        } else if (action == "add_hot_stock_ht") {
            handle_add_hot_stock_ht(req_id, payload, false);

        } else if (action == "remove_hot_stock_ht") {
            handle_remove_hot_stock_ht(req_id, payload, false);

        } else {
            LOG_M_WARNING("Unknown action: {}", action);
        }
    }

    // 处理外部命令（DEALER2 版本，使用 send_response2 发送响应）
    void handle_command2(const std::string& req_id, const json& payload) {
        std::string action = zmq_ht_proto::get_action(payload);

        if (action.empty()) {
            LOG_M_WARNING("Message missing 'action' field (socket2), req_id={}, payload={}", req_id, payload.dump());
            return;
        }

        // 心跳消息静默忽略
        if (action == zmq_ht_proto::Action::HEARTBEAT) {
            return;
        }

        LOG_M_INFO("Received command (socket2): action={}, req_id={}", action, req_id);

        if (action == "ack") {
            // 注册成功响应
            std::string status = payload.value("status", "");
            std::string message = payload.value("message", "");
            LOG_M_INFO("DEALER2 registered: status={}, message={}", status, message);

        } else if (action == "add_hot_stock_ht") {
            handle_add_hot_stock_ht(req_id, payload, true);

        } else if (action == "remove_hot_stock_ht") {
            handle_remove_hot_stock_ht(req_id, payload, true);

        } else {
            LOG_M_WARNING("Unknown action (socket2): {}", action);
        }
    }

    // ==========================================
    // 策略管理命令处理
    // ==========================================

    void handle_add_strategy(const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot add strategy");
            send_response(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto msg = zmq_ht_proto::AddStrategyMsg::from_json(payload);

        if (msg.symbol.empty()) {
            LOG_M_WARNING("ADD_STRATEGY: missing symbol");
            send_response(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        // 自动补全后缀（支持 "600000" 或 "600000.SH" 两种格式）
        std::string symbol = symbol_utils::normalize_symbol(msg.symbol);

        // 检查策略是否已存在
        if (engine_->has_strategy(symbol, msg.strategy_name)) {
            LOG_M_WARNING("ADD_STRATEGY: strategy already exists {}:{}", symbol, msg.strategy_name);
            send_response(req_id, "error", {{"message", "Strategy already exists: " + symbol + ":" + msg.strategy_name}});
            return;
        }

        // 检查策略类型是否有效
        auto& factory = StrategyFactory::instance();
        if (!factory.has_strategy(msg.strategy_name)) {
            LOG_M_WARNING("ADD_STRATEGY: unknown strategy type {}", msg.strategy_name);
            auto available = factory.get_registered_strategies();
            std::string avail_str;
            for (const auto& s : available) avail_str += s + " ";
            send_response(req_id, "error", {
                {"message", "Unknown strategy: " + msg.strategy_name},
                {"available", avail_str}
            });
            return;
        }

        // 解析 params：支持字符串、数值或对象格式
        std::string params_str;
        if (msg.params.is_string()) {
            params_str = msg.params.get<std::string>();
        } else if (msg.params.is_number()) {
            // 数值直接转字符串（如 98500 -> "98500"）
            params_str = std::to_string(msg.params.get<int64_t>());
        } else if (msg.params.is_object() && !msg.params.empty()) {
            // 对象格式：尝试取 "breakout_price" 或第一个值
            if (msg.params.contains("breakout_price")) {
                auto& bp = msg.params["breakout_price"];
                if (bp.is_number()) {
                    params_str = std::to_string(bp.get<int64_t>());
                } else if (bp.is_string()) {
                    params_str = bp.get<std::string>();
                }
            }
        }

        // 创建并注册策略
        try {
            auto strategy = factory.create(msg.strategy_name, symbol, params_str);
            if (engine_->register_strategy_runtime(symbol, std::move(strategy))) {
                LOG_M_INFO("ADD_STRATEGY: added {} -> {} (params={})", symbol, msg.strategy_name, params_str);
                send_response(req_id, "success", {
                    {"message", "Strategy added"},
                    {"symbol", symbol},
                    {"strategy", msg.strategy_name},
                    {"params", params_str}
                });
            } else {
                LOG_M_ERROR("ADD_STRATEGY: failed to register {}", symbol);
                send_response(req_id, "error", {{"message", "Failed to register strategy"}});
            }
        } catch (const std::exception& e) {
            LOG_M_ERROR("ADD_STRATEGY: exception - {}", e.what());
            send_response(req_id, "error", {{"message", e.what()}});
        }
    }

    void handle_remove_strategy(const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot remove strategy");
            send_response(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto msg = zmq_ht_proto::RemoveStrategyMsg::from_json(payload);

        if (msg.symbol.empty()) {
            LOG_M_WARNING("REMOVE_STRATEGY: missing symbol");
            send_response(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        if (msg.strategy_name.empty()) {
            LOG_M_WARNING("REMOVE_STRATEGY: missing strategy name");
            send_response(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        // 自动补全后缀
        std::string symbol = symbol_utils::normalize_symbol(msg.symbol);

        if (!engine_->has_strategy(symbol, msg.strategy_name)) {
            LOG_M_WARNING("REMOVE_STRATEGY: strategy not found {}:{}", symbol, msg.strategy_name);
            send_response(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + msg.strategy_name}});
            return;
        }

        engine_->unregister_strategy(symbol, msg.strategy_name);
        LOG_M_INFO("REMOVE_STRATEGY: removed {}:{}", symbol, msg.strategy_name);
        send_response(req_id, "success", {
            {"message", "Strategy removed"},
            {"symbol", symbol},
            {"strategy", msg.strategy_name}
        });
    }

    void handle_list_strategies(const std::string& req_id) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot list strategies");
            send_response(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto list = engine_->get_strategy_list();
        json strategies = json::array();
        for (const auto& s : list) {
            strategies.push_back(s);
        }

        LOG_M_INFO("LIST_STRATEGIES: {} strategies", list.size());
        send_response(req_id, "success", {
            {"count", list.size()},
            {"strategies", strategies}
        });
    }

    void handle_enable_strategy(const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot enable strategy");
            send_response(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            LOG_M_WARNING("ENABLE_STRATEGY: missing symbol");
            send_response(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        if (strategy_name.empty()) {
            LOG_M_WARNING("ENABLE_STRATEGY: missing strategy name");
            send_response(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        // 自动补全后缀
        symbol = symbol_utils::normalize_symbol(symbol);

        if (engine_->enable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("ENABLE_STRATEGY: enabled {}:{}", symbol, strategy_name);
            send_response(req_id, "success", {
                {"message", "Strategy enabled"},
                {"symbol", symbol},
                {"strategy", strategy_name}
            });
        } else {
            LOG_M_WARNING("ENABLE_STRATEGY: strategy not found {}:{}", symbol, strategy_name);
            send_response(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    void handle_disable_strategy(const std::string& req_id, const json& payload) {
        if (!engine_) {
            LOG_M_ERROR("Engine not initialized, cannot disable strategy");
            send_response(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            LOG_M_WARNING("DISABLE_STRATEGY: missing symbol");
            send_response(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        if (strategy_name.empty()) {
            LOG_M_WARNING("DISABLE_STRATEGY: missing strategy name");
            send_response(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        // 自动补全后缀
        symbol = symbol_utils::normalize_symbol(symbol);

        if (engine_->disable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("DISABLE_STRATEGY: disabled {}:{}", symbol, strategy_name);
            send_response(req_id, "success", {
                {"message", "Strategy disabled"},
                {"symbol", symbol},
                {"strategy", strategy_name}
            });
        } else {
            LOG_M_WARNING("DISABLE_STRATEGY: strategy not found {}:{}", symbol, strategy_name);
            send_response(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    // 发送响应消息
    void send_response(const std::string& req_id, const std::string& action, const json& data) {
        json payload;
        payload["action"] = action;
        payload["data"] = data;
        payload["data"]["req_id"] = req_id;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string response_req_id = action + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        send(response_req_id, payload);
    }

    // 发送响应消息（通过 socket2）
    void send_response2(const std::string& req_id, const std::string& action, const json& data) {
        json payload;
        payload["action"] = action;
        payload["data"] = data;
        payload["data"]["req_id"] = req_id;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string response_req_id = action + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        send2(response_req_id, payload);
    }

    // ==========================================
    // add_hot_stock_ht / remove_hot_stock_ht 处理
    // ==========================================

    // 处理 add_hot_stock_ht 请求（启用已存在的策略）
    void handle_add_hot_stock_ht(const std::string& req_id, const json& payload, bool use_socket2) {
        std::string symbol = payload.value("symbol", "");
        double target_price = payload.value("target_price", 0.0);
        std::string strategy_name = payload.value("strategy", "BreakoutPriceVolumeStrategy_v2");

        if (symbol.empty()) {
            json resp = {{"success", false}, {"msg", "Missing symbol"}};
            use_socket2 ? send_response2(req_id, "add_hot_stock_ht_resp", resp) : send_response(req_id, "add_hot_stock_ht_resp", resp);
            return;
        }

        // 内部使用带后缀的 symbol
        std::string symbol_internal = symbol_utils::normalize_symbol(symbol);
        // 发送给 ROUTER 的 symbol 不带后缀
        std::string symbol_resp = symbol_utils::strip_suffix(symbol_internal);

        if (!engine_) {
            json resp = {{"success", false}, {"msg", "Engine not initialized"}};
            use_socket2 ? send_response2(req_id, "add_hot_stock_ht_resp", resp) : send_response(req_id, "add_hot_stock_ht_resp", resp);
            return;
        }

        if (engine_->enable_strategy(symbol_internal, strategy_name, symbol_utils::price_to_int(target_price))) {
            LOG_M_INFO("add_hot_stock_ht: enabled {}:{}, target_price={}", symbol_internal, strategy_name, target_price);
            json resp = {
                {"success", true},
                {"symbol", symbol_resp},
                {"strategy", strategy_name},
                {"target_price", target_price}
            };
            use_socket2 ? send_response2(req_id, "add_hot_stock_ht_resp", resp) : send_response(req_id, "add_hot_stock_ht_resp", resp);
        } else {
            json resp = {{"success", false}, {"msg", "Strategy not found: " + symbol_resp + ":" + strategy_name}};
            use_socket2 ? send_response2(req_id, "add_hot_stock_ht_resp", resp) : send_response(req_id, "add_hot_stock_ht_resp", resp);
        }
    }

    // 处理 remove_hot_stock_ht 请求（禁用策略）
    void handle_remove_hot_stock_ht(const std::string& req_id, const json& payload, bool use_socket2) {
        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "BreakoutPriceVolumeStrategy_v2");

        if (symbol.empty()) {
            json resp = {{"success", false}, {"msg", "Missing symbol"}};
            use_socket2 ? send_response2(req_id, "remove_hot_stock_ht_resp", resp) : send_response(req_id, "remove_hot_stock_ht_resp", resp);
            return;
        }

        // 内部使用带后缀的 symbol
        std::string symbol_internal = symbol_utils::normalize_symbol(symbol);
        // 发送给 ROUTER 的 symbol 不带后缀
        std::string symbol_resp = symbol_utils::strip_suffix(symbol_internal);

        if (!engine_) {
            json resp = {{"success", false}, {"msg", "Engine not initialized"}};
            use_socket2 ? send_response2(req_id, "remove_hot_stock_ht_resp", resp) : send_response(req_id, "remove_hot_stock_ht_resp", resp);
            return;
        }

        if (engine_->disable_strategy(symbol_internal, strategy_name)) {
            LOG_M_INFO("remove_hot_stock_ht: disabled {}:{}", symbol_internal, strategy_name);
            json resp = {
                {"success", true},
                {"symbol", symbol_resp},
                {"strategy", strategy_name}
            };
            use_socket2 ? send_response2(req_id, "remove_hot_stock_ht_resp", resp) : send_response(req_id, "remove_hot_stock_ht_resp", resp);
        } else {
            json resp = {{"success", false}, {"msg", "Strategy not found: " + symbol_resp + ":" + strategy_name}};
            use_socket2 ? send_response2(req_id, "remove_hot_stock_ht_resp", resp) : send_response(req_id, "remove_hot_stock_ht_resp", resp);
        }
    }

    // ==========================================
    // Socket2 版本的策略管理命令处理
    // ==========================================

    void handle_add_strategy2(const std::string& req_id, const json& payload) {
        if (!engine_) {
            send_response2(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto msg = zmq_ht_proto::AddStrategyMsg::from_json(payload);
        if (msg.symbol.empty()) {
            send_response2(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }

        std::string symbol = symbol_utils::normalize_symbol(msg.symbol);
        if (engine_->has_strategy(symbol, msg.strategy_name)) {
            send_response2(req_id, "error", {{"message", "Strategy already exists: " + symbol + ":" + msg.strategy_name}});
            return;
        }

        auto& factory = StrategyFactory::instance();
        if (!factory.has_strategy(msg.strategy_name)) {
            auto available = factory.get_registered_strategies();
            std::string avail_str;
            for (const auto& s : available) avail_str += s + " ";
            send_response2(req_id, "error", {{"message", "Unknown strategy: " + msg.strategy_name}, {"available", avail_str}});
            return;
        }

        std::string params_str;
        if (msg.params.is_string()) {
            params_str = msg.params.get<std::string>();
        } else if (msg.params.is_number()) {
            params_str = std::to_string(msg.params.get<int64_t>());
        } else if (msg.params.is_object() && !msg.params.empty() && msg.params.contains("breakout_price")) {
            auto& bp = msg.params["breakout_price"];
            if (bp.is_number()) params_str = std::to_string(bp.get<int64_t>());
            else if (bp.is_string()) params_str = bp.get<std::string>();
        }

        try {
            auto strategy = factory.create(msg.strategy_name, symbol, params_str);
            if (engine_->register_strategy_runtime(symbol, std::move(strategy))) {
                LOG_M_INFO("ADD_STRATEGY (socket2): added {} -> {} (params={})", symbol, msg.strategy_name, params_str);
                send_response2(req_id, "success", {{"message", "Strategy added"}, {"symbol", symbol}, {"strategy", msg.strategy_name}, {"params", params_str}});
            } else {
                send_response2(req_id, "error", {{"message", "Failed to register strategy"}});
            }
        } catch (const std::exception& e) {
            send_response2(req_id, "error", {{"message", e.what()}});
        }
    }

    void handle_remove_strategy2(const std::string& req_id, const json& payload) {
        if (!engine_) {
            send_response2(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto msg = zmq_ht_proto::RemoveStrategyMsg::from_json(payload);
        if (msg.symbol.empty()) {
            send_response2(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }
        if (msg.strategy_name.empty()) {
            send_response2(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        std::string symbol = symbol_utils::normalize_symbol(msg.symbol);
        if (!engine_->has_strategy(symbol, msg.strategy_name)) {
            send_response2(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + msg.strategy_name}});
            return;
        }

        engine_->unregister_strategy(symbol, msg.strategy_name);
        LOG_M_INFO("REMOVE_STRATEGY (socket2): removed {}:{}", symbol, msg.strategy_name);
        send_response2(req_id, "success", {{"message", "Strategy removed"}, {"symbol", symbol}, {"strategy", msg.strategy_name}});
    }

    void handle_list_strategies2(const std::string& req_id) {
        if (!engine_) {
            send_response2(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        auto list = engine_->get_strategy_list();
        json strategies = json::array();
        for (const auto& s : list) {
            strategies.push_back(s);
        }
        LOG_M_INFO("LIST_STRATEGIES (socket2): {} strategies", list.size());
        send_response2(req_id, "success", {{"count", list.size()}, {"strategies", strategies}});
    }

    void handle_enable_strategy2(const std::string& req_id, const json& payload) {
        if (!engine_) {
            send_response2(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            send_response2(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }
        if (strategy_name.empty()) {
            send_response2(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        symbol = symbol_utils::normalize_symbol(symbol);
        if (engine_->enable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("ENABLE_STRATEGY (socket2): enabled {}:{}", symbol, strategy_name);
            send_response2(req_id, "success", {{"message", "Strategy enabled"}, {"symbol", symbol}, {"strategy", strategy_name}});
        } else {
            send_response2(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    void handle_disable_strategy2(const std::string& req_id, const json& payload) {
        if (!engine_) {
            send_response2(req_id, "error", {{"message", "Engine not initialized"}});
            return;
        }

        std::string symbol = payload.value("symbol", "");
        std::string strategy_name = payload.value("strategy", "");

        if (symbol.empty()) {
            send_response2(req_id, "error", {{"message", "Missing symbol"}});
            return;
        }
        if (strategy_name.empty()) {
            send_response2(req_id, "error", {{"message", "Missing strategy name"}});
            return;
        }

        symbol = symbol_utils::normalize_symbol(symbol);
        if (engine_->disable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("DISABLE_STRATEGY (socket2): disabled {}:{}", symbol, strategy_name);
            send_response2(req_id, "success", {{"message", "Strategy disabled"}, {"symbol", symbol}, {"strategy", strategy_name}});
        } else {
            send_response2(req_id, "error", {{"message", "Strategy not found: " + symbol + ":" + strategy_name}});
        }
    }

    std::string endpoint_;
    std::string endpoint2_;       // 第二个 DEALER 端点
    std::string identity_;
    std::string identity2_;       // 第二个 DEALER 的 identity
    std::atomic<bool> running_;
    void* context_;
    void* socket_;
    void* socket2_;               // 第二个 DEALER socket
    std::thread recv_thread_;
    std::thread heartbeat_thread_;
    std::thread recv_thread2_;    // 第二个 DEALER 接收线程
    std::mutex send_mutex_;       // 保护 socket_ 的发送操作
    std::mutex send2_mutex_;      // 保护 socket2_ 的发送操作
    StrategyEngine* engine_;
};

#undef LOG_MODULE

#endif // ZMQ_CLIENT_H
