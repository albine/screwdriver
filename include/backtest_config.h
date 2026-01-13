#ifndef BACKTEST_CONFIG_H
#define BACKTEST_CONFIG_H

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

// ==========================================
// 回测配置结构
// ==========================================
struct BacktestEntry {
    std::string symbol;         // 股票代码，如 "600759.SH"
    std::string strategy_name;  // 策略名称，如 "PriceLevelVolumeStrategy"
};

// ==========================================
// 配置文件解析器
// ==========================================
inline std::vector<BacktestEntry> parse_backtest_config(const std::string& filepath) {
    std::vector<BacktestEntry> entries;
    std::ifstream file(filepath);

    if (!file.is_open()) {
        return entries;
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

        // 解析 "股票代码,策略名称"
        std::istringstream iss(line);
        std::string symbol, strategy;

        if (std::getline(iss, symbol, ',') && std::getline(iss, strategy)) {
            // 去除字段首尾空白
            auto trim = [](std::string& s) {
                auto start = s.find_first_not_of(" \t");
                auto end = s.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    s = s.substr(start, end - start + 1);
                }
            };
            trim(symbol);
            trim(strategy);

            if (!symbol.empty() && !strategy.empty()) {
                BacktestEntry entry;
                // 自动补全交易所后缀
                if (symbol.find('.') == std::string::npos) {
                    // 6开头是上海，其他是深圳
                    if (symbol[0] == '6') {
                        symbol += ".SH";
                    } else {
                        symbol += ".SZ";
                    }
                }
                entry.symbol = symbol;
                entry.strategy_name = strategy;
                entries.push_back(entry);
            }
        }
    }

    return entries;
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
