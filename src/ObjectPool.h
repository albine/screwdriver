#pragma once

#include <vector>
#include <cstdint>
#include <stdexcept>

/**
 * @brief 通用对象池实现
 * 
 * 使用 std::vector 作为底层存储，结合空闲链表（Free List）实现对象的复用。
 * 对象 T 需要是可默认构造的。
 * 
 * @tparam T 对象类型
 */
template <typename T>
class ObjectPool {
public:
    /**
     * @brief 构造函数
     * @param initial_capacity 初始容量，避免频繁扩容
     */
    explicit ObjectPool(size_t initial_capacity = 100000) {
        pool_.reserve(initial_capacity);
        free_list_.reserve(initial_capacity);
    }

    /**
     * @brief 分配一个对象
     * @return 对象的索引 (idx)。如果分配失败返回 -1。
     */
    int32_t alloc() {
        // 1. 优先从空闲链表中获取
        if (!free_list_.empty()) {
            int32_t idx = free_list_.back();
            free_list_.pop_back();
            return idx;
        }
        
        // 2. 检查容量上限 (int32_t 限制)
        if (pool_.size() >= static_cast<size_t>(INT32_MAX)) {
            return -1; 
        }
        
        // 3. 新增对象
        // 注意：这里可能会触发 vector 扩容
        pool_.emplace_back();
        return static_cast<int32_t>(pool_.size() - 1);
    }

    /**
     * @brief 释放对象，归还到池中
     * @param idx 对象索引
     */
    void free(int32_t idx) {
        if (idx < 0 || static_cast<size_t>(idx) >= pool_.size()) {
            return;
        }
        free_list_.push_back(idx);
    }

    /**
     * @brief 获取对象引用
     * @param idx 对象索引
     * @return T& 
     */
    T& get(int32_t idx) {
        // 简单的边界检查
        if (idx < 0 || static_cast<size_t>(idx) >= pool_.size()) {
            throw std::out_of_range("ObjectPool index out of range");
        }
        return pool_[idx];
    }

    /**
     * @brief 获取对象常量引用
     * @param idx 对象索引
     * @return const T& 
     */
    const T& get(int32_t idx) const {
        if (idx < 0 || static_cast<size_t>(idx) >= pool_.size()) {
            throw std::out_of_range("ObjectPool index out of range");
        }
        return pool_[idx];
    }

    /**
     * @brief 获取底层 vector 的大小（包含已分配和未使用的空洞）
     */
    size_t size() const {
        return pool_.size();
    }
    
    /**
     * @brief 获取当前空闲对象的数量
     */
    size_t free_count() const {
        return free_list_.size();
    }
    
    /**
     * @brief 清空对象池
     */
    void clear() {
        pool_.clear();
        free_list_.clear();
    }

private:
    std::vector<T> pool_;
    std::vector<int32_t> free_list_;
};
