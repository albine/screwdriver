#ifndef HISTORY_DATA_REPLAYER_H
#define HISTORY_DATA_REPLAYER_H

#include <string>
#include <vector>
#include <variant>
#include <functional>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <cstring>
#include "market_data_structs_aligned.h"
#include "market_data_enums.h"

enum class MarketEventType { TICK, ORDER, TRANSACTION };

struct MarketEvent {
    int64_t timestamp;       // MDDate * 1e9 + MDTime
    int64_t applseqnum;      // Secondary sort key
    MarketEventType type;
    std::variant<MDStockStruct, MDOrderStruct, MDTransactionStruct> data;

    bool operator<(const MarketEvent& other) const {
        if (timestamp != other.timestamp) return timestamp < other.timestamp;
        return applseqnum < other.applseqnum;
    }
};

class HistoryDataReplayer {
public:
    explicit HistoryDataReplayer(int shard_count = 4)
        : shard_count_(shard_count), shard_events_(shard_count) {}

    bool load_tick_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            MDStockStruct stock = {};
            if (parse_tick_line(line, stock)) {
                int shard_id = get_shard_id(stock.htscsecurityid);
                MarketEvent event;
                event.timestamp = static_cast<int64_t>(stock.mddate) * 1000000000LL + stock.mdtime;
                event.applseqnum = -1;  // TICK数据使用-1确保排在ORDER/TRANSACTION之前
                event.type = MarketEventType::TICK;
                event.data = stock;
                shard_events_[shard_id].push_back(std::move(event));
            }
        }
        return true;
    }

    bool load_order_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            MDOrderStruct order = {};
            if (parse_order_line(line, order)) {
                int shard_id = get_shard_id(order.htscsecurityid);
                MarketEvent event;
                event.timestamp = static_cast<int64_t>(order.mddate) * 1000000000LL + order.mdtime;
                event.applseqnum = order.applseqnum;
                event.type = MarketEventType::ORDER;
                event.data = order;
                shard_events_[shard_id].push_back(std::move(event));
            }
        }
        return true;
    }

    bool load_transaction_file(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            MDTransactionStruct txn = {};
            if (parse_transaction_line(line, txn)) {
                int shard_id = get_shard_id(txn.htscsecurityid);
                MarketEvent event;
                event.timestamp = static_cast<int64_t>(txn.mddate) * 1000000000LL + txn.mdtime;
                event.applseqnum = txn.applseqnum;
                event.type = MarketEventType::TRANSACTION;
                event.data = txn;
                shard_events_[shard_id].push_back(std::move(event));
            }
        }
        return true;
    }

    void set_tick_callback(std::function<void(const MDStockStruct&)> cb) {
        tick_callback_ = std::move(cb);
    }

    void set_order_callback(std::function<void(const MDOrderStruct&)> cb) {
        order_callback_ = std::move(cb);
    }

    void set_transaction_callback(std::function<void(const MDTransactionStruct&)> cb) {
        transaction_callback_ = std::move(cb);
    }

    void replay() {
        // Sort events within each shard
        for (auto& events : shard_events_) {
            std::sort(events.begin(), events.end());
        }

        // Launch one thread per shard for concurrent replay
        std::vector<std::thread> threads;
        for (int i = 0; i < shard_count_; ++i) {
            threads.emplace_back([this, i]() {
                for (const auto& event : shard_events_[i]) {
                    if (event.type == MarketEventType::TICK) {
                        if (tick_callback_) {
                            tick_callback_(std::get<MDStockStruct>(event.data));
                        }
                    } else if (event.type == MarketEventType::ORDER) {
                        if (order_callback_) {
                            order_callback_(std::get<MDOrderStruct>(event.data));
                        }
                    } else {
                        if (transaction_callback_) {
                            transaction_callback_(std::get<MDTransactionStruct>(event.data));
                        }
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }
    }

    size_t event_count() const {
        size_t total = 0;
        for (const auto& events : shard_events_) {
            total += events.size();
        }
        return total;
    }

private:
    int shard_count_;
    std::vector<std::vector<MarketEvent>> shard_events_;
    std::function<void(const MDStockStruct&)> tick_callback_;
    std::function<void(const MDOrderStruct&)> order_callback_;
    std::function<void(const MDTransactionStruct&)> transaction_callback_;

    int get_shard_id(const char* symbol) {
        uint64_t hash = 0;
        for (const char* p = symbol; *p && (p - symbol) < 40; ++p) {
            hash = hash * 31 + static_cast<unsigned char>(*p);
        }
        return static_cast<int>(hash % shard_count_);
    }

    std::string extract_string_value(const std::string& line, const std::string& key) {
        size_t pos = line.find(key + ": ");
        if (pos == std::string::npos) return "";
        pos += key.length() + 2;

        if (pos < line.length() && line[pos] == '"') {
            // Quoted string
            size_t end = line.find('"', pos + 1);
            if (end != std::string::npos) {
                return line.substr(pos + 1, end - pos - 1);
            }
        } else {
            // Unquoted value
            size_t end = line.find(' ', pos);
            if (end == std::string::npos) end = line.length();
            return line.substr(pos, end - pos);
        }
        return "";
    }

    int64_t extract_int_value(const std::string& line, const std::string& key) {
        std::string val = extract_string_value(line, key);
        if (val.empty()) return 0;
        return std::stoll(val);
    }

    // 解析重复key数组字段，格式: key: val1 key: val2 key: val3 ...
    void extract_repeated_int_array(const std::string& line, const std::string& key, int64_t* arr, size_t max_size) {
        std::string search_key = key + ": ";
        size_t pos = 0;
        size_t idx = 0;

        while (idx < max_size && (pos = line.find(search_key, pos)) != std::string::npos) {
            pos += search_key.length();
            size_t end = line.find(' ', pos);
            if (end == std::string::npos) end = line.length();
            std::string val = line.substr(pos, end - pos);
            if (!val.empty()) {
                arr[idx++] = std::stoll(val);
            }
            pos = end;
        }
    }

    // 处理SecurityIDSource枚举（可能是名称或数字）
    int32_t extract_securityidsource(const std::string& line, const std::string& key) {
        std::string val = extract_string_value(line, key);
        if (val.empty()) return 0;
        return MarketDataEnums::GetSecurityIDSourceValue(val);
    }

    // 处理SecurityType枚举（可能是名称或数字）
    int32_t extract_securitytype(const std::string& line, const std::string& key) {
        std::string val = extract_string_value(line, key);
        if (val.empty()) return 0;
        return MarketDataEnums::GetSecurityTypeValue(val);
    }

    bool parse_order_line(const std::string& line, MDOrderStruct& order) {
        std::string security_id = extract_string_value(line, "HTSCSecurityID");
        if (security_id.empty()) return false;

        std::strncpy(order.htscsecurityid, security_id.c_str(), sizeof(order.htscsecurityid) - 1);
        order.htscsecurityid[sizeof(order.htscsecurityid) - 1] = '\0';

        order.mddate = static_cast<int32_t>(extract_int_value(line, "MDDate"));
        order.mdtime = static_cast<int32_t>(extract_int_value(line, "MDTime"));
        order.securityidsource = extract_securityidsource(line, "securityIDSource");
        order.securitytype = extract_securitytype(line, "securityType");
        order.orderindex = extract_int_value(line, "OrderIndex");
        order.ordertype = static_cast<int32_t>(extract_int_value(line, "OrderType"));
        order.orderprice = extract_int_value(line, "OrderPrice");
        order.orderqty = extract_int_value(line, "OrderQty");
        order.orderbsflag = static_cast<int32_t>(extract_int_value(line, "OrderBSFlag"));
        order.channelno = static_cast<int32_t>(extract_int_value(line, "ChannelNo"));
        order.orderno = extract_int_value(line, "OrderNO");
        order.applseqnum = extract_int_value(line, "ApplSeqNum");
        order.datamultiplepowerof10 = static_cast<int32_t>(extract_int_value(line, "DataMultiplePowerOf10"));
        order.tradedqty = extract_int_value(line, "TradedQty");

        // 证券状态（上海专有）
        std::string security_status = extract_string_value(line, "SecurityStatus");
        std::strncpy(order.securitystatus, security_status.c_str(), sizeof(order.securitystatus) - 1);
        order.securitystatus[sizeof(order.securitystatus) - 1] = '\0';

        return true;
    }

    bool parse_transaction_line(const std::string& line, MDTransactionStruct& txn) {
        std::string security_id = extract_string_value(line, "HTSCSecurityID");
        if (security_id.empty()) return false;

        std::strncpy(txn.htscsecurityid, security_id.c_str(), sizeof(txn.htscsecurityid) - 1);
        txn.htscsecurityid[sizeof(txn.htscsecurityid) - 1] = '\0';

        txn.mddate = static_cast<int32_t>(extract_int_value(line, "MDDate"));
        txn.mdtime = static_cast<int32_t>(extract_int_value(line, "MDTime"));
        txn.securityidsource = extract_securityidsource(line, "securityIDSource");
        txn.securitytype = extract_securitytype(line, "securityType");
        txn.tradeindex = extract_int_value(line, "TradeIndex");
        txn.tradebuyno = extract_int_value(line, "TradeBuyNo");
        txn.tradesellno = extract_int_value(line, "TradeSellNo");
        txn.tradebsflag = static_cast<int32_t>(extract_int_value(line, "TradeBSFlag"));
        txn.tradeprice = extract_int_value(line, "TradePrice");
        txn.tradeqty = extract_int_value(line, "TradeQty");
        txn.trademoney = extract_int_value(line, "TradeMoney");
        txn.applseqnum = extract_int_value(line, "ApplSeqNum");
        txn.channelno = static_cast<int32_t>(extract_int_value(line, "ChannelNo"));
        txn.datamultiplepowerof10 = static_cast<int32_t>(extract_int_value(line, "DataMultiplePowerOf10"));

        return true;
    }

    bool parse_tick_line(const std::string& line, MDStockStruct& stock) {
        std::string security_id = extract_string_value(line, "HTSCSecurityID");
        if (security_id.empty()) return false;

        std::strncpy(stock.htscsecurityid, security_id.c_str(), sizeof(stock.htscsecurityid) - 1);
        stock.htscsecurityid[sizeof(stock.htscsecurityid) - 1] = '\0';

        stock.mddate = static_cast<int32_t>(extract_int_value(line, "MDDate"));
        stock.mdtime = static_cast<int32_t>(extract_int_value(line, "MDTime"));
        stock.datatimestamp = extract_int_value(line, "DataTimeStamp");

        // 交易阶段
        std::string phase_code = extract_string_value(line, "TradingPhaseCode");
        stock.tradingphasecode = phase_code.empty() ? ' ' : phase_code[0];

        stock.securityidsource = extract_securityidsource(line, "securityIDSource");
        stock.securitytype = extract_securitytype(line, "securityType");

        // 价格限制和昨收价（关键字段）
        stock.maxpx = extract_int_value(line, "MaxPx");
        stock.minpx = extract_int_value(line, "MinPx");
        stock.preclosepx = extract_int_value(line, "PreClosePx");

        // 当前价格数据
        stock.lastpx = extract_int_value(line, "LastPx");
        stock.openpx = extract_int_value(line, "OpenPx");
        stock.closepx = extract_int_value(line, "ClosePx");
        stock.highpx = extract_int_value(line, "HighPx");
        stock.lowpx = extract_int_value(line, "LowPx");

        // 交易统计
        stock.numtrades = extract_int_value(line, "NumTrades");
        stock.totalvolumetrade = extract_int_value(line, "TotalVolumeTrade");
        stock.totalvaluetrade = extract_int_value(line, "TotalValueTrade");

        stock.channelno = static_cast<int32_t>(extract_int_value(line, "ChannelNo"));
        stock.datamultiplepowerof10 = static_cast<int32_t>(extract_int_value(line, "DataMultiplePowerOf10"));

        // 买卖盘口（10档）
        extract_repeated_int_array(line, "BuyPriceQueue", stock.buypricequeue, 10);
        extract_repeated_int_array(line, "BuyOrderQtyQueue", stock.buyorderqtyqueue, 10);
        extract_repeated_int_array(line, "SellPriceQueue", stock.sellpricequeue, 10);
        extract_repeated_int_array(line, "SellOrderQtyQueue", stock.sellorderqtyqueue, 10);

        return true;
    }
};

#endif // HISTORY_DATA_REPLAYER_H
