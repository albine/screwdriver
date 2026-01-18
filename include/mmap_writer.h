#ifndef MMAP_WRITER_H
#define MMAP_WRITER_H

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <string>

#define LOG_MODULE "MmapWriter"
#include "logger.h"

// ============================================================================
// MmapWriter - 高性能 mmap 文件写入器
// ============================================================================
// 用于高频市场数据持久化，支持单消费者写入
// 文件格式:
//   [64 字节 Header] + [N 条 Record]
//
// Header 结构 (64 字节，cache line 对齐):
//   magic:        4 字节 - 文件类型标识
//   version:      2 字节 - 格式版本
//   struct_size:  2 字节 - 单条记录大小
//   record_count: 8 字节 - 已写入记录数
//   write_offset: 8 字节 - 下一写入位置
//   reserved:    40 字节 - 保留
//
template<typename T>
class MmapWriter {
public:
    static constexpr size_t HEADER_SIZE = 64;

    // 文件头结构 (64 字节，cache line 对齐)
    struct alignas(64) Header {
        uint32_t magic;                        // 文件类型标识
        uint16_t version;                      // 格式版本
        uint16_t struct_size;                  // sizeof(T)
        std::atomic<uint64_t> record_count;    // 已写入记录数
        std::atomic<uint64_t> write_offset;    // 下一写入位置 (记录索引)
        char reserved[40];                     // 保留字节
    };

    static_assert(sizeof(Header) == 64, "Header must be 64 bytes");

private:
    int fd_ = -1;
    void* base_ = nullptr;
    size_t file_size_ = 0;
    size_t capacity_ = 0;        // 最大记录数
    Header* header_ = nullptr;
    T* data_ = nullptr;
    std::string path_;

public:
    MmapWriter() = default;

    ~MmapWriter() {
        close();
    }

    // 禁用拷贝
    MmapWriter(const MmapWriter&) = delete;
    MmapWriter& operator=(const MmapWriter&) = delete;

    // 允许移动
    MmapWriter(MmapWriter&& other) noexcept {
        *this = std::move(other);
    }

    MmapWriter& operator=(MmapWriter&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            base_ = other.base_;
            file_size_ = other.file_size_;
            capacity_ = other.capacity_;
            header_ = other.header_;
            data_ = other.data_;
            path_ = std::move(other.path_);

            other.fd_ = -1;
            other.base_ = nullptr;
            other.header_ = nullptr;
            other.data_ = nullptr;
        }
        return *this;
    }

    // 打开或创建 mmap 文件
    // @param path      文件路径
    // @param capacity  预分配容量 (记录数)
    // @param magic     文件类型标识 (用于校验)
    // @return 成功返回 true
    bool open(const char* path, size_t capacity, uint32_t magic) {
        path_ = path;
        capacity_ = capacity;
        file_size_ = HEADER_SIZE + capacity * sizeof(T);

        LOG_M_INFO("Opening mmap file: {} capacity={} struct_size={} total_size={}",
                   path, capacity, sizeof(T), file_size_);

        // 打开文件 (创建如果不存在)
        fd_ = ::open(path, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            LOG_M_ERROR("Failed to open file: {} errno={}", path, errno);
            return false;
        }

        // 预分配文件空间
        // 优先使用 fallocate (避免写时分配延迟)
        // fallback 到 ftruncate
#ifdef __linux__
        if (::fallocate(fd_, 0, 0, static_cast<off_t>(file_size_)) != 0) {
            LOG_M_WARNING("fallocate failed, falling back to ftruncate");
            if (::ftruncate(fd_, static_cast<off_t>(file_size_)) != 0) {
                LOG_M_ERROR("ftruncate failed: errno={}", errno);
                ::close(fd_);
                fd_ = -1;
                return false;
            }
        }
#else
        if (::ftruncate(fd_, static_cast<off_t>(file_size_)) != 0) {
            LOG_M_ERROR("ftruncate failed: errno={}", errno);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
#endif

        // 建立内存映射
        base_ = ::mmap(nullptr, file_size_,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (base_ == MAP_FAILED) {
            LOG_M_ERROR("mmap failed: errno={}", errno);
            ::close(fd_);
            fd_ = -1;
            base_ = nullptr;
            return false;
        }

        header_ = reinterpret_cast<Header*>(base_);
        data_ = reinterpret_cast<T*>(static_cast<char*>(base_) + HEADER_SIZE);

        // 初始化或恢复 header
        if (header_->magic != magic) {
            // 新文件，初始化 header
            LOG_M_INFO("Initializing new file header, magic=0x{:08X}", magic);
            header_->magic = magic;
            header_->version = 1;
            header_->struct_size = static_cast<uint16_t>(sizeof(T));
            header_->record_count.store(0, std::memory_order_relaxed);
            header_->write_offset.store(0, std::memory_order_relaxed);
            std::memset(header_->reserved, 0, sizeof(header_->reserved));
        } else {
            // 已有文件，恢复写入位置
            uint64_t existing_count = header_->record_count.load(std::memory_order_acquire);
            LOG_M_INFO("Resuming from existing file, records={}", existing_count);

            // 校验 struct_size
            if (header_->struct_size != sizeof(T)) {
                LOG_M_ERROR("Struct size mismatch: file={} expected={}",
                           header_->struct_size, sizeof(T));
                close();
                return false;
            }
        }

        return true;
    }

    // 批量写入记录 (单消费者调用)
    // @param records  记录数组
    // @param count    记录数量
    // @return 实际写入的记录数
    size_t write_batch(const T* records, size_t count) {
        if (!header_ || count == 0) return 0;

        uint64_t offset = header_->write_offset.load(std::memory_order_relaxed);

        // 检查容量
        if (offset >= capacity_) {
            LOG_M_ERROR("File capacity exhausted: offset={} capacity={}", offset, capacity_);
            return 0;
        }

        size_t to_write = std::min(count, capacity_ - static_cast<size_t>(offset));

        // 批量内存拷贝
        std::memcpy(&data_[offset], records, to_write * sizeof(T));

        // 更新写入位置 (使用 release 语义保证数据可见性)
        header_->write_offset.store(offset + to_write, std::memory_order_release);
        header_->record_count.store(offset + to_write, std::memory_order_release);

        return to_write;
    }

    // 异步刷新到磁盘
    void sync() {
        if (base_ && base_ != MAP_FAILED) {
            ::msync(base_, file_size_, MS_ASYNC);
        }
    }

    // 同步刷新到磁盘
    void sync_blocking() {
        if (base_ && base_ != MAP_FAILED) {
            ::msync(base_, file_size_, MS_SYNC);
        }
    }

    // 关闭文件
    void close() {
        if (base_ && base_ != MAP_FAILED) {
            LOG_M_INFO("Closing mmap file: {} records={}",
                       path_, header_ ? header_->record_count.load() : 0);
            ::msync(base_, file_size_, MS_SYNC);
            ::munmap(base_, file_size_);
            base_ = nullptr;
            header_ = nullptr;
            data_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // 获取已写入记录数
    size_t record_count() const {
        return header_ ? header_->record_count.load(std::memory_order_acquire) : 0;
    }

    // 获取容量
    size_t capacity() const { return capacity_; }

    // 是否已打开
    bool is_open() const { return fd_ >= 0 && base_ != nullptr; }

    // 读取记录 (用于调试)
    const T& operator[](size_t i) const { return data_[i]; }
};

#undef LOG_MODULE

#endif // MMAP_WRITER_H
