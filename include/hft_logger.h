/**
 * @file hft_logger.h
 * @brief High-Performance Async Logger for HFT Systems
 *
 * 设计理念：
 * 1. 前端（热路径）：仅做最小工作 - 格式化参数并推入无锁队列
 * 2. 后端（冷路径）：专用线程负责实际 I/O 写入
 * 3. 零拷贝：使用结构化消息避免字符串拷贝
 * 4. 缓存友好：日志消息对齐到缓存行
 */

#ifndef HFT_LOGGER_H
#define HFT_LOGGER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <functional>
#include <ctime>

#include "concurrentqueue.h"

namespace hft {

// ============================================================================
// 日志级别定义
// ============================================================================
enum class LogLevel : uint8_t {
    DEBUG   = 0,  // 调试信息：详细的内部状态，仅开发环境启用
    INFO    = 1,  // 运行信息：正常操作记录，如订单发送、成交确认
    WARNING = 2,  // 警告信息：非致命异常，如延迟超阈值、重连
    ERROR   = 3   // 错误信息：需要立即关注的问题
};

inline const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DBG";
        case LogLevel::INFO:    return "INF";
        case LogLevel::WARNING: return "WRN";
        case LogLevel::ERROR:   return "ERR";
        default:                return "UNK";
    }
}

// ============================================================================
// 高精度时间戳
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

    // 格式化为 HH:MM:SS.nnnnnnnnn
    std::string to_string() const {
        auto seconds = nanos_since_epoch / 1'000'000'000LL;
        auto nanos = nanos_since_epoch % 1'000'000'000LL;

        std::time_t time = static_cast<std::time_t>(seconds);
        std::tm tm_buf;
        localtime_r(&time, &tm_buf);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%09ld",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, nanos);
        return std::string(buf);
    }

    // 格式化为完整日期时间 YYYY-MM-DD HH:MM:SS.nnnnnnnnn
    std::string to_full_string() const {
        auto seconds = nanos_since_epoch / 1'000'000'000LL;
        auto nanos = nanos_since_epoch % 1'000'000'000LL;

        std::time_t time = static_cast<std::time_t>(seconds);
        std::tm tm_buf;
        localtime_r(&time, &tm_buf);

        char buf[48];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%09ld",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, nanos);
        return std::string(buf);
    }
};

// ============================================================================
// 日志消息结构（对齐到缓存行以避免 False Sharing）
// ============================================================================
struct alignas(64) LogMessage {
    Timestamp timestamp;                  // 8 bytes
    LogLevel level;                       // 1 byte
    uint8_t padding1[3];                  // 3 bytes padding
    uint32_t thread_id;                   // 4 bytes
    char source_file[32];                 // 32 bytes (文件名)
    uint32_t source_line;                 // 4 bytes (行号)
    char category[16];                    // 16 bytes (日志类别)
    char message[448];                    // 448 bytes (实际消息)
    // Total: 512 bytes = 8 cache lines

    void set_source(const char* file, uint32_t line) {
        // 只保留文件名，不要完整路径
        const char* filename = file;
        for (const char* p = file; *p; ++p) {
            if (*p == '/' || *p == '\\') filename = p + 1;
        }
        std::strncpy(source_file, filename, sizeof(source_file) - 1);
        source_file[sizeof(source_file) - 1] = '\0';
        source_line = line;
    }

    void set_category(const char* cat) {
        std::strncpy(category, cat, sizeof(category) - 1);
        category[sizeof(category) - 1] = '\0';
    }

    void set_message(const char* msg) {
        std::strncpy(message, msg, sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }
};

static_assert(sizeof(LogMessage) == 512, "LogMessage should be 512 bytes");

// ============================================================================
// 日志输出接口（Sink）
// ============================================================================
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogMessage& msg) = 0;
    virtual void flush() = 0;
};

