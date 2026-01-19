#ifndef BACKTEST_CONFIG_H
#define BACKTEST_CONFIG_H

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <set>
#include "utils/symbol_utils.h"

// ==========================================
// 策略配置结构（回测和实盘通用）
// ==========================================
struct StrategyConfigEntry {
    std::string symbol;         // 股票代码，如 "600759.SH"
    std::string strategy_name;  // 策略名称，如 "BreakoutPriceVolumeStrategy"
    std::string params;         // 可选参数，如突破价格 "98500"
};

// 向后兼容的别名
using BacktestEntry = StrategyConfigEntry;

// ==========================================
// 配置文件解析器（回测和实盘通用）
// ==========================================
inline std::vector<StrategyConfigEntry> parse_strategy_config(const std::string& filepath) {
    std::vector<StrategyConfigEntry> entries;
    std::set<std::string> seen_keys;  // 用于检测重复的 (symbol, strategy) 组合
    std::ifstream file(filepath);

    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        line_num++;

        // 跳过空行和注释
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // 去除首尾空白
        auto start = line.find_first_not_of(" \t");
        auto end = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        line = line.substr(start, end - start + 1);

        // 解析 "股票代码,策略名称[,可选参数]"
        std::istringstream iss(line);
        std::string symbol, strategy, params;

        if (std::getline(iss, symbol, ',') && std::getline(iss, strategy, ',')) {
            // 尝试读取可选参数
            std::getline(iss, params);

            // 去除字段首尾空白
            auto trim = [](std::string& s) {
                auto start = s.find_first_not_of(" \t");
                auto end = s.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    s = s.substr(start, end - start + 1);
                } else {
                    s.clear();
                }
            };
            trim(symbol);
            trim(strategy);
            trim(params);

            if (!symbol.empty() && !strategy.empty()) {
                std::string normalized_symbol = symbol_utils::normalize_symbol(symbol);
                std::string key = normalized_symbol + "|" + strategy;

                // 检测重复配置
                if (seen_keys.count(key) > 0) {
                    std::fprintf(stderr, "WARNING: Duplicate config at line %d: %s,%s (skipped)\n",
                                 line_num, normalized_symbol.c_str(), strategy.c_str());
                    continue;
                }
                seen_keys.insert(key);

                StrategyConfigEntry entry;
                entry.symbol = normalized_symbol;
                entry.strategy_name = strategy;
                entry.params = params;
                entries.push_back(entry);
            }
        }
    }

    return entries;
}

// 向后兼容的函数别名
inline std::vector<StrategyConfigEntry> parse_backtest_config(const std::string& filepath) {
    return parse_strategy_config(filepath);
}

// ==========================================
// 引擎配置结构
// ==========================================
struct EngineConfig {
    // ZMQ 配置
    std::string zmq_endpoint = "tcp://localhost:13380";   // DEALER1 连接的 ROUTER 地址
    std::string zmq_endpoint2 = "tcp://localhost:13381";  // DEALER2 连接的 ROUTER 地址
    bool disable_zmq = false;

    // 持久化配置
    bool disable_persist = false;
    std::string persist_data_dir = "data/raw";
};

// ==========================================
// 引擎配置解析器
// ==========================================
inline EngineConfig parse_engine_config(const std::string& filepath) {
    EngineConfig config;
    std::ifstream file(filepath);

    if (!file.is_open()) {
        return config;  // 返回默认值
    }

    std::string line;
    while (std::getline(file, line)) {
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // 去除首尾空白
        auto start = line.find_first_not_of(" \t");
        auto end = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        line = line.substr(start, end - start + 1);

        // 解析 key=value
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // 去除 key 和 value 的首尾空白
        auto trim = [](std::string& s) {
            auto start = s.find_first_not_of(" \t");
            auto end = s.find_last_not_of(" \t");
            if (start != std::string::npos) {
                s = s.substr(start, end - start + 1);
            } else {
                s.clear();
            }
        };
        trim(key);
        trim(value);

        // 设置配置值
        if (key == "zmq_endpoint") {
            config.zmq_endpoint = value;
        } else if (key == "zmq_endpoint2" || key == "zmq_rep_endpoint") {
            config.zmq_endpoint2 = value;
        } else if (key == "disable_zmq") {
            config.disable_zmq = (value == "true" || value == "1");
        } else if (key == "disable_persist") {
            config.disable_persist = (value == "true" || value == "1");
        } else if (key == "persist_data_dir") {
            config.persist_data_dir = value;
        }
    }

    return config;
}

// ==========================================
// 数据文件检测与下载
// ==========================================
inline bool check_data_exists(const std::string& symbol, const std::string& data_dir = "test_data") {
    std::vector<std::string> required_files = {
        data_dir + "/MD_TICK_StockType_" + symbol + ".csv",
        data_dir + "/MD_ORDER_StockType_" + symbol + ".csv",
        data_dir + "/MD_TRANSACTION_StockType_" + symbol + ".csv"
    };

    for (const auto& file : required_files) {
        if (!std::filesystem::exists(file)) {
            return false;
        }
    }
    return true;
}

inline bool download_market_data(const std::string& symbol, const std::string& script_path = "script/download_market_data.sh") {
    if (!std::filesystem::exists(script_path)) {
        return false;
    }

    std::string cmd = script_path + " " + symbol;
    int ret = std::system(cmd.c_str());
    return (ret == 0);
}

inline bool ensure_data_exists(const std::string& symbol,
                               const std::string& data_dir = "test_data",
                               const std::string& script_path = "script/download_market_data.sh") {
    if (check_data_exists(symbol, data_dir)) {
        return true;
    }

    // 尝试下载
    if (download_market_data(symbol, script_path)) {
        return check_data_exists(symbol, data_dir);
    }

    return false;
}

#endif // BACKTEST_CONFIG_H
