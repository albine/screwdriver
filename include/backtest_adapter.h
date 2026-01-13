#ifndef BACKTEST_ADAPTER_H
#define BACKTEST_ADAPTER_H

#include "history_data_replayer.h"
#include "strategy_engine.h"

// ==========================================
// 回测数据适配器
// ==========================================
// 封装 HistoryDataReplayer，将历史数据转发到策略引擎
class BacktestAdapter {
private:
    HistoryDataReplayer replayer_;
    StrategyEngine* engine_;

public:
    explicit BacktestAdapter(StrategyEngine* engine, int shard_count = 4)
        : replayer_(shard_count), engine_(engine) {

        // 设置回调函数，将数据转发到策略引擎
        replayer_.set_tick_callback([this](const MDStockStruct& stock) {
            engine_->on_market_tick(stock);
        });

        replayer_.set_order_callback([this](const MDOrderStruct& order) {
            engine_->on_market_order(order);
        });

        replayer_.set_transaction_callback([this](const MDTransactionStruct& txn) {
            engine_->on_market_transaction(txn);
        });
    }

    // 加载数据文件
    bool load_tick_file(const std::string& filepath) {
        return replayer_.load_tick_file(filepath);
    }

    bool load_order_file(const std::string& filepath) {
        return replayer_.load_order_file(filepath);
    }

    bool load_transaction_file(const std::string& filepath) {
        return replayer_.load_transaction_file(filepath);
    }

    // 开始回放
    void replay() {
        replayer_.replay();
    }

    // 获取事件总数
    size_t event_count() const {
        return replayer_.event_count();
    }
};

#endif // BACKTEST_ADAPTER_H