// 控制台输出（带颜色）
class ConsoleSink : public LogSink {
public:
    void write(const LogMessage& msg) override {
        const char* color = "";
        const char* reset = "\033[0m";

        switch (msg.level) {
            case LogLevel::DEBUG:   color = "\033[36m"; break; // Cyan
            case LogLevel::INFO:    color = "\033[32m"; break; // Green
            case LogLevel::WARNING: color = "\033[33m"; break; // Yellow
            case LogLevel::ERROR:   color = "\033[31m"; break; // Red
        }

        // HFT日志格式:
        // [时间戳] [级别] [线程ID] [类别] [源文件:行号] 消息
        std::fprintf(stdout, "%s[%s] [%s] [T%08X] [%-12s] [%s:%u] %s%s\n",
            color,
            msg.timestamp.to_string().c_str(),
            log_level_to_string(msg.level),
            msg.thread_id,
            msg.category,
            msg.source_file,
            msg.source_line,
            msg.message,
            reset
        );
    }

    void flush() override {
        std::fflush(stdout);
    }
};

// 文件输出
class FileSink : public LogSink {
public:
    explicit FileSink(const std::string& filepath)
        : file_(filepath, std::ios::app | std::ios::out) {
        if (!file_.is_open()) {
            throw std::runtime_error("Failed to open log file: " + filepath);
        }
    }

    void write(const LogMessage& msg) override {
        // 结构化格式，便于后续分析
        // 格式: timestamp|level|thread_id|category|source_file|line|message
        file_ << msg.timestamp.to_full_string() << "|"
              << log_level_to_string(msg.level) << "|"
              << std::hex << std::setfill('0') << std::setw(8) << msg.thread_id << std::dec << "|"
              << msg.category << "|"
              << msg.source_file << "|"
              << msg.source_line << "|"
              << msg.message << "\n";
    }

    void flush() override {
        file_.flush();
    }

private:
    std::ofstream file_;
};

// ============================================================================
// 异步日志器（核心）
// ============================================================================
class AsyncLogger {
public:
    // 单例访问
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    // 初始化配置
    struct Config {
        LogLevel min_level = LogLevel::DEBUG;
        size_t queue_capacity = 65536;        // 队列容量
        size_t flush_interval_ms = 100;       // 刷新间隔
        bool enable_console = true;
        std::string log_file_path = "";       // 空则不写文件
    };

    void init(const Config& config) {
        config_ = config;

        // 创建队列
        queue_ = std::make_unique<moodycamel::ConcurrentQueue<LogMessage>>(config.queue_capacity);

        // 添加 Sinks
        if (config.enable_console) {
            sinks_.push_back(std::make_unique<ConsoleSink>());
        }
        if (!config.log_file_path.empty()) {
            sinks_.push_back(std::make_unique<FileSink>(config.log_file_path));
        }

        // 启动后台写入线程
        running_ = true;
        backend_thread_ = std::thread(&AsyncLogger::backend_loop, this);
    }

    void shutdown() {
        running_ = false;
        if (backend_thread_.joinable()) {
            backend_thread_.join();
        }
        // 最终刷新
        for (auto& sink : sinks_) {
            sink->flush();
        }
    }

    // 获取当前日志级别
    LogLevel get_level() const { return config_.min_level; }

    // 设置日志级别（运行时可调整）
    void set_level(LogLevel level) { config_.min_level = level; }

    // 热路径：提交日志消息
    // 这是性能关键路径，只做最小工作
    void log(LogLevel level, const char* category, const char* file,
             uint32_t line, const char* message) {
        if (level < config_.min_level) return;

        LogMessage msg{};
        msg.timestamp = Timestamp::now();
        msg.level = level;
        msg.thread_id = get_thread_id();
        msg.set_source(file, line);
        msg.set_category(category);
        msg.set_message(message);

        // 无锁入队
        queue_->enqueue(msg);
    }

    // 带格式化的日志（稍慢，但更灵活）
    template<typename... Args>
    void logf(LogLevel level, const char* category, const char* file,
              uint32_t line, const char* fmt, Args&&... args) {
        if (level < config_.min_level) return;

        char buffer[448];
        std::snprintf(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);
        log(level, category, file, line, buffer);
    }

private:
    AsyncLogger() = default;
    ~AsyncLogger() { shutdown(); }

