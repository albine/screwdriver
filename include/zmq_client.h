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

// 定义本文件的日志模块
#define LOG_MODULE "ZMQ"

using json = nlohmann::json;

class ZmqClient {
public:
    explicit ZmqClient(const std::string& endpoint = "tcp://localhost:13380")
        : endpoint_(endpoint), running_(false), context_(nullptr), socket_(nullptr) {}

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

        LOG_M_INFO("ZMQ client started with heartbeat every 60s");

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

        if (socket_) {
            zmq_close(socket_);
            socket_ = nullptr;
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

        } else if (action == zmq_ht_proto::Action::ADD_HOT_STOCK) {
            auto msg = zmq_ht_proto::AddHotStockMsg::from_json(payload);
            LOG_M_INFO("ADD_HOT_STOCK: symbol={}, target_price={}", msg.symbol, msg.target_price);
            // TODO: 调用策略引擎添加热门股票
            // 不能response，没完没了了

        } else if (action == zmq_ht_proto::Action::REMOVE_HOT_STOCK) {
            auto msg = zmq_ht_proto::RemoveHotStockMsg::from_json(payload);
            LOG_M_INFO("REMOVE_HOT_STOCK: symbol={}", msg.symbol);
            // TODO: 调用策略引擎移除热门股票
            // 不能response，没完没了了

        } else {
            LOG_M_WARNING("Unknown action: {}", action);
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
    std::string identity_;
    std::atomic<bool> running_;
    void* context_;
    void* socket_;
    std::thread recv_thread_;
    std::thread heartbeat_thread_;
};

#undef LOG_MODULE

#endif // ZMQ_CLIENT_H
