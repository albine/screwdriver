#ifndef PRICE_LEVEL_H
#define PRICE_LEVEL_H

#include "Order.h"
#include <queue>
#include <memory>
#include <unordered_map>
#include <cstdint>

// 价格档位 - 使用 std::queue 实现 FIFO
struct PriceLevel {
    uint32_t price;                                      // 价格 (单位：分)
    uint64_t total_volume;                               // 总挂单量
    uint32_t order_count;                                // 订单数量
    std::queue<std::shared_ptr<Order>> orders;          // 订单队列 (FIFO)
    
    // 为了支持删除操作，我们仍需要一个辅助结构来快速定位
    // 但保持队列的 FIFO 特性用于撮合
    std::unordered_map<uint64_t, std::shared_ptr<Order>> order_map;  // 快速查找
    
    PriceLevel();
    
    explicit PriceLevel(uint32_t p);
    
    // 添加订单
    void add_order(std::shared_ptr<Order> order);
    
    // 移除订单（通过序号）
    bool remove_order(uint64_t order_seq);
    
    // 更新订单数量（成交或撤单后）
    void update_volume(uint64_t order_seq, int64_t volume_change);
    
    // 判断档位是否为空
    bool is_empty() const;
    
    // 获取队首订单（用于撮合）
    std::shared_ptr<Order> get_front_order();
};

#endif // PRICE_LEVEL_H
