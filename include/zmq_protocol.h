#ifndef ZMQ_PROTOCOL_H
#define ZMQ_PROTOCOL_H

#include <nlohmann/json.hpp>
#include <string>

namespace zmq_ht_proto {

// 协议版本
constexpr int PROTOCOL_VERSION = 1;

// ========== Action 类型 (payload.action) ==========
namespace Action {
    // 系统消息
    constexpr const char* REGISTER = "register";
    constexpr const char* HEARTBEAT = "heartbeat";
    constexpr const char* STATUS = "status";
    constexpr const char* ACK = "ack";

    // 业务消息（旧版，保持兼容）
    constexpr const char* ADD_HOT_STOCK = "add_hot_stock";
    constexpr const char* REMOVE_HOT_STOCK = "remove_hot_stock";
    constexpr const char* PLACE_ORDER = "place_order";

    // 策略管理
    constexpr const char* ADD_STRATEGY = "add_strategy";
    constexpr const char* REMOVE_STRATEGY = "remove_strategy";
    constexpr const char* LIST_STRATEGIES = "list_strategies";

    // 策略启用/禁用
    constexpr const char* ENABLE_STRATEGY = "enable_strategy";
    constexpr const char* DISABLE_STRATEGY = "disable_strategy";
}

// ========== 消息结构体 ==========

// 添加热门股票请求
struct AddHotStockMsg {
    std::string symbol;
    double target_price;
    int version = PROTOCOL_VERSION;

    static AddHotStockMsg from_json(const nlohmann::json& j) {
        AddHotStockMsg msg;
        msg.symbol = j.value("symbol", "");
        msg.target_price = j.value("target_price", 0.0);
        msg.version = j.value("version", PROTOCOL_VERSION);
        return msg;
    }

    nlohmann::json to_json() const {
        return {
            {"action", Action::ADD_HOT_STOCK},
            {"symbol", symbol},
            {"target_price", target_price},
            {"version", version}
        };
    }
};

// 移除热门股票请求
struct RemoveHotStockMsg {
    std::string symbol;

    static RemoveHotStockMsg from_json(const nlohmann::json& j) {
        RemoveHotStockMsg msg;
        msg.symbol = j.value("symbol", "");
        return msg;
    }
};

// 添加策略请求
struct AddStrategyMsg {
    std::string symbol;
    std::string strategy_name;
    nlohmann::json params;  // 可选参数

    static AddStrategyMsg from_json(const nlohmann::json& j) {
        AddStrategyMsg msg;
        msg.symbol = j.value("symbol", "");
        msg.strategy_name = j.value("strategy", "BreakoutPriceVolumeStrategy");
        msg.params = j.value("params", nlohmann::json::object());
        return msg;
    }
};

// 移除策略请求
struct RemoveStrategyMsg {
    std::string symbol;
    std::string strategy_name;

    static RemoveStrategyMsg from_json(const nlohmann::json& j) {
        RemoveStrategyMsg msg;
        msg.symbol = j.value("symbol", "");
        msg.strategy_name = j.value("strategy", "");
        return msg;
    }
};

// ========== 辅助函数 ==========

inline std::string get_action(const nlohmann::json& payload) {
    return payload.value("action", "");
}

inline std::string get_type(const nlohmann::json& payload) {
    return payload.value("type", "");
}

} // namespace zmq_ht_proto

#endif // ZMQ_PROTOCOL_H
