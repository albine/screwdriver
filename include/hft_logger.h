/**
 * @file hft_logger.h
 * @brief High-Performance Async Logger for HFT Systems (基于 Quill)
 *
 * Quill 特性：
 * - 前端延迟仅 ~7-20 纳秒（业界最低）
 * - 使用 SPSC 无锁队列
 * - 编译时格式检查
 * - 后端线程异步写入，不阻塞热路径
 *
 * @see https://github.com/odygrd/quill
 */

#ifndef HFT_LOGGER_H
#define HFT_LOGGER_H

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

#include <string>
#include <chrono>
#include <cstdint>

namespace hft {

// ============================================================================
// HFT 专用日志类别常量
// ============================================================================
namespace LogCategory {
    constexpr const char* MARKET_DATA = "MKT_DATA";     // 行情数据
    constexpr const char* ORDER       = "ORDER";        // 订单管理
    constexpr const char* TRADE       = "TRADE";        // 成交
    constexpr const char* STRATEGY    = "STRATEGY";     // 策略
    constexpr const char* RISK        = "RISK";         // 风控
    constexpr const char* NETWORK     = "NETWORK";      // 网络
    constexpr const char* SYSTEM      = "SYSTEM";       // 系统
    constexpr const char* LATENCY     = "LATENCY";      // 延迟监控
}

// ============================================================================
// 日志级别映射
// ============================================================================
// Quill 日志级别:
// - quill::LogLevel::TraceL3, TraceL2, TraceL1
// - quill::LogLevel::Debug
// - quill::LogLevel::Info
// - quill::LogLevel::Warning
// - quill::LogLevel::Error
// - quill::LogLevel::Critical

// ============================================================================
// HFT Logger 封装类
// ============================================================================
class HftLogger {
public:
    struct Config {
        quill::LogLevel min_level = quill::LogLevel::Debug;
        bool enable_console = true;
        std::string log_file_path = "";                 // 空则不写文件
        std::string log_pattern =
            "[%(time)] [%(log_level:<8)] [%(thread_id)] [%(logger:<12)] "
            "[%(file_name):%(line_number)] %(message)";
        std::string timestamp_pattern = "%H:%M:%S.%Qns"; // 纳秒级时间戳
    };

    // 单例访问
    static HftLogger& instance() {
        static HftLogger logger;
        return logger;
    }

    // 初始化
    void init(const Config& config = Config{}) {
        config_ = config;

        // 配置后端线程
        quill::BackendOptions backend_options;
        backend_options.thread_name = "HftLoggerBackend";
        // 使用忙等待以获得最低延迟（可选，会增加 CPU 使用）
        // backend_options.sleep_duration = std::chrono::nanoseconds{0};

        quill::Backend::start(backend_options);

        // 创建 Sinks
        std::vector<std::shared_ptr<quill::Sink>> sinks;

        if (config.enable_console) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>(
                "console_sink",
                true  // 启用颜色
            );
            sinks.push_back(console_sink);
        }

        if (!config.log_file_path.empty()) {
            auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
                config.log_file_path,
                []() {
                    quill::FileSinkConfig cfg;
                    cfg.set_open_mode('w');
                    cfg.set_filename_append_option(
                        quill::FilenameAppendOption::StartDateTime);
                    return cfg;
                }(),
                quill::FileEventNotifier{}
            );
            sinks.push_back(file_sink);
        }

        // 创建格式化模式
        auto pattern = quill::PatternFormatterOptions{
            config.log_pattern,
            config.timestamp_pattern
        };

        // 创建各个类别的 Logger
        create_category_logger(LogCategory::MARKET_DATA, sinks, pattern);
        create_category_logger(LogCategory::ORDER, sinks, pattern);
        create_category_logger(LogCategory::TRADE, sinks, pattern);
        create_category_logger(LogCategory::STRATEGY, sinks, pattern);
        create_category_logger(LogCategory::RISK, sinks, pattern);
        create_category_logger(LogCategory::NETWORK, sinks, pattern);
        create_category_logger(LogCategory::SYSTEM, sinks, pattern);
        create_category_logger(LogCategory::LATENCY, sinks, pattern);

        // 创建默认 Logger
        default_logger_ = quill::Frontend::create_or_get_logger(
            "default", sinks, pattern);
        default_logger_->set_log_level(config.min_level);