    // 禁止拷贝
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    static uint32_t get_thread_id() {
        // 缓存线程ID，避免每次系统调用
        thread_local uint32_t cached_id = 0;
        if (cached_id == 0) {
            std::hash<std::thread::id> hasher;
            cached_id = static_cast<uint32_t>(hasher(std::this_thread::get_id()));
        }
        return cached_id;
    }

    // 后端线程：负责实际 I/O
    void backend_loop() {
        moodycamel::ConsumerToken token(*queue_);
        LogMessage msg;

        auto last_flush = std::chrono::steady_clock::now();

        while (running_ || queue_->size_approx() > 0) {
            bool has_msg = false;

            // 批量出队以提高效率
            while (queue_->try_dequeue(token, msg)) {
                has_msg = true;
                for (auto& sink : sinks_) {
                    sink->write(msg);
                }
            }

            // 定期刷新
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush);
            if (elapsed.count() >= static_cast<long>(config_.flush_interval_ms) || !running_) {
                for (auto& sink : sinks_) {
                    sink->flush();
                }
                last_flush = now;
            }

            if (!has_msg && running_) {
                // 无消息时短暂休眠，避免空转
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    Config config_;
    std::unique_ptr<moodycamel::ConcurrentQueue<LogMessage>> queue_;
    std::vector<std::unique_ptr<LogSink>> sinks_;
    std::thread backend_thread_;
    std::atomic<bool> running_{false};
};

// ============================================================================
// 便捷宏定义
// ============================================================================

// 基础日志宏
#define HFT_LOG(level, category, msg) \
    hft::AsyncLogger::instance().log(level, category, __FILE__, __LINE__, msg)

#define HFT_LOGF(level, category, fmt, ...) \
    hft::AsyncLogger::instance().logf(level, category, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// 级别快捷宏
#define LOG_DEBUG(category, msg)    HFT_LOG(hft::LogLevel::DEBUG, category, msg)
#define LOG_INFO(category, msg)     HFT_LOG(hft::LogLevel::INFO, category, msg)
#define LOG_WARNING(category, msg)  HFT_LOG(hft::LogLevel::WARNING, category, msg)
#define LOG_ERROR(category, msg)    HFT_LOG(hft::LogLevel::ERROR, category, msg)

// 带格式化的快捷宏
#define LOG_DEBUGF(category, fmt, ...)    HFT_LOGF(hft::LogLevel::DEBUG, category, fmt, ##__VA_ARGS__)
#define LOG_INFOF(category, fmt, ...)     HFT_LOGF(hft::LogLevel::INFO, category, fmt, ##__VA_ARGS__)
#define LOG_WARNINGF(category, fmt, ...)  HFT_LOGF(hft::LogLevel::WARNING, category, fmt, ##__VA_ARGS__)
#define LOG_ERRORF(category, fmt, ...)    HFT_LOGF(hft::LogLevel::ERROR, category, fmt, ##__VA_ARGS__)

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
// 延迟测量工具（用于性能监控）
// ============================================================================
class LatencyTracker {
public:
    LatencyTracker(const char* operation)
        : operation_(operation), start_(Timestamp::now()) {}

    ~LatencyTracker() {
        auto end = Timestamp::now();
        int64_t latency_ns = end.nanos_since_epoch - start_.nanos_since_epoch;

        // 根据延迟选择日志级别
        if (latency_ns > 1'000'000) { // > 1ms
            LOG_WARNINGF(LogCategory::LATENCY, "%s took %ldus",
                operation_, latency_ns / 1000);
        } else {
            LOG_DEBUGF(LogCategory::LATENCY, "%s took %ldns",
                operation_, latency_ns);
        }
    }

private:
    const char* operation_;
    Timestamp start_;
};

#define TRACK_LATENCY(name) hft::LatencyTracker _latency_tracker_##__LINE__(name)

} // namespace hft

#endif // HFT_LOGGER_H
