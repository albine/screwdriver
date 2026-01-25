#pragma once

/**
 * @file logger.h
 * @brief 高频交易系统日志模块
 *
 * 基于Quill异步日志库，针对HFT场景优化：
 * - RDTSC时间戳，热路径延迟<15ns
 * - 后台线程处理格式化和I/O
 * - 纳秒精度人类可读时间格式
 */

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <string>
#include <memory>

namespace hft {
namespace logger {

// ============ 编译期日志等级控制 ============
// 生产环境编译时禁用DEBUG: -DQUILL_COMPILE_ACTIVE_LOG_LEVEL=QUILL_LOG_LEVEL_INFO

// ============ 日志模块标识 ============
// 使用宏简化模块日志调用
#define LOG_MODULE_DEBUG(logger, module, fmt, ...) \
    LOG_DEBUG(logger, "[" module "] " fmt, ##__VA_ARGS__)

#define LOG_MODULE_INFO(logger, module, fmt, ...) \
    LOG_INFO(logger, "[" module "] " fmt, ##__VA_ARGS__)

#define LOG_MODULE_WARNING(logger, module, fmt, ...) \
    LOG_WARNING(logger, "[" module "] " fmt, ##__VA_ARGS__)

#define LOG_MODULE_ERROR(logger, module, fmt, ...) \
    LOG_ERROR(logger, "[" module "] " fmt, ##__VA_ARGS__)

// ============ 常用模块名称 ============
#define MOD_ENGINE    "Engine"
#define MOD_ORDERBOOK "OrderBook"
#define MOD_STRATEGY  "Strategy"
#define MOD_GATEWAY   "Gateway"
#define MOD_MARKET    "Market"

// ============ 业务日志类型标识（4字符固定宽度） ============
#define BIZ_SESS "SESS"  // 会话（登录/登出/心跳）
#define BIZ_ORDR "ORDR"  // 下单
#define BIZ_FILL "FILL"  // 成交回报
#define BIZ_CNCL "CNCL"  // 撤单
#define BIZ_RJCT "RJCT"  // 拒绝
#define BIZ_POSN "POSN"  // 持仓变化
#define BIZ_ACCT "ACCT"  // 账户/资金
#define BIZ_STRA "STRA"  // 策略触发

// 业务日志宏（自动使用业务日志器）
#define LOG_BIZ(type, fmt, ...) \
    LOG_INFO(hft::logger::get_biz_logger(), "[" type "] " fmt, ##__VA_ARGS__)

// ============ 文件级模块日志宏 ============
// 使用方法：在.cpp文件任意位置定义 LOG_MODULE，然后使用 LOG_M_* 宏
// 例如：
//   #include "logger.h"
//   #define LOG_MODULE MOD_ORDERBOOK
//   LOG_M_INFO("price={}", 100);  // 输出: [OrderBook] price=100
//
#define LOG_M_DEBUG(fmt, ...)   LOG_MODULE_DEBUG(hft::logger::get_logger(), LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_M_INFO(fmt, ...)    LOG_MODULE_INFO(hft::logger::get_logger(), LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_M_WARNING(fmt, ...) LOG_MODULE_WARNING(hft::logger::get_logger(), LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_M_ERROR(fmt, ...)   LOG_MODULE_ERROR(hft::logger::get_logger(), LOG_MODULE, fmt, ##__VA_ARGS__)

/**
 * @brief 日志配置参数
 */
struct LogConfig {
    std::string log_dir = "logs";           // 日志目录
    std::string log_file = "trading.log";   // 日志文件名
    size_t max_file_size = 100 * 1024 * 1024;  // 单文件最大100MB
    size_t max_backup_files = 10;           // 最多保留10个备份
    bool console_output = true;             // 是否输出到控制台
    bool use_rdtsc = true;                  // 使用RDTSC时间戳
    quill::LogLevel log_level = quill::LogLevel::Info;  // 日志等级（回测用Debug，生产用Info）
};

/**
 * @brief 初始化日志系统
 *
 * @param config 日志配置
 * @return quill::Logger* 主日志器指针
 *
 * 特性：
 * - RDTSC时间戳，热路径延迟<15ns
 * - 纳秒精度时间格式
 * - 日志文件轮转
 * - 崩溃信号处理
 */
inline quill::Logger* init(const LogConfig& config = LogConfig{}) {
    // 后台线程配置
    quill::BackendOptions backend_options;
    // RDTSC重同步间隔（默认500ms）
    backend_options.rdtsc_resync_interval = std::chrono::milliseconds{500};

    // 自定义日志等级名称（统一4字符）
    // 索引: 0=TRACE_L3, 1=TRACE_L2, 2=TRACE_L1, 3=DEBUG, 4=INFO, 5=NOTICE, 6=WARNING, 7=ERROR, 8=CRITICAL, 9=BACKTRACE, 10=NONE, 11=DYNAMIC
    backend_options.log_level_descriptions[3] = "DBUG";  // DEBUG -> DBUG
    backend_options.log_level_descriptions[4] = "INFO";  // INFO (保持)
    backend_options.log_level_descriptions[6] = "WARN";  // WARNING -> WARN
    backend_options.log_level_descriptions[7] = "ERR!";  // ERROR -> ERR!

    // 允许 UTF-8 字符输出（默认只允许 ASCII 0x20-0x7E）
    backend_options.check_printable_char = {};

    // 启动后台线程
    quill::Backend::start(backend_options);

    // 日志格式：纳秒精度时间戳 + 等级 + 线程ID + 消息
    // 格式: 2025-01-11 09:30:00.123456789 [INFO] [12345] message
    std::string pattern =
        "%(time) [%(thread_id)] [%(log_level)] %(message)";
    std::string time_format = "%Y-%m-%d %H:%M:%S.%Qns";  // 纳秒精度

    std::vector<std::shared_ptr<quill::Sink>> sinks;

    // 控制台输出
    if (config.console_output) {
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
        sinks.push_back(console_sink);
    }

    // 文件轮转输出
    std::string log_path = config.log_dir + "/" + config.log_file;
    quill::RotatingFileSinkConfig file_config;
    file_config.set_open_mode('w');
    file_config.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
    file_config.set_rotation_max_file_size(config.max_file_size);
    file_config.set_max_backup_files(static_cast<uint32_t>(config.max_backup_files));

    auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
        log_path,
        file_config
    );
    sinks.push_back(file_sink);

    // 创建主日志器（默认使用TSC时钟，即RDTSC）
    // ClockSourceType::Tsc 是默认值
    quill::Logger* logger = quill::Frontend::create_or_get_logger(
        "root",
        std::move(sinks),
        quill::PatternFormatterOptions{pattern, time_format}
    );

    // 设置日志等级
    logger->set_log_level(config.log_level);

    LOG_INFO(logger, "Logger initialized, rdtsc={}, debug_enabled={}", config.use_rdtsc,
             config.log_level == quill::LogLevel::Debug);
    return logger;
}

/**
 * @brief 获取根日志器
 */
inline quill::Logger* get_logger() {
    return quill::Frontend::get_logger("root");
}

/**
 * @brief 刷新所有日志到磁盘
 */
inline void flush() {
    quill::Frontend::get_logger("root")->flush_log();
}

/**
 * @brief 停止日志系统
 */
inline void shutdown() {
    flush();
    quill::Backend::stop();
}

// ============ 业务日志器 ============

/**
 * @brief 业务日志配置参数
 */
struct BizLogConfig {
    std::string log_dir = "logs";              // 日志目录
    std::string log_file = "business.log";     // 业务日志文件名
    size_t max_file_size = 100 * 1024 * 1024;  // 单文件最大100MB
    size_t max_backup_files = 10;              // 最多保留10个备份
    bool console_output = false;               // 业务日志默认不输出到控制台
};

/**
 * @brief 初始化业务日志器
 *
 * @param config 业务日志配置
 * @return quill::Logger* 业务日志器指针
 *
 * 业务日志独立于程序日志，用于记录：
 * - 登录/登出
 * - 下单/撤单
 * - 成交回报
 * - 持仓/资金变化
 */
inline quill::Logger* init_biz(const BizLogConfig& config = BizLogConfig{}) {
    // 业务日志格式：纳秒精度时间戳 + 消息（不需要日志等级和线程ID）
    // 格式: 2025-01-11 09:30:00.123456789 [ORDR] 下单 id=1001 ...
    std::string pattern = "%(time) %(message)";
    std::string time_format = "%Y-%m-%d %H:%M:%S.%Qns";

    std::vector<std::shared_ptr<quill::Sink>> sinks;

    // 控制台输出（可选）
    if (config.console_output) {
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("biz_console");
        sinks.push_back(console_sink);
    }

    // 文件轮转输出
    std::string log_path = config.log_dir + "/" + config.log_file;
    quill::RotatingFileSinkConfig file_config;
    file_config.set_open_mode('w');
    file_config.set_filename_append_option(quill::FilenameAppendOption::StartDateTime);
    file_config.set_rotation_max_file_size(config.max_file_size);
    file_config.set_max_backup_files(static_cast<uint32_t>(config.max_backup_files));

    auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
        log_path,
        file_config
    );
    sinks.push_back(file_sink);

    // 创建业务日志器
    quill::Logger* biz_logger = quill::Frontend::create_or_get_logger(
        "biz",
        std::move(sinks),
        quill::PatternFormatterOptions{pattern, time_format}
    );

    return biz_logger;
}

/**
 * @brief 获取业务日志器
 */
inline quill::Logger* get_biz_logger() {
    return quill::Frontend::get_logger("biz");
}

// ============ 类型别名（隐藏 quill 实现细节） ============
using Logger = quill::Logger;

}  // namespace logger
}  // namespace hft
