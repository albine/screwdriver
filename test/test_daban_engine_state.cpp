#include <cassert>
#include <iostream>
#include <vector>

#include "logger.h"
#include "strategy_base.h"
#include "strategy_context.h"

#define private public
#include "strategy/DabanStrategy.h"
#undef private

void Strategy::on_control_message(const ControlMessage&) {}

void Strategy::place_order(const TradeSignal& signal) {
    if (ctx_) {
        ctx_->place_order(signal);
    }
}

namespace {

struct CapturingContext : public StrategyContext {
    std::vector<TradeSignal> signals;

    void place_order(const TradeSignal& signal) override {
        signals.push_back(signal);
    }
};

MDStockStruct make_stock(int32_t mdtime, int64_t preclose) {
    MDStockStruct stock{};
    stock.mdtime = mdtime;
    stock.preclosepx = preclose;
    return stock;
}

int test_backup_buy_activates_before_signal() {
    DabanStrategy strategy("600000.SH_DB", "600000.SH");
    CapturingContext ctx;
    strategy.set_context(&ctx);

    MDStockStruct stock = make_stock(93000000, 10000);
    stock.sellpricequeue[0] = 11000;
    stock.sellorderqtyqueue[0] = 10000000;
    stock.totalvolumetrade = 6000000;

    strategy.on_tick(stock);

    assert(strategy.activate_count_ == 1);
    assert(strategy.state_ == DabanStrategy::State::EXIT_ARMED);
    assert(strategy.t0_ == stock.mdtime);
    assert(strategy.signal_triggered_);
    assert(ctx.signals.size() == 1);
    assert(ctx.signals[0].side == TradeSignal::Side::BUY);
    assert(ctx.signals[0].price == strategy.limit_up_price_);
    assert(ctx.signals[0].trigger_time == stock.mdtime);
    return 0;
}

int test_close_forces_done_without_signal() {
    DabanStrategy strategy("600000.SH_DB", "600000.SH");
    CapturingContext ctx;
    strategy.set_context(&ctx);
    strategy.state_ = DabanStrategy::State::ACTIVE;

    MDStockStruct stock = make_stock(145700000, 10000);
    strategy.on_tick(stock);

    assert(strategy.state_ == DabanStrategy::State::DONE);
    assert(ctx.signals.empty());
    return 0;
}

}  // namespace

int main() {
    hft::logger::LogConfig log_config;
    log_config.console_output = false;
    log_config.log_file = "test_daban_engine_state.log";
    log_config.log_level = quill::LogLevel::Debug;
    hft::logger::init(log_config);
    hft::logger::init_biz();

    if (test_backup_buy_activates_before_signal() != 0) {
        std::cerr << "test_backup_buy_activates_before_signal failed\n";
        return 1;
    }

    if (test_close_forces_done_without_signal() != 0) {
        std::cerr << "test_close_forces_done_without_signal failed\n";
        return 1;
    }

    hft::logger::shutdown();
    std::cout << "test_daban_engine_state PASSED\n";
    return 0;
}
