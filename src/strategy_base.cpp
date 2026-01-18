#include "strategy_base.h"
#include "strategy_engine.h"  // for ControlMessage
#include "strategy_context.h"
#include "trade_signal.h"

void Strategy::on_control_message(const ControlMessage& msg) {
    // 用 unique_id 匹配：检查策略类型 ID 是否一致（数字比较，高性能）
    uint8_t msg_strategy_id = msg.unique_id & 0xFF;
    if (strategy_type_id != msg_strategy_id) {
        return;  // 不是目标策略
    }

    if (msg.type == ControlMessage::Type::ENABLE) {
        set_enabled(true);
    } else if (msg.type == ControlMessage::Type::DISABLE) {
        set_enabled(false);
    }
}

void Strategy::place_order(const TradeSignal& signal) {
    if (ctx_) {
        ctx_->place_order(signal);
    }
}
