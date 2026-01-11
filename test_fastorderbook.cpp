/**
 * 使用HistoryDataReplayer测试FastOrderBook
 *
 * 编译: g++ -std=c++17 -O2 -I include -I src test_fastorderbook.cpp src/FastOrderBook.cpp -o test_fastorderbook -lpthread
 * 运行: ./test_fastorderbook
 */

#include <iostream>
#include <chrono>
#include <atomic>
#include <mutex>
#include "history_data_replayer.h"
#include "FastOrderBook.h"
#include "ObjectPool.h"

// 统计信息
struct Stats {
    std::atomic<uint64_t> order_count{0};
    std::atomic<uint64_t> transaction_count{0};
    std::atomic<uint64_t> order_success{0};
    std::atomic<uint64_t> transaction_success{0};
};

int main() {
    std::cout << "=== FastOrderBook 历史数据回放测试 ===" << std::endl;
    std::cout << std::endl;

    // 1. 初始化
    // 价格范围: 603122在20251031的价格范围约为80100-97900
    // 设置更宽泛的范围以覆盖可能的波动
    const uint32_t MIN_PRICE = 70000;  // 7.0元 * 10000
    const uint32_t MAX_PRICE = 110000; // 11.0元 * 10000
    const uint32_t STOCK_CODE = 603122;

    ObjectPool<OrderNode> pool(200000);  // 预分配20万订单节点
    FastOrderBook orderbook(STOCK_CODE, pool, MIN_PRICE, MAX_PRICE);

    Stats stats;
    std::mutex orderbook_mutex;  // 保护OrderBook的并发访问

    // 2. 创建回放器
    HistoryDataReplayer replayer(1);  // 单线程回放以保证顺序

    // 3. 加载数据
    std::cout << "加载数据文件..." << std::endl;

    if (!replayer.load_order_file("test_data_cleaned/MD_ORDER_StockType_603122.SH.txt")) {
        std::cerr << "加载ORDER文件失败!" << std::endl;
        return 1;
    }

    if (!replayer.load_transaction_file("test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.txt")) {
        std::cerr << "加载TRANSACTION文件失败!" << std::endl;
        return 1;
    }

    std::cout << "总事件数: " << replayer.event_count() << std::endl;
    std::cout << std::endl;

    // 4. 设置回调
    replayer.set_order_callback([&](const MDOrderStruct& order) {
        stats.order_count++;
        std::lock_guard<std::mutex> lock(orderbook_mutex);
        if (orderbook.on_order(order)) {
            stats.order_success++;
        }
    });

    replayer.set_transaction_callback([&](const MDTransactionStruct& txn) {
        stats.transaction_count++;
        std::lock_guard<std::mutex> lock(orderbook_mutex);
        if (orderbook.on_transaction(txn)) {
            stats.transaction_success++;
        }
    });

    // 5. 回放
    std::cout << "开始回放..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    replayer.replay();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 6. 输出统计
    std::cout << std::endl;
    std::cout << "=== 回放完成 ===" << std::endl;
    std::cout << "耗时: " << duration.count() << " ms" << std::endl;
    std::cout << std::endl;

    std::cout << "=== 处理统计 ===" << std::endl;
    std::cout << "委托消息: " << stats.order_count << " (成功: " << stats.order_success << ")" << std::endl;
    std::cout << "成交消息: " << stats.transaction_count << " (成功: " << stats.transaction_success << ")" << std::endl;
    std::cout << std::endl;

    std::cout << "=== 订单簿状态 (买卖十档) ===" << std::endl;
    std::cout << std::endl;

    // 获取买卖十档
    auto ask_levels = orderbook.get_ask_levels(10);
    auto bid_levels = orderbook.get_bid_levels(10);

    // 打印卖盘 (从卖十到卖一，即价格从高到低)
    std::cout << "--- 卖盘 ---" << std::endl;
    if (ask_levels.empty()) {
        std::cout << "  (空)" << std::endl;
    } else {
        // 卖盘从高到低显示 (先打印卖十再打印卖一)
        for (int i = (int)ask_levels.size() - 1; i >= 0; --i) {
            auto& [price, volume] = ask_levels[i];
            std::cout << "  卖" << (i + 1) << ": "
                      << price / 10000.0 << " 元  "
                      << volume << std::endl;
        }
    }

    std::cout << "------------" << std::endl;

    // 打印买盘 (从买一到买十，即价格从高到低)
    std::cout << "--- 买盘 ---" << std::endl;
    if (bid_levels.empty()) {
        std::cout << "  (空)" << std::endl;
    } else {
        for (size_t i = 0; i < bid_levels.size(); ++i) {
            auto& [price, volume] = bid_levels[i];
            std::cout << "  买" << (i + 1) << ": "
                      << price / 10000.0 << " 元  "
                      << volume << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "=== 内存池状态 ===" << std::endl;
    std::cout << "已分配节点: " << pool.size() << std::endl;
    std::cout << "空闲节点: " << pool.free_count() << std::endl;
    std::cout << "活跃订单: " << (pool.size() - pool.free_count()) << std::endl;

    // 调试：检查80100价格档位的实际状态
    std::cout << std::endl;
    std::cout << "=== 调试信息 ===" << std::endl;
    std::cout << "80100档位量: " << orderbook.get_volume_at_price(80100) << std::endl;
    std::cout << "97900档位量: " << orderbook.get_volume_at_price(97900) << std::endl;

    return 0;
}
