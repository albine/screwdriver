#ifndef PERSIST_LAYER_H
#define PERSIST_LAYER_H

#include "concurrentqueue.h"
#include "mmap_writer.h"
#include "market_data_structs_aligned.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <ctime>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

#ifdef __x86_64__
#include <emmintrin.h>  // _mm_pause
#endif

#define LOG_MODULE "PersistLayer"
#include "logger.h"

// ============================================================================
// PersistLayer - 高频市场数据持久化层
// ============================================================================
// 架构:
//   Gateway SDK (5 threads) --enqueue--> MPSC Queues --dequeue--> Writer Thread --mmap--> Files
//
// 特点:
//   - 无锁 MPSC 队列 (moodycamel::ConcurrentQueue)
//   - mmap 内存映射文件写入
//   - 单独 writer 线程，可 CPU 绑核
//   - 支持优雅关闭 (drain 所有队列)
//
class PersistLayer {
public:
    // Magic 定义 - V2 格式 (使用对齐结构体和 local_recv_timestamp)
    static constexpr uint32_t MAGIC_ORDER       = MAGIC_ORDER_V2;       // 0x4F524432 "ORD2"
    static constexpr uint32_t MAGIC_TRANSACTION = MAGIC_TRANSACTION_V2; // 0x54584E32 "TXN2"
    static constexpr uint32_t MAGIC_TICK        = MAGIC_TICK_V2;        // 0x54494B32 "TIK2"
    static constexpr uint32_t MAGIC_SNAPSHOT    = MAGIC_ORDERBOOK_V2;   // 0x4F424B32 "OBK2"

    // 预分配容量 (含 20% buffer)
    // 根据数据量估算:
    //   ORDER:       2.68 亿/天 -> 320M
    //   TRANSACTION: 2.21 亿/天 -> 270M
    //   TICK:        2050 万/天 -> 25M
    //   SNAPSHOT:    2400 万/天 -> 30M
    static constexpr size_t ORDER_CAPACITY    = 320000000;
    static constexpr size_t TXN_CAPACITY      = 270000000;
    static constexpr size_t TICK_CAPACITY     = 40000000;
    static constexpr size_t SNAPSHOT_CAPACITY = 400000000;

    // 队列初始容量 (用于预分配内部块)
    static constexpr size_t ORDER_QUEUE_SIZE    = 131072;  // 128K
    static constexpr size_t TXN_QUEUE_SIZE      = 131072;
    static constexpr size_t TICK_QUEUE_SIZE     = 16384;   // 16K
    static constexpr size_t SNAPSHOT_QUEUE_SIZE = 16384;

private:
    // MPSC 队列
    moodycamel::ConcurrentQueue<MDOrderStruct>       order_queue_;
    moodycamel::ConcurrentQueue<MDTransactionStruct> txn_queue_;
    moodycamel::ConcurrentQueue<MDStockStruct>       tick_queue_;
    moodycamel::ConcurrentQueue<MDOrderbookStruct>   snapshot_queue_;

    // mmap 写入器
    MmapWriter<MDOrderStruct>       order_writer_;
    MmapWriter<MDTransactionStruct> txn_writer_;
    MmapWriter<MDStockStruct>       tick_writer_;
    MmapWriter<MDOrderbookStruct>   snapshot_writer_;

    // Writer 线程
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    int writer_cpu_id_ = -1;  // CPU 绑核 (-1 表示不绑核)

    // 统计
    std::atomic<uint64_t> total_orders_{0};
    std::atomic<uint64_t> total_txns_{0};
    std::atomic<uint64_t> total_ticks_{0};
    std::atomic<uint64_t> total_snapshots_{0};

public:
    PersistLayer()
        : order_queue_(ORDER_QUEUE_SIZE)
        , txn_queue_(TXN_QUEUE_SIZE)
        , tick_queue_(TICK_QUEUE_SIZE)
        , snapshot_queue_(SNAPSHOT_QUEUE_SIZE)
    {}

    ~PersistLayer() {
        stop();
    }

    // 禁用拷贝和移动
    PersistLayer(const PersistLayer&) = delete;
    PersistLayer& operator=(const PersistLayer&) = delete;
    PersistLayer(PersistLayer&&) = delete;
    PersistLayer& operator=(PersistLayer&&) = delete;

