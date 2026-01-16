#ifndef ZMQ_CLIENT_H
#define ZMQ_CLIENT_H

#include <zmq.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include "logger.h"
#include "zmq_protocol.h"
#include "strategy_engine.h"
#include "strategy_factory.h"
#include "symbol_utils.h"

// 定义本文件的日志模块
#define LOG_MODULE "ZMQ"

using json = nlohmann::json;

class ZmqClient {
public:
    explicit ZmqClient(const std::string& endpoint = "tcp://localhost:13380",
                       const std::string& rep_endpoint = "tcp://*:13381")
        : endpoint_(endpoint), rep_endpoint_(rep_endpoint),
          running_(false), context_(nullptr), socket_(nullptr),
          rep_socket_(nullptr), engine_(nullptr) {}

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

        // 启动 REP 服务端
        if (!start_rep_server()) {
            LOG_M_WARNING("REP server failed to start, continuing without it");
        }

        LOG_M_INFO("ZMQ client started with heartbeat every 60s");

        return true;
    }

    // 启动 REP 服务端（端口 13381）
    bool start_rep_server() {
        rep_socket_ = zmq_socket(context_, ZMQ_REP);
        if (!rep_socket_) {
            LOG_M_ERROR("Failed to create REP socket: {}", zmq_strerror(errno));
            return false;
        }

        // 设置接收超时
        int recv_timeout = 1000;
        zmq_setsockopt(rep_socket_, ZMQ_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // 绑定到端口
        if (zmq_bind(rep_socket_, rep_endpoint_.c_str()) != 0) {
            LOG_M_ERROR("Failed to bind REP socket to {}: {}", rep_endpoint_, zmq_strerror(errno));
            zmq_close(rep_socket_);
            rep_socket_ = nullptr;
            return false;
        }

        LOG_M_INFO("REP server listening on {}", rep_endpoint_);

        // 启动 REP 接收线程
        rep_thread_ = std::thread(&ZmqClient::rep_recv_loop, this);
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
        if (rep_thread_.joinable()) {
            rep_thread_.join();
        }

        if (socket_) {
            zmq_close(socket_);
            socket_ = nullptr;
        }
        if (rep_socket_) {
            zmq_close(rep_socket_);
            rep_socket_ = nullptr;
        }
        if (context_) {
            zmq_ctx_destroy(context_);
            context_ = nullptr;
        }

        LOG_M_INFO("ZMQ client stopped");
    }

    // 发送消息（DEALER模式，直接发送，无需手动加空帧）
    bool send(const std::string& req_id, const json& payload) {
        if (!socket_ || !running_) return false;

        json msg;
        msg["req_id"] = req_id;
        msg["payload"] = payload;
        std::string data = msg.dump();

        // DEALER 直接发送数据，ZMQ 会自动处理路由
        int rc = zmq_send(socket_, data.c_str(), data.size(), 0);
        if (rc >= 0) {
            LOG_M_INFO("Sent message: req_id={}, size={}", req_id, data.size());
        } else {
            LOG_M_ERROR("Failed to send message: {}", zmq_strerror(errno));
        }
        return rc >= 0;
    }

    bool is_running() const { return running_; }

    // 发送下单消息到 ROUTER
    void send_place_order(const std::string& symbol, double price) {
        static int order_seq = 0;
        json payload;
        payload["action"] = zmq_ht_proto::Action::PLACE_ORDER;
        payload["symbol"] = symbol;
        payload["price"] = price;
        payload["side"] = "buy";
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string req_id = "order_" + std::to_string(++order_seq);
        send(req_id, payload);
        LOG_M_INFO("Sent place_order: symbol={}, price={}, side=buy", symbol, price);
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

    // REP 服务端接收循环（处理 add_hot_stock / remove_hot_stock 请求）
    void rep_recv_loop() {
        char buffer[65536];

        while (running_) {
            int rc = zmq_recv(rep_socket_, buffer, sizeof(buffer) - 1, 0);
            if (rc < 0) {
                if (errno == EAGAIN) continue;  // 超时，继续循环
                if (!running_) break;
                continue;
            }

            if (rc > 0) {
                buffer[rc] = '\0';
                std::string response = process_rep_request(buffer, rc);
                // REQ/REP 模式必须发送响应
                zmq_send(rep_socket_, response.c_str(), response.size(), 0);
            }
        }
    }

    // 处理 REP 服务端收到的请求
    std::string process_rep_request(const char* data, size_t len) {
        json response;
        response["success"] = false;

        try {
            json request = json::parse(std::string(data, len));
            std::string action = request.value("action", "");

            LOG_M_INFO("REP received: action={}", action);

            if (action == "add_hot_stock") {
                return handle_rep_add_hot_stock(request);
            } else if (action == "remove_hot_stock") {
                return handle_rep_remove_hot_stock(request);
            } else {
                response["error"] = "Unknown action: " + action;
                LOG_M_WARNING("REP unknown action: {}", action);
            }
        } catch (const json::exception& e) {
            response["error"] = std::string("JSON parse error: ") + e.what();
            LOG_M_ERROR("REP JSON parse error: {}", e.what());
        }

        return response.dump();
    }

    // 处理 add_hot_stock 请求（启用已存在的 BreakoutPriceVolumeStrategy）
    std::string handle_rep_add_hot_stock(const json& request) {
        json response;
        std::string symbol = request.value("symbol", "");

        if (symbol.empty()) {
            response["success"] = false;
            response["error"] = "Missing symbol";
            return response.dump();
        }

        symbol = symbol_utils::normalize_symbol(symbol);

        if (!engine_) {
            response["success"] = false;
            response["error"] = "Engine not initialized";
            return response.dump();
        }

        std::string strategy_name = "BreakoutPriceVolumeStrategy";

        if (engine_->enable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("REP add_hot_stock: enabled {}:{}", symbol, strategy_name);
            response["success"] = true;
            response["symbol"] = symbol;
            response["strategy"] = strategy_name;
        } else {
            response["success"] = false;
            response["error"] = "Strategy not found: " + symbol + ":" + strategy_name;
        }

        return response.dump();
    }

    // 处理 remove_hot_stock 请求（禁用 BreakoutPriceVolumeStrategy）
    std::string handle_rep_remove_hot_stock(const json& request) {
        json response;
        std::string symbol = request.value("symbol", "");

        if (symbol.empty()) {
            response["success"] = false;
            response["error"] = "Missing symbol";
            return response.dump();
        }

        symbol = symbol_utils::normalize_symbol(symbol);

        if (!engine_) {
            response["success"] = false;
            response["error"] = "Engine not initialized";
            return response.dump();
        }

        std::string strategy_name = "BreakoutPriceVolumeStrategy";

        if (engine_->disable_strategy(symbol, strategy_name)) {
            LOG_M_INFO("REP remove_hot_stock: disabled {}:{}", symbol, strategy_name);
            response["success"] = true;
            response["symbol"] = symbol;
            response["strategy"] = strategy_name;
        } else {
            response["success"] = false;
            response["error"] = "Strategy not found: " + symbol + ":" + strategy_name;
        }

        return response.dump();
    }

    // 发送注册消息，让 ROUTER 知道我们的 identity
    void send_register() {
        json payload;
        payload["action"] = zmq_ht_proto::Action::REGISTER;
        payload["client"] = identity_;
        payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        send("register_0", payload);
        LOG_M_INFO("Sent register message to ROUTER");
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

        if (action == zmq_ht_proto::Action::STATUS) {
            send_response(req_id, zmq_ht_proto::Action::STATUS, {{"status", "running"}});

        } else if (action == zmq_ht_proto::Action::ADD_HOT_STOCK ||
                   action == zmq_ht_proto::Action::ADD_STRATEGY) {
            handle_add_strategy(req_id, payload);

        } else if (action == zmq_ht_proto::Action::REMOVE_HOT_STOCK ||
                   action == zmq_ht_proto::Action::REMOVE_STRATEGY) {
            handle_remove_strategy(req_id, payload);

        } else if (action == zmq_ht_proto::Action::LIST_STRATEGIES) {
            handle_list_strategies(req_id);

        } else if (action == zmq_ht_proto::Action::ENABLE_STRATEGY) {
            handle_enable_strategy(req_id, payload);

        } else if (action == zmq_ht_proto::Action::DISABLE_STRATEGY) {
            handle_disable_strategy(req_id, payload);

        } else {
            LOG_M_WARNING("Unknown action: {}", action);
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

    std::string endpoint_;
    std::string rep_endpoint_;  // REP 服务端端点
    std::string identity_;
    std::atomic<bool> running_;
    void* context_;
    void* socket_;
    void* rep_socket_;          // REP 服务端 socket
    std::thread recv_thread_;
    std::thread heartbeat_thread_;
    std::thread rep_thread_;    // REP 服务端接收线程
    StrategyEngine* engine_;
};

#undef LOG_MODULE

#endif // ZMQ_CLIENT_H