        initialized_ = true;
    }

    // 获取指定类别的 Logger
    quill::Logger* get_logger(const char* category) {
        return quill::Frontend::get_logger(category);
    }

    // 获取默认 Logger
    quill::Logger* get_default_logger() {
        return default_logger_;
    }

    // 设置日志级别（运行时可调整）
    void set_level(quill::LogLevel level) {
        config_.min_level = level;
        // 更新所有 logger 的级别
        const char* categories[] = {
            LogCategory::MARKET_DATA, LogCategory::ORDER, LogCategory::TRADE,
            LogCategory::STRATEGY, LogCategory::RISK, LogCategory::NETWORK,
            LogCategory::SYSTEM, LogCategory::LATENCY
        };
        for (const char* cat : categories) {
            auto* logger = quill::Frontend::get_logger(cat);
            if (logger) {
                logger->set_log_level(level);
            }
        }
        if (default_logger_) {
            default_logger_->set_log_level(level);
        }
    }

    // 刷新所有日志
    void flush() {
        if (default_logger_) {
            default_logger_->flush_log();
        }
    }

    // 停止后端线程
    void shutdown() {
        flush();
        quill::Backend::stop();
    }

    bool is_initialized() const { return initialized_; }

private:
    HftLogger() = default;
    ~HftLogger() {
        if (initialized_) {
            shutdown();
        }
    }

    HftLogger(const HftLogger&) = delete;
    HftLogger& operator=(const HftLogger&) = delete;

    void create_category_logger(
        const char* category,
        const std::vector<std::shared_ptr<quill::Sink>>& sinks,
        const quill::PatternFormatterOptions& pattern
    ) {
        auto* logger = quill::Frontend::create_or_get_logger(category, sinks, pattern);
        logger->set_log_level(config_.min_level);
    }

    Config config_;
    quill::Logger* default_logger_ = nullptr;
    bool initialized_ = false;
};

// ============================================================================
// 便捷初始化函数
// ============================================================================
inline void init_logger(const HftLogger::Config& config = HftLogger::Config{}) {
    HftLogger::instance().init(config);
}

inline void shutdown_logger() {
    HftLogger::instance().shutdown();
}

// ============================================================================
// 获取 Logger 的便捷宏
// ============================================================================
#define HFT_LOGGER(category) hft::HftLogger::instance().get_logger(category)

// ============================================================================
// 日志宏定义 - 使用 Quill 的高性能日志宏
// ============================================================================

// DEBUG 级别
#define LOG_DEBUG(category, fmt, ...) \
    LOG_DEBUG_L1(HFT_LOGGER(category), fmt, ##__VA_ARGS__)

// INFO 级别
#define LOG_INFO(category, fmt, ...) \
    LOG_INFO_L1(HFT_LOGGER(category), fmt, ##__VA_ARGS__)

// WARNING 级别
#define LOG_WARNING(category, fmt, ...) \
    LOG_WARNING_L1(HFT_LOGGER(category), fmt, ##__VA_ARGS__)

// ERROR 级别
#define LOG_ERROR(category, fmt, ...) \
    LOG_ERROR_L1(HFT_LOGGER(category), fmt, ##__VA_ARGS__)

// CRITICAL 级别
#define LOG_CRITICAL(category, fmt, ...) \
    LOG_CRITICAL_L1(HFT_LOGGER(category), fmt, ##__VA_ARGS__)

// ============================================================================
// 延迟测量工具（用于性能监控）
// ============================================================================
class LatencyTracker {
public:
    explicit LatencyTracker(const char* operation)
        : operation_(operation)
        , start_(std::chrono::high_resolution_clock::now()) {}

    ~LatencyTracker() {
        auto end = std::chrono::high_resolution_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start_).count();

        // 根据延迟选择日志级别
        if (latency_ns > 1'000'000) { // > 1ms
            LOG_WARNING(LogCategory::LATENCY, "{} took {}us",
                operation_, latency_ns / 1000);
        } else if (latency_ns > 100'000) { // > 100us
            LOG_INFO(LogCategory::LATENCY, "{} took {}us",
                operation_, latency_ns / 1000);
        } else {
            LOG_DEBUG(LogCategory::LATENCY, "{} took {}ns",
                operation_, latency_ns);
        }
    }

private:
    const char* operation_;
    std::chrono::high_resolution_clock::time_point start_;
};

#define TRACK_LATENCY(name) hft::LatencyTracker _latency_tracker_##__LINE__(name)

// ============================================================================
// 高精度时间戳工具
// ============================================================================
struct Timestamp {
    int64_t nanos_since_epoch;

    static Timestamp now() {
        auto now = std::chrono::high_resolution_clock::now();
        auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
        return {nanos};
    }

    int64_t elapsed_ns(const Timestamp& other) const {
        return nanos_since_epoch - other.nanos_since_epoch;
    }
};

} // namespace hft

#endif // HFT_LOGGER_H