    // 初始化持久化层
    // @param date       日期字符串 (YYYYMMDD)
    // @param data_dir   数据根目录 (如 /data/raw)
    // @param writer_cpu CPU 绑核 ID (-1 表示不绑核)
    // @return 成功返回 true
    bool init(const std::string& date, const std::string& data_dir, int writer_cpu = -1) {
        writer_cpu_id_ = writer_cpu;

        // 构建目录路径: /data/raw/YYYY/MM/DD/
        std::string prefix = data_dir + "/" + date.substr(0, 4) + "/"
                           + date.substr(4, 2) + "/" + date.substr(6, 2) + "/";

        LOG_M_INFO("Initializing PersistLayer: date={} dir={}", date, prefix);

        // 创建目录 (递归)
        if (!create_directories(prefix)) {
            LOG_M_ERROR("Failed to create directory: {}", prefix);
            return false;
        }

        // 打开 mmap 文件
        std::string order_path = prefix + "orders.bin";
        std::string txn_path   = prefix + "transactions.bin";
        std::string tick_path  = prefix + "ticks.bin";
        std::string snap_path  = prefix + "snapshots.bin";

        if (!order_writer_.open(order_path.c_str(), ORDER_CAPACITY, MAGIC_ORDER)) {
            LOG_M_ERROR("Failed to open order file: {}", order_path);
            return false;
        }

        if (!txn_writer_.open(txn_path.c_str(), TXN_CAPACITY, MAGIC_TRANSACTION)) {
            LOG_M_ERROR("Failed to open transaction file: {}", txn_path);
            return false;
        }

        if (!tick_writer_.open(tick_path.c_str(), TICK_CAPACITY, MAGIC_TICK)) {
            LOG_M_ERROR("Failed to open tick file: {}", tick_path);
            return false;
        }

        if (!snapshot_writer_.open(snap_path.c_str(), SNAPSHOT_CAPACITY, MAGIC_SNAPSHOT)) {
            LOG_M_ERROR("Failed to open snapshot file: {}", snap_path);
            return false;
        }

        // 启动 writer 线程
        running_ = true;
        writer_thread_ = std::thread(&PersistLayer::writer_loop, this);

        LOG_M_INFO("PersistLayer initialized successfully, writer_cpu={}", writer_cpu);
        return true;
    }

    // 停止持久化层
    void stop() {
        if (!running_.exchange(false)) {
            return;  // 已经停止
        }

        LOG_M_INFO("Stopping PersistLayer...");

        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }

        // 输出统计
        LOG_M_INFO("PersistLayer stopped. Stats: orders={} txns={} ticks={} snapshots={}",
                   total_orders_.load(), total_txns_.load(),
                   total_ticks_.load(), total_snapshots_.load());
    }

    // ========================================
    // 热路径入口 - Gateway 线程调用
    // ========================================

    void log_order(const MDOrderStruct& order) {
        order_queue_.enqueue(order);
        total_orders_.fetch_add(1, std::memory_order_relaxed);
    }

    void log_transaction(const MDTransactionStruct& txn) {
        txn_queue_.enqueue(txn);
        total_txns_.fetch_add(1, std::memory_order_relaxed);
    }

    void log_tick(const MDStockStruct& tick) {
        tick_queue_.enqueue(tick);
        total_ticks_.fetch_add(1, std::memory_order_relaxed);
    }

    void log_snapshot(const MDOrderbookStruct& snapshot) {
        snapshot_queue_.enqueue(snapshot);
        total_snapshots_.fetch_add(1, std::memory_order_relaxed);
    }

    // ========================================
    // 统计查询
    // ========================================

    uint64_t get_total_orders() const { return total_orders_.load(); }
    uint64_t get_total_transactions() const { return total_txns_.load(); }
    uint64_t get_total_ticks() const { return total_ticks_.load(); }
    uint64_t get_total_snapshots() const { return total_snapshots_.load(); }

    size_t get_written_orders() const { return order_writer_.record_count(); }
    size_t get_written_transactions() const { return txn_writer_.record_count(); }
    size_t get_written_ticks() const { return tick_writer_.record_count(); }
    size_t get_written_snapshots() const { return snapshot_writer_.record_count(); }

