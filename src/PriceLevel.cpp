#include "PriceLevel.h"

PriceLevel::PriceLevel() : price(0), total_volume(0), order_count(0) {}

PriceLevel::PriceLevel(uint32_t p) : price(p), total_volume(0), order_count(0) {}

// 添加订单
void PriceLevel::add_order(std::shared_ptr<Order> order) {
    orders.push(order);
    order_map[order->order_seq] = order;
    total_volume += order->get_remaining_volume();
    order_count++;
}

// 移除订单（通过序号）
bool PriceLevel::remove_order(uint64_t order_seq) {
    auto it = order_map.find(order_seq);
    if (it == order_map.end()) {
        return false;
    }
    
    auto order = it->second;
    total_volume -= order->get_remaining_volume();
    order_map.erase(it);
    order_count--;
    
    // 注意：从队列中物理删除比较复杂，这里标记为已删除即可
    // 在出队时检查订单是否仍在 order_map 中
    
    return true;
}

// 更新订单数量（成交或撤单后）
void PriceLevel::update_volume(uint64_t order_seq, int64_t volume_change) {
    // volume_change 可以是负数（表示减少）
    total_volume = static_cast<uint64_t>(
        static_cast<int64_t>(total_volume) + volume_change
    );
}

// 判断档位是否为空
bool PriceLevel::is_empty() const {
    return order_map.empty();
}

// 获取队首订单（用于撮合）
std::shared_ptr<Order> PriceLevel::get_front_order() {
    // 清理已删除的订单
    while (!orders.empty()) {
        auto order = orders.front();
        if (order_map.find(order->order_seq) != order_map.end()) {
            return order;  // 找到有效订单
        }
        orders.pop();  // 移除无效订单
    }
    return nullptr;
}
