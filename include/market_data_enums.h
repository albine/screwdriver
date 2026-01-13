#ifndef MARKET_DATA_ENUMS_H
#define MARKET_DATA_ENUMS_H

#include <string>
#include <unordered_map>
#include <stdexcept>

// ============================================================================
// 市场数据枚举值映射
// ============================================================================
// 这些枚举值来自 fastfish/src/protobuf/ESecurityIDSource.pb.h 和 ESecurityType.pb.h

namespace MarketDataEnums {

// SecurityIDSource 枚举名称到数值的映射
static const std::unordered_map<std::string, int32_t> SecurityIDSourceMap = {
    {"DefaultSecurityIDSource", 0},
    {"XSHG", 101},   // 上海证券交易所
    {"XSHE", 102},   // 深圳证券交易所
    {"NEEQ", 103},   // 全国中小企业股份转让系统（新三板）
    {"XSHGFI", 104}, // 上海固定收益
    {"XSHECA", 105}, // 上海现金拍卖
    {"XBSE", 106},   // 北京证券交易所
    {"XSHGFC", 107}, // 上海期货合约
    {"XSHEFC", 108}, // 深圳期货合约
    {"XHKG", 203},   // 香港交易所
    {"HKSC", 204},
    {"HGHQ", 205},
    {"CCFX", 301},   // 中金所
    {"XSGE", 302},   // 上海黄金交易所
    {"INE", 303},    // 上海国际能源交易中心
    {"SGEX", 401},
    {"XCFE", 501},   // 广州期货交易所
    {"CCDC", 502},
    {"CNEX", 503},
    {"XDCE", 601},   // 大连商品交易所
    {"XZCE", 602},   // 郑州商品交易所
    {"XGFE", 603},
    {"SWS", 701},
    {"CNI", 702},
    {"CSI", 703},
    {"HTIS", 801},
    {"MORN", 802},
    {"QB", 803},
    {"SPDB", 804},
    {"HTSM", 805},
    {"SCB", 806},
    {"CUBE", 807},
    {"LSE", 901},
    {"LME", 902},
    {"LIFFE", 903},
    {"ICEU", 904},
    {"BSE", 905},
    {"NSE", 906},
    {"NEX", 907},
    {"APEX", 908},
    {"ICE_SG", 909},
    {"SGX", 910},
    {"TSE", 911},
    {"TOCOM", 912},
    {"OSE", 913},
    {"EUREX", 914},
    {"ICE", 915},
    {"CME", 916},
    {"CBOT", 917},
    {"CBOE", 918},
    {"AMEX", 919},
    {"US", 920},
    {"NYSE", 921},
    {"NYMEX", 922},
    {"COMEX", 923},
    {"ICUS", 924},
    {"NASDAQ", 925},
    {"BBG", 926},
    {"BMD", 927},
    {"LUXSE", 928},
    {"KRX", 929},
    {"MICEX", 930},
    {"ASE", 931},
    {"ISE", 932},
    {"DME", 933},
    {"IHK", 934},
    {"STOXX", 935},
    {"SPI", 936},
    {"NIKKEI", 937},
    {"DJI", 938},
    {"BATS", 939},
    {"IEX", 940},
    {"OPRA", 941},
    {"REFINITIV", 942},
    {"OTCM", 943},
    {"EURONEXT", 944},
    {"FSI", 945},
    {"DBDX", 946},
    {"SAO", 947},
    {"XASX", 948},
    {"XCBO", 949},
    {"XMIL", 950},
    {"XMOD", 951},
    {"XMEF", 952},
    {"XOME", 953},
    {"UST", 954}
};

// SecurityType 枚举名称到数值的映射
static const std::unordered_map<std::string, int32_t> SecurityTypeMap = {
    {"DefaultSecurityType", 0},
    {"IndexType", 1},      // 指数
    {"StockType", 2},      // 股票
    {"FundType", 3},       // 基金
    {"BondType", 4},       // 债券
    {"RepoType", 5},       // 回购
    {"WarrantType", 6},    // 权证
    {"OptionType", 7},     // 期权
    {"FuturesType", 8},    // 期货
    {"ForexType", 9},      // 外汇
    {"RateType", 10},      // 利率
    {"NmetalType", 11},    // 贵金属
    {"CashBondType", 12},  // 现金债券
    {"SpotType", 13},      // 现货
    {"SPFuturesType", 14}, // 股指期货
    {"CurrencyType", 15},  // 货币
    {"BenchmarkType", 16}, // 基准
    {"InsightType", 20},   // 洞察
    {"OtherType", 99}      // 其他
};

// 根据枚举名称或数字字符串获取枚举值
inline int32_t GetSecurityIDSourceValue(const std::string& val) {
    // 尝试直接转换数字
    try {
        return std::stoi(val);
    } catch (const std::invalid_argument&) {
        // 按枚举名称查找
        auto it = SecurityIDSourceMap.find(val);
        return (it != SecurityIDSourceMap.end()) ? it->second : 0;
    }
}

inline int32_t GetSecurityTypeValue(const std::string& val) {
    // 尝试直接转换数字
    try {
        return std::stoi(val);
    } catch (const std::invalid_argument&) {
        // 按枚举名称查找
        auto it = SecurityTypeMap.find(val);
        return (it != SecurityTypeMap.end()) ? it->second : 0;
    }
}

} // namespace MarketDataEnums

#endif // MARKET_DATA_ENUMS_H