private:
    // 创建目录 (递归)
    bool create_directories(const std::string& path) {
        size_t pos = 0;
        while ((pos = path.find('/', pos + 1)) != std::string::npos) {
            std::string sub = path.substr(0, pos);
            if (::mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
        return true;
    }

    // Writer 线程主循环
    void writer_loop() {
        // CPU 绑核
#ifdef __linux__
        if (writer_cpu_id_ >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(writer_cpu_id_, &cpuset);
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
                LOG_M_INFO("Writer thread pinned to CPU {}", writer_cpu_id_);
            } else {
                LOG_M_WARNING("Failed to pin writer thread to CPU {}", writer_cpu_id_);
            }
        }
#endif

        // 批量缓冲区
        MDOrderStruct       order_batch[512];
        MDTransactionStruct txn_batch[512];
        MDStockStruct       tick_batch[64];
        MDOrderbookStruct   snapshot_batch[64];

        auto last_sync = std::chrono::steady_clock::now();
        auto last_stats = last_sync;

        while (running_) {
            bool did_work = false;

            // 批量出队并写入
            size_t n = order_queue_.try_dequeue_bulk(order_batch, 512);
            if (n > 0) {
                order_writer_.write_batch(order_batch, n);
                did_work = true;
            }

            n = txn_queue_.try_dequeue_bulk(txn_batch, 512);
            if (n > 0) {
                txn_writer_.write_batch(txn_batch, n);
                did_work = true;
            }

            n = tick_queue_.try_dequeue_bulk(tick_batch, 64);
            if (n > 0) {
                tick_writer_.write_batch(tick_batch, n);
                did_work = true;
            }

            n = snapshot_queue_.try_dequeue_bulk(snapshot_batch, 64);
            if (n > 0) {
                snapshot_writer_.write_batch(snapshot_batch, n);
                did_work = true;
            }

            auto now = std::chrono::steady_clock::now();

            // 每秒 msync (异步刷盘)
            if (now - last_sync > std::chrono::seconds(1)) {
                order_writer_.sync();
                txn_writer_.sync();
                tick_writer_.sync();
                snapshot_writer_.sync();
                last_sync = now;
            }

            // 每分钟打印统计
            if (now - last_stats > std::chrono::minutes(1)) {
                LOG_M_INFO("PersistLayer stats: written orders={} txns={} ticks={} snaps={}",
                           order_writer_.record_count(),
                           txn_writer_.record_count(),
                           tick_writer_.record_count(),
                           snapshot_writer_.record_count());
                last_stats = now;
            }

            // 没有工作时短暂休眠/pause
            if (!did_work) {
#ifdef __x86_64__
                _mm_pause();  // CPU hint，避免 busy spin 功耗
#else
                std::this_thread::yield();
#endif
            }
        }

        // 退出前 drain 所有队列
        drain_all(order_batch, txn_batch, tick_batch, snapshot_batch);

        // 关闭文件 (会同步刷盘)
        order_writer_.close();
        txn_writer_.close();
        tick_writer_.close();
        snapshot_writer_.close();

        LOG_M_INFO("Writer thread exited");
    }

    // 排空所有队列
    void drain_all(MDOrderStruct* order_batch,
                   MDTransactionStruct* txn_batch,
                   MDStockStruct* tick_batch,
                   MDOrderbookStruct* snapshot_batch) {
        LOG_M_INFO("Draining queues...");

        size_t n;
        size_t drained_orders = 0, drained_txns = 0, drained_ticks = 0, drained_snaps = 0;

        while ((n = order_queue_.try_dequeue_bulk(order_batch, 512)) > 0) {
            order_writer_.write_batch(order_batch, n);
            drained_orders += n;
        }
        while ((n = txn_queue_.try_dequeue_bulk(txn_batch, 512)) > 0) {
            txn_writer_.write_batch(txn_batch, n);
            drained_txns += n;
        }
        while ((n = tick_queue_.try_dequeue_bulk(tick_batch, 64)) > 0) {
            tick_writer_.write_batch(tick_batch, n);
            drained_ticks += n;
        }
        while ((n = snapshot_queue_.try_dequeue_bulk(snapshot_batch, 64)) > 0) {
            snapshot_writer_.write_batch(snapshot_batch, n);
            drained_snaps += n;
        }

        LOG_M_INFO("Drained: orders={} txns={} ticks={} snaps={}",
                   drained_orders, drained_txns, drained_ticks, drained_snaps);
    }
};

#undef LOG_MODULE

// ============================================================================
// 辅助函数: 获取当前日期字符串 (YYYYMMDD)
// ============================================================================
inline std::string get_current_date() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y%m%d", t);
    return buf;
}

#endif // PERSIST_LAYER_H
