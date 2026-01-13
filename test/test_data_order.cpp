#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include "history_data_replayer.h"
#include "FastOrderBook.h"

// 直接回放测试（不经过StrategyEngine队列）
std::unordered_set<uint64_t> processed_orders;
uint64_t missing_order_count = 0;
uint64_t total_txn_count = 0;

void check_transaction(const MDTransactionStruct& txn) {
    total_txn_count++;

    TradeBSFlag bsflag = static_cast<TradeBSFlag>(txn.tradebsflag);
    uint64_t buy_order = txn.tradebuyno;
    uint64_t sell_order = txn.tradesellno;

    bool buy_exists = processed_orders.count(buy_order) > 0;
    bool sell_exists = processed_orders.count(sell_order) > 0;

    // 上海市场逻辑
    if (bsflag == TradeBSFlag::Buy) {
        // 买方主动，需要卖单存在
        if (!sell_exists) {
            missing_order_count++;
            if (missing_order_count <= 10) {
                std::cout << "❌ TXN #" << total_txn_count
                          << " ApplSeqNum=" << txn.applseqnum
                          << " 找不到卖单 " << sell_order << "\n";
            }
        }
    } else if (bsflag == TradeBSFlag::Sell) {
        // 卖方主动，需要买单存在
        if (!buy_exists) {
            missing_order_count++;
            if (missing_order_count <= 10) {
                std::cout << "❌ TXN #" << total_txn_count
                          << " ApplSeqNum=" << txn.applseqnum
                          << " 找不到买单 " << buy_order << "\n";
            }
        }
    }
}

int main() {
    std::cout << "=== 数据文件顺序检查（直接回放，无队列）===\n\n";

    HistoryDataReplayer replayer(1);

    if (!replayer.load_order_file("test_data_cleaned/MD_ORDER_StockType_603122.SH.txt")) {
        std::cerr << "加载ORDER文件失败!\n";
        return 1;
    }

    if (!replayer.load_transaction_file("test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt")) {
        std::cerr << "加载TRANSACTION文件失败!\n";
        return 1;
    }

    std::cout << "总事件数: " << replayer.event_count() << "\n\n";

    replayer.set_order_callback([](const MDOrderStruct& order) {
        processed_orders.insert(order.orderno);
    });

    replayer.set_transaction_callback([](const MDTransactionStruct& txn) {
        check_transaction(txn);
    });

    std::cout << "开始回放...\n";
    replayer.replay();

    std::cout << "\n=== 统计结果 ===\n";
    std::cout << "处理的ORDER数: " << processed_orders.size() << "\n";
    std::cout << "处理的TXN数: " << total_txn_count << "\n";
    std::cout << "TXN找不到订单次数: " << missing_order_count << "\n";

    if (missing_order_count > 0) {
        std::cout << "\n❌ 数据文件本身存在顺序问题！\n";
        std::cout << "   这意味着HistoryDataReplayer的排序没有正确工作\n";
    } else {
        std::cout << "\n✅ 数据文件顺序正确\n";
        std::cout << "   问题出在StrategyEngine的队列系统\n";
    }

    return 0;
}
