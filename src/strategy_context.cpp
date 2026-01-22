#include "strategy_context.h"
#include "zmq_client.h"

void LiveContext::place_order(const TradeSignal& signal) {
    // 记录到业务日志
    LOG_BIZ(BIZ_ORDR,
            "{} | {} | {} @ {} | qty={} | trigger_time={}",
            signal.symbol,
            signal.strategy_name,
            signal.side_str(),
            symbol_utils::int_to_price(signal.price),
            signal.quantity,
            signal.trigger_time);

    // 通过 ZMQ 发送下单请求
    if (zmq_client_) {
        zmq_client_->send_place_order(signal.symbol, symbol_utils::int_to_price(signal.price), signal.strategy_type_name());
    }
}
