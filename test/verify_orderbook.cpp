/**
 * FastOrderBook 十档盘口验证程序
 *
 * 验证 FastOrderBook 通过重放 Order/Transaction 消息重建的十档盘口,
 * 是否与交易所提供的 MDStockStruct 快照中的十档数据一致。
 *
 * 用法:
 *   ./verify_orderbook <symbol> <order_file> <txn_file> <cutoff_time> <snapshot_file>
 *
 * 参数:
 *   symbol       - 股票代码, 如 600185.SH
 *   order_file   - Order 数据文件 (TSV 格式)
 *   txn_file     - Transaction 数据文件 (TSV 格式)
 *   cutoff_time  - 截止时间, 如 92500000 表示 09:25:00.000
 *   snapshot_file - 交易所快照文件 (TSV 格式: minpx maxpx bid1_price bid1_vol ... ask10_vol)
 *
 * 编译:
 *   ./build.sh verify_orderbook
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>

#include "FastOrderBook.h"
#include "ObjectPool.h"
#include "market_data_structs_aligned.h"

// ============================================================================
// 数据结构
// ============================================================================

struct ExchangeSnapshot {
    int64_t minpx;
    int64_t maxpx;
    int64_t bid_price[10];
    int64_t bid_vol[10];
    int64_t ask_price[10];
    int64_t ask_vol[10];
};

struct MarketEvent {
    int64_t timestamp;   // mddate * 1e9 + mdtime
    int64_t applseqnum;
    int type;            // 0=order, 1=txn
    MDOrderStruct order;
    MDTransactionStruct txn;

    bool operator<(const MarketEvent& other) const {
        if (timestamp != other.timestamp) return timestamp < other.timestamp;
        return applseqnum < other.applseqnum;
    }
};

// ============================================================================
// TSV 解析函数
// ============================================================================

// 解析 Order TSV 行
// 格式: htscsecurityid\tmddate\tmdtime\tordertype\torderprice\torderqty\torderbsflag\tapplseqnum\t...
bool parse_order_tsv(const std::string& line, MDOrderStruct& order) {
    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> fields;

    while (std::getline(iss, token, '\t')) {
        fields.push_back(token);
    }

    // 至少需要基本字段
    if (fields.size() < 8) return false;

    std::memset(&order, 0, sizeof(order));

    // 解析字段 (根据 ClickHouse 导出顺序)
    // 字段顺序: htscsecurityid, mddate, mdtime, securityidsource, securitytype,
    //          orderindex, ordertype, orderprice, orderqty, orderbsflag,
    //          channelno, orderno, tradedqty, applseqnum, datamultiplepowerof10, securitystatus
    size_t idx = 0;
    if (idx < fields.size()) {
        std::strncpy(order.htscsecurityid, fields[idx++].c_str(), sizeof(order.htscsecurityid) - 1);
    }
    if (idx < fields.size()) order.mddate = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.mdtime = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.securityidsource = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.securitytype = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.orderindex = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.ordertype = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.orderprice = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.orderqty = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.orderbsflag = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.channelno = std::stoi(fields[idx++]);
    if (idx < fields.size()) order.orderno = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.tradedqty = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.applseqnum = std::stoll(fields[idx++]);
    if (idx < fields.size()) order.datamultiplepowerof10 = std::stoi(fields[idx++]);
    if (idx < fields.size()) {
        std::strncpy(order.securitystatus, fields[idx++].c_str(), sizeof(order.securitystatus) - 1);
    }

    return true;
}

// 解析 Transaction TSV 行
bool parse_txn_tsv(const std::string& line, MDTransactionStruct& txn) {
    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> fields;

    while (std::getline(iss, token, '\t')) {
        fields.push_back(token);
    }

    if (fields.size() < 8) return false;

    std::memset(&txn, 0, sizeof(txn));

    // 字段顺序: htscsecurityid, mddate, mdtime, securityidsource, securitytype,
    //          tradeindex, tradebuyno, tradesellno, tradetype, tradebsflag,
    //          tradeprice, tradeqty, trademoney, applseqnum, channelno, datamultiplepowerof10
    size_t idx = 0;
    if (idx < fields.size()) {
        std::strncpy(txn.htscsecurityid, fields[idx++].c_str(), sizeof(txn.htscsecurityid) - 1);
    }
    if (idx < fields.size()) txn.mddate = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.mdtime = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.securityidsource = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.securitytype = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.tradeindex = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.tradebuyno = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.tradesellno = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.tradetype = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.tradebsflag = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.tradeprice = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.tradeqty = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.trademoney = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.applseqnum = std::stoll(fields[idx++]);
    if (idx < fields.size()) txn.channelno = std::stoi(fields[idx++]);
    if (idx < fields.size()) txn.datamultiplepowerof10 = std::stoi(fields[idx++]);

    return true;
}

// 加载交易所快照
// 格式: minpx maxpx bid1_price bid1_vol bid2_price bid2_vol ... bid10_vol ask1_price ask1_vol ... ask10_vol
bool load_exchange_snapshot(const std::string& filepath, ExchangeSnapshot& snap) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "无法打开快照文件: " << filepath << std::endl;
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) {
        std::cerr << "快照文件为空" << std::endl;
        return false;
    }

    std::istringstream iss(line);
    std::vector<int64_t> values;
    int64_t val;

    while (iss >> val) {
        values.push_back(val);
    }

    // 期望格式: minpx maxpx + 10*2 (bid) + 10*2 (ask) = 42 个值
    if (values.size() < 42) {
        std::cerr << "快照数据不完整, 期望 42 个值, 实际 " << values.size() << std::endl;
        return false;
    }

    size_t idx = 0;
    snap.minpx = values[idx++];
    snap.maxpx = values[idx++];

    for (int i = 0; i < 10; ++i) {
        snap.bid_price[i] = values[idx++];
        snap.bid_vol[i] = values[idx++];
    }
    for (int i = 0; i < 10; ++i) {
        snap.ask_price[i] = values[idx++];
        snap.ask_vol[i] = values[idx++];
    }

    return true;
}

// ============================================================================
// 对比与输出
// ============================================================================

void print_separator() {
    std::cout << std::string(70, '=') << std::endl;
}

void print_header(const std::string& symbol, int32_t date, int32_t cutoff_time) {
    print_separator();
    std::cout << "FastOrderBook 十档盘口验证" << std::endl;
    print_separator();
    std::cout << "股票: " << symbol << std::endl;
    std::cout << "日期: " << date << std::endl;

    // 格式化时间
    int hour = cutoff_time / 10000000;
    int minute = (cutoff_time / 100000) % 100;
    int second = (cutoff_time / 1000) % 100;
    int ms = cutoff_time % 1000;
    std::cout << "截止时间: " << std::setfill('0')
              << std::setw(2) << hour << ":"
              << std::setw(2) << minute << ":"
              << std::setw(2) << second << "."
              << std::setw(3) << ms << std::endl;
    std::cout << std::endl;
}

int compare_and_print(const ExchangeSnapshot& exchange,
                      const std::vector<std::pair<uint32_t, uint64_t>>& rebuilt_bids,
                      const std::vector<std::pair<uint32_t, uint64_t>>& rebuilt_asks) {
    int match_count = 0;
    int total_levels = 20;

    std::cout << "--- 交易所快照 vs 重建结果 ---" << std::endl;
    std::cout << std::endl;

    // 买盘对比
    std::cout << "买盘:" << std::endl;
    std::cout << std::left
              << std::setw(6) << "档位"
              << std::setw(14) << "交易所价格"
              << std::setw(14) << "交易所数量"
              << std::setw(14) << "重建价格"
              << std::setw(14) << "重建数量"
              << "匹配" << std::endl;

    for (int i = 0; i < 10; ++i) {
        int64_t ex_price = exchange.bid_price[i];
        int64_t ex_vol = exchange.bid_vol[i];

        uint32_t rb_price = 0;
        uint64_t rb_vol = 0;
        if (i < static_cast<int>(rebuilt_bids.size())) {
            rb_price = rebuilt_bids[i].first;
            rb_vol = rebuilt_bids[i].second;
        }

        bool match = (static_cast<int64_t>(rb_price) == ex_price &&
                      static_cast<int64_t>(rb_vol) == ex_vol);
        if (match) match_count++;

        std::cout << std::right << std::setw(4) << (i + 1) << "  "
                  << std::setw(12) << std::fixed << std::setprecision(4) << (ex_price / 10000.0) << "  "
                  << std::setw(12) << ex_vol << "  "
                  << std::setw(12) << std::fixed << std::setprecision(4) << (rb_price / 10000.0) << "  "
                  << std::setw(12) << rb_vol << "  "
                  << (match ? "[OK]" : "[DIFF]") << std::endl;
    }

    std::cout << std::endl;

    // 卖盘对比
    std::cout << "卖盘:" << std::endl;
    std::cout << std::left
              << std::setw(6) << "档位"
              << std::setw(14) << "交易所价格"
              << std::setw(14) << "交易所数量"
              << std::setw(14) << "重建价格"
              << std::setw(14) << "重建数量"
              << "匹配" << std::endl;

    for (int i = 0; i < 10; ++i) {
        int64_t ex_price = exchange.ask_price[i];
        int64_t ex_vol = exchange.ask_vol[i];

        uint32_t rb_price = 0;
        uint64_t rb_vol = 0;
        if (i < static_cast<int>(rebuilt_asks.size())) {
            rb_price = rebuilt_asks[i].first;
            rb_vol = rebuilt_asks[i].second;
        }

        bool match = (static_cast<int64_t>(rb_price) == ex_price &&
                      static_cast<int64_t>(rb_vol) == ex_vol);
        if (match) match_count++;

        std::cout << std::right << std::setw(4) << (i + 1) << "  "
                  << std::setw(12) << std::fixed << std::setprecision(4) << (ex_price / 10000.0) << "  "
                  << std::setw(12) << ex_vol << "  "
                  << std::setw(12) << std::fixed << std::setprecision(4) << (rb_price / 10000.0) << "  "
                  << std::setw(12) << rb_vol << "  "
                  << (match ? "[OK]" : "[DIFF]") << std::endl;
    }

    std::cout << std::endl;
    print_separator();
    std::cout << "结果: " << match_count << "/" << total_levels << " 档位匹配 ("
              << std::fixed << std::setprecision(1) << (match_count * 100.0 / total_levels) << "%)" << std::endl;
    print_separator();

    return match_count;
}

// ============================================================================
// 主函数
// ============================================================================

void print_usage(const char* prog) {
    std::cerr << "用法: " << prog << " <symbol> <order_file> <txn_file> <cutoff_time> <snapshot_file>" << std::endl;
    std::cerr << std::endl;
    std::cerr << "参数:" << std::endl;
    std::cerr << "  symbol        - 股票代码, 如 600185.SH" << std::endl;
    std::cerr << "  order_file    - Order 数据文件 (TSV 格式)" << std::endl;
    std::cerr << "  txn_file      - Transaction 数据文件 (TSV 格式, 可为空文件)" << std::endl;
    std::cerr << "  cutoff_time   - 截止时间, 如 92500000 表示 09:25:00.000" << std::endl;
    std::cerr << "  snapshot_file - 交易所快照文件" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例:" << std::endl;
    std::cerr << "  " << prog << " 600185.SH /tmp/order.tsv /tmp/txn.tsv 92500000 /tmp/snapshot.txt" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        print_usage(argv[0]);
        return 1;
    }

    std::string symbol = argv[1];
    std::string order_file = argv[2];
    std::string txn_file = argv[3];
    int32_t cutoff_time = std::atoi(argv[4]);
    std::string snapshot_file = argv[5];

    // 1. 加载交易所快照
    std::cout << "[1/5] 加载交易所快照..." << std::endl;
    ExchangeSnapshot exchange;
    if (!load_exchange_snapshot(snapshot_file, exchange)) {
        return 1;
    }
    std::cout << "  价格范围: " << (exchange.minpx / 10000.0) << " - " << (exchange.maxpx / 10000.0) << std::endl;

    // 2. 加载 Order 数据
    std::cout << "[2/5] 加载 Order 数据..." << std::endl;
    std::vector<MarketEvent> events;
    int32_t mddate = 0;

    {
        std::ifstream file(order_file);
        if (!file.is_open()) {
            std::cerr << "无法打开 Order 文件: " << order_file << std::endl;
            return 1;
        }

        std::string line;
        size_t order_count = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            MDOrderStruct order;
            if (parse_order_tsv(line, order)) {
                // 只加载截止时间之前的数据
                if (order.mdtime < cutoff_time) {
                    MarketEvent ev;
                    ev.timestamp = static_cast<int64_t>(order.mddate) * 1000000000LL + order.mdtime;
                    ev.applseqnum = order.applseqnum;
                    ev.type = 0;  // order
                    ev.order = order;
                    events.push_back(std::move(ev));
                    order_count++;
                    if (mddate == 0) mddate = order.mddate;
                }
            }
        }
        std::cout << "  加载 " << order_count << " 条 Order 记录 (mdtime < " << cutoff_time << ")" << std::endl;
    }

    // 3. 加载 Transaction 数据
    std::cout << "[3/5] 加载 Transaction 数据..." << std::endl;
    {
        std::ifstream file(txn_file);
        if (!file.is_open()) {
            std::cout << "  Transaction 文件不存在或为空 (集合竞价期间正常)" << std::endl;
        } else {
            std::string line;
            size_t txn_count = 0;
            while (std::getline(file, line)) {
                if (line.empty()) continue;
                MDTransactionStruct txn;
                if (parse_txn_tsv(line, txn)) {
                    if (txn.mdtime < cutoff_time) {
                        MarketEvent ev;
                        ev.timestamp = static_cast<int64_t>(txn.mddate) * 1000000000LL + txn.mdtime;
                        ev.applseqnum = txn.applseqnum;
                        ev.type = 1;  // txn
                        ev.txn = txn;
                        events.push_back(std::move(ev));
                        txn_count++;
                    }
                }
            }
            std::cout << "  加载 " << txn_count << " 条 Transaction 记录" << std::endl;
        }
    }

    // 4. 排序并回放
    std::cout << "[4/5] 排序并回放事件..." << std::endl;
    std::sort(events.begin(), events.end());
    std::cout << "  总事件数: " << events.size() << std::endl;

    // 初始化 FastOrderBook
    // 注意: minpx/maxpx 已经是 * 10000 格式
    uint32_t min_price = static_cast<uint32_t>(exchange.minpx);
    uint32_t max_price = static_cast<uint32_t>(exchange.maxpx);

    // 扩大价格范围以防边界问题
    if (min_price > 10000) min_price -= 10000;  // -1 元
    max_price += 10000;  // +1 元

    ObjectPool<OrderNode> pool(500000);
    uint32_t stock_code = 0;  // 简化处理, 不解析股票代码数字部分
    FastOrderBook book(stock_code, pool, min_price, max_price);

    // 回放
    auto start = std::chrono::high_resolution_clock::now();
    size_t order_processed = 0;
    size_t txn_processed = 0;

    for (const auto& ev : events) {
        if (ev.type == 0) {
            book.on_order(ev.order);
            order_processed++;
        } else {
            book.on_transaction(ev.txn);
            txn_processed++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "  回放完成, 耗时 " << duration.count() << " ms" << std::endl;
    std::cout << "  处理 Order: " << order_processed << ", Transaction: " << txn_processed << std::endl;

    // 5. 获取重建的十档并对比
    std::cout << "[5/5] 对比十档盘口..." << std::endl;
    std::cout << std::endl;

    auto rebuilt_bids = book.get_bid_levels(10);
    auto rebuilt_asks = book.get_ask_levels(10);

    print_header(symbol, mddate, cutoff_time);
    int match_count = compare_and_print(exchange, rebuilt_bids, rebuilt_asks);

    // 输出内存池状态
    std::cout << std::endl;
    std::cout << "内存池状态:" << std::endl;
    std::cout << "  已分配节点: " << pool.size() << std::endl;
    std::cout << "  空闲节点: " << pool.free_count() << std::endl;
    std::cout << "  活跃订单: " << (pool.size() - pool.free_count()) << std::endl;

    return (match_count == 20) ? 0 : 1;
}
