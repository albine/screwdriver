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
#include "market_data_structs.h"

enum class MarketEventType { ORDER, TRANSACTION };

struct MarketEvent {
    int64_t timestamp;       // MDDate * 1e9 + MDTime
    int64_t applseqnum;      // Secondary sort key
    MarketEventType type;
    std::variant<MDOrderStruct, MDTransactionStruct> data;

    bool operator<(const MarketEvent& other) const {
        if (timestamp != other.timestamp) return timestamp < other.timestamp;
        return applseqnum < other.applseqnum;
    }
};

class HistoryDataReplayer {
public:
    explicit HistoryDataReplayer(int shard_count = 4)
        : shard_count_(shard_count), shard_events_(shard_count) {}

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
                    if (event.type == MarketEventType::ORDER) {
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

    bool parse_order_line(const std::string& line, MDOrderStruct& order) {
        std::string security_id = extract_string_value(line, "HTSCSecurityID");
        if (security_id.empty()) return false;

        std::strncpy(order.htscsecurityid, security_id.c_str(), sizeof(order.htscsecurityid) - 1);
        order.htscsecurityid[sizeof(order.htscsecurityid) - 1] = '\0';

        order.mddate = static_cast<int32_t>(extract_int_value(line, "MDDate"));
        order.mdtime = static_cast<int32_t>(extract_int_value(line, "MDTime"));
        order.orderindex = extract_int_value(line, "OrderIndex");
        order.ordertype = static_cast<int32_t>(extract_int_value(line, "OrderType"));
        order.orderprice = extract_int_value(line, "OrderPrice");
        order.orderqty = extract_int_value(line, "OrderQty");
        order.orderbsflag = static_cast<int32_t>(extract_int_value(line, "OrderBSFlag"));
        order.channelno = static_cast<int32_t>(extract_int_value(line, "ChannelNo"));
        order.applseqnum = extract_int_value(line, "ApplSeqNum");
        order.datamultiplepowerof10 = static_cast<int32_t>(extract_int_value(line, "DataMultiplePowerOf10"));

        return true;
    }

    bool parse_transaction_line(const std::string& line, MDTransactionStruct& txn) {
        std::string security_id = extract_string_value(line, "HTSCSecurityID");
        if (security_id.empty()) return false;

        std::strncpy(txn.htscsecurityid, security_id.c_str(), sizeof(txn.htscsecurityid) - 1);
        txn.htscsecurityid[sizeof(txn.htscsecurityid) - 1] = '\0';

        txn.mddate = static_cast<int32_t>(extract_int_value(line, "MDDate"));
        txn.mdtime = static_cast<int32_t>(extract_int_value(line, "MDTime"));
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
};

#endif // HISTORY_DATA_REPLAYER_H
