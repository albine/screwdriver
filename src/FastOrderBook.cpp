#include "FastOrderBook.h"
#include <cstring>         // for memset if needed
#include <algorithm>       // for std::find

// 日志模块
#define LOG_MODULE MOD_ORDERBOOK

// 价格档位间隔：0.01元 * 10000 = 100
// 例如：9.99元 = 99900，10.00元 = 100000，间隔100
constexpr uint32_t TICK_SIZE = 100;

// 确保价格在合法范围内
#define CHECK_PRICE_RANGE(p) \
    if ((p) < min_price_ || (p) > (min_price_ + (levels_.size() - 1) * TICK_SIZE)) return false;

FastOrderBook::FastOrderBook(uint32_t code, ObjectPool<OrderNode>& pool, uint32_t min_price, uint32_t max_price)
    : stock_code_(code), pool_(pool), min_price_(min_price) {

    // 1. 计算价格覆盖范围 (例如 跌停价~涨停价)
    // 每个档位间隔TICK_SIZE (100)，多加1是为了包含 max_price 本身
    size_t capacity = (max_price - min_price) / TICK_SIZE + 1;
    
    // 2. 预分配 Level 数组 (Direct Array Mapping)
    // 关键：这里只分配内存，不构造内部对象，因为 Level 是 POD
    levels_.resize(capacity);
    
    // 初始化 Level 状态 (全部置空)
    for (auto& lvl : levels_) {
        lvl.bid_volume = 0;
        lvl.bid_head_idx = -1;
        lvl.bid_tail_idx = -1;
        lvl.ask_volume = 0;
        lvl.ask_head_idx = -1;
        lvl.ask_tail_idx = -1;
        // lvl.price 可以在这里初始化，也可以在用的时候算，这里为了严谨设为对应价格
        // 注意：实际访问是靠下标，这个 price 字段主要用于调试或校验
        lvl.price = 0;
    }
    
    // 初始化游标，-1 表示当前无挂单
    best_bid_idx_ = -1;
    best_ask_idx_ = -1;

    // 预留空间
    market_orders_.reserve(1000);
    order_index_.reserve(100000); // 建议使用 flat_hash_map
}

bool FastOrderBook::on_order(const MDOrderStruct& order) {
    // 映射 MDOrderStruct.ordertype 到内部 OrderType
    // 约定 (基于 OrderBook.cpp): 1=Market, 2=Limit, 3=Best, 4=Cancel
    // 注意：深圳使用 orderindex 作为订单唯一标识（与成交回报中的 TradeBuyNo/TradeSellNo 匹配）
    //      上海使用 orderno
    uint64_t order_id = (order.orderno != 0) ? order.orderno : order.orderindex;

    switch (order.ordertype) {
        case 1: // Market Order
            return add_order(order_id, OrderType::Market,
                             (order.orderbsflag == 1 ? Side::Buy : Side::Sell),
                             (uint32_t)order.orderprice, (uint32_t)order.orderqty);
        case 2: // Limit Order
            return add_order(order_id, OrderType::Limit,
                             (order.orderbsflag == 1 ? Side::Buy : Side::Sell),
                             (uint32_t)order.orderprice, (uint32_t)order.orderqty);
        case 3: // Best Order
            return add_order(order_id, OrderType::Best,
                             (order.orderbsflag == 1 ? Side::Buy : Side::Sell),
                             (uint32_t)order.orderprice, (uint32_t)order.orderqty);
        case 4:  // Cancel (Standard)
        case 10: // ShanghaiCancel
            return cancel_order(order_id, (uint32_t)order.orderqty);
        default:
            return false;
    }
}

bool FastOrderBook::add_order(uint64_t seq, OrderType type, Side side, uint32_t price, uint32_t volume) {
    // 1. 从内存池申请节点 (Zero Allocation)
    int32_t node_idx = pool_.alloc();
    if (node_idx < 0) {
        LOG_M_ERROR("Memory pool exhausted!");
        return false;
    }

    OrderNode& node = pool_.get(node_idx);

    // 2. 填充数据 (POD赋值，极快)
    node.seq = seq;
    node.volume = volume;
    node.type = type;
    node.side = side;
    node.original_price = price; // 保存原始语义
    node.prev_idx = -1;
    node.next_idx = -1;

    // 3. 建立索引 (Seq -> Index)
    order_index_[seq] = node_idx;

    // 4. 分发逻辑
    if (type == OrderType::Market) {
        // --- 市价单 ---
        node.sort_price = 0; // 无意义
        market_orders_.push_back(node_idx);
        return true;
    }
    
    // --- 确定物理挂单价格 (sort_price) ---
    uint32_t target_price = price;

    if (type == OrderType::Best) {
        // 本方最优：获取当前最优价
        // 如果买单，找 best_bid；如果卖单，找 best_ask
        // 注意：get_best_xxx 返回的是 optional
        auto best_opt = (side == Side::Buy) ? get_best_bid() : get_best_ask();
        
        if (!best_opt.has_value()) {
            // 对手盘空或者本方空？通常Best指本方最优。
            // 如果本方没有挂单，Best单通常转为市价单或撤销，这里按转市价处理
            market_orders_.push_back(node_idx);
            return true;
        }
        target_price = best_opt.value();
    }
    
    // 记录物理价格
    node.sort_price = target_price;
    CHECK_PRICE_RANGE(target_price); // 边界检查

    // 5. 挂入 Level 链表
    // 计算数组下标：Offset Mapping，每TICK_SIZE为一档
    uint32_t lvl_idx = (target_price - min_price_) / TICK_SIZE;
    Level& lvl = levels_[lvl_idx];
    
    // 设置 Level 的价格 (如果是第一次用到)
    lvl.price = target_price; 
    
    add_node_to_level(lvl, node_idx, node);

    // 6. 更新最优价游标 (Cursor Update)
    // 这是一个 O(1) 的检查
    if (side == Side::Buy) {
        // 买单：价格越高越好。如果新单价格 > 当前最优，或者当前没最优，更新指针
        if (best_bid_idx_ == -1 || (int32_t)lvl_idx > best_bid_idx_) {
            best_bid_idx_ = (int32_t)lvl_idx;
        }
    } else {
        // 卖单：价格越低越好。如果新单价格 < 当前最优，或者当前没最优，更新指针
        if (best_ask_idx_ == -1 || (int32_t)lvl_idx < best_ask_idx_) {
            best_ask_idx_ = (int32_t)lvl_idx;
        }
    }

    return true;
}

bool FastOrderBook::on_trade(uint64_t bid_seq, uint64_t ask_seq, uint32_t volume) {
    bool b = update_volume_internal(bid_seq, volume);
    bool a = update_volume_internal(ask_seq, volume);
    return b && a;
}

bool FastOrderBook::cancel_order(uint64_t seq, uint32_t cancel_volume) {
    return update_volume_internal(seq, cancel_volume);
}

// 内部核心逻辑
bool FastOrderBook::update_volume_internal(uint64_t seq, uint32_t delta_vol) {
    // 1. 查找订单 (O(1) Hash Map)
    auto it = order_index_.find(seq);
    if (it == order_index_.end()) return false;

    int32_t node_idx = it->second;
    OrderNode& node = pool_.get(node_idx);

    // 2. 扣减量
    if (node.volume < delta_vol) {
         // 异常情况：成交量大于剩余量
         LOG_M_ERROR("Volume underflow! seq={}, node.volume={}, delta_vol={}, price={}, side={}",
             seq, node.volume, delta_vol, node.sort_price, static_cast<int>(node.side));
         node.volume = 0; 
    } else {
        node.volume -= delta_vol;
    }

    // 3. 更新 Level 总量 (仅限限价类订单)
    bool is_limit_type = (node.type != OrderType::Market);
    Level* lvl_ptr = nullptr;

    if (is_limit_type) {
        // 直接通过 sort_price 定位 Level，无需搜索
        lvl_ptr = get_level_ptr(node.sort_price);
        if (lvl_ptr) {
            // 根据买卖方向更新对应的 volume
            if (node.side == Side::Buy) {
                lvl_ptr->bid_volume -= delta_vol;
            } else {
                lvl_ptr->ask_volume -= delta_vol;
            }
        }
    }

    // 4. 如果仍有剩余，处理结束
    if (node.volume > 0) return true;

    // --- 订单完结 (Volume归零) ---

    if (is_limit_type && lvl_ptr) {
        // 从 Level 链表摘除
        remove_node_from_level(*lvl_ptr, node_idx, node);

        // 5. 关键：检查是否需要移动最优价游标
        // 只有当删除的单子属于最优价档位，且该档位变空时才需要扫描
        uint32_t lvl_idx = (node.sort_price - min_price_) / TICK_SIZE;

        if (node.side == Side::Buy) {
            if ((int32_t)lvl_idx == best_bid_idx_) {
                // 检查该档位买单是否已空
                if (lvl_ptr->bid_volume == 0) {
                    update_best_bid_cursor(); // 触发线性扫描
                }
            }
        } else {
            if ((int32_t)lvl_idx == best_ask_idx_) {
                // 检查该档位卖单是否已空
                if (lvl_ptr->ask_volume == 0) {
                    update_best_ask_cursor(); // 触发线性扫描
                }
            }
        }
    } 
    else if (node.type == OrderType::Market) {
        // 市价单移除逻辑：从 market_orders_ vector 中移除
        auto it = std::find(market_orders_.begin(), market_orders_.end(), node_idx);
        if (it != market_orders_.end()) {
            // swap-and-pop 策略：O(1) 删除，不保持顺序
            *it = market_orders_.back();
            market_orders_.pop_back();
        }
    }

    // 6. 回收资源
    order_index_.erase(it);
    pool_.free(node_idx);

    return true;
}

// 链表挂载 (O(1))
void FastOrderBook::add_node_to_level(Level& lvl, int32_t node_idx, OrderNode& node) {
    // 根据买卖方向选择不同链表
    if (node.side == Side::Buy) {
        // 更新买方统计
        lvl.bid_volume += node.volume;

        if (lvl.bid_head_idx == -1) {
            // 链表为空，作为头节点
            lvl.bid_head_idx = node_idx;
            lvl.bid_tail_idx = node_idx;
            node.prev_idx = -1;
            node.next_idx = -1;
        } else {
            // 挂到尾部 (Tail)
            int32_t old_tail_idx = lvl.bid_tail_idx;
            OrderNode& old_tail = pool_.get(old_tail_idx);

            old_tail.next_idx = node_idx;
            node.prev_idx = old_tail_idx;
            node.next_idx = -1;
            lvl.bid_tail_idx = node_idx;
        }
    } else {
        // 更新卖方统计
        lvl.ask_volume += node.volume;

        if (lvl.ask_head_idx == -1) {
            // 链表为空，作为头节点
            lvl.ask_head_idx = node_idx;
            lvl.ask_tail_idx = node_idx;
            node.prev_idx = -1;
            node.next_idx = -1;
        } else {
            // 挂到尾部 (Tail)
            int32_t old_tail_idx = lvl.ask_tail_idx;
            OrderNode& old_tail = pool_.get(old_tail_idx);

            old_tail.next_idx = node_idx;
            node.prev_idx = old_tail_idx;
            node.next_idx = -1;
            lvl.ask_tail_idx = node_idx;
        }
    }
}

// 链表摘除 (O(1))
void FastOrderBook::remove_node_from_level(Level& lvl, int32_t node_idx, const OrderNode& node) {
    // 根据买卖方向选择不同链表
    if (node.side == Side::Buy) {
        // 1. 处理前驱
        if (node.prev_idx != -1) {
            pool_.get(node.prev_idx).next_idx = node.next_idx;
        } else {
            // 是头节点
            lvl.bid_head_idx = node.next_idx;
        }

        // 2. 处理后继
        if (node.next_idx != -1) {
            pool_.get(node.next_idx).prev_idx = node.prev_idx;
        } else {
            // 是尾节点
            lvl.bid_tail_idx = node.prev_idx;
        }
    } else {
        // 1. 处理前驱
        if (node.prev_idx != -1) {
            pool_.get(node.prev_idx).next_idx = node.next_idx;
        } else {
            // 是头节点
            lvl.ask_head_idx = node.next_idx;
        }

        // 2. 处理后继
        if (node.next_idx != -1) {
            pool_.get(node.next_idx).prev_idx = node.prev_idx;
        } else {
            // 是尾节点
            lvl.ask_tail_idx = node.prev_idx;
        }
    }
    // volume 已经在外面减过了，这里只负责链表结构
}

// 游标更新 (线性扫描 Linear Scan)
void FastOrderBook::update_best_bid_cursor() {
    // 只有当 best_bid_idx 指向的 Level 的买单空了才调用这里
    // 买盘：价格从高向低扫
    while (best_bid_idx_ >= 0) {
        const Level& lvl = levels_[best_bid_idx_];
        // 检查该档位是否有买单
        if (lvl.bid_volume > 0) {
            return; // 找到了新的支撑位
        }
        best_bid_idx_--;
    }
    // 如果循环结束，best_bid_idx_ 为 -1，表示买盘空了
}

void FastOrderBook::update_best_ask_cursor() {
    // 卖盘：价格从低向高扫
    // 注意边界检查
    int32_t max_idx = (int32_t)levels_.size() - 1;
    while (best_ask_idx_ <= max_idx && best_ask_idx_ != -1) {
        const Level& lvl = levels_[best_ask_idx_];
        // 检查该档位是否有卖单
        if (lvl.ask_volume > 0) {
            return; // 找到了新的压力位
        }
        best_ask_idx_++;
    }
    // 如果超界，表示卖盘空了
    if (best_ask_idx_ > max_idx) best_ask_idx_ = -1;
}

// 获取最优买价
std::optional<uint32_t> FastOrderBook::get_best_bid() const {
    if (best_bid_idx_ == -1) return std::nullopt;
    return min_price_ + best_bid_idx_ * TICK_SIZE;
}

// 获取最优卖价
std::optional<uint32_t> FastOrderBook::get_best_ask() const {
    if (best_ask_idx_ == -1) return std::nullopt;
    return min_price_ + best_ask_idx_ * TICK_SIZE;
}

// 获取某价格档位挂单量 (O(1) Array Access)
// 返回买卖总量
uint64_t FastOrderBook::get_volume_at_price(uint32_t price) const {
    if (price < min_price_ || price > min_price_ + (levels_.size() - 1) * TICK_SIZE) return 0;
    const Level& lvl = levels_[(price - min_price_) / TICK_SIZE];
    return lvl.bid_volume + lvl.ask_volume;
}

// 获取某价格档位的买方挂单量
uint64_t FastOrderBook::get_bid_volume_at_price(uint32_t price) const {
    if (price < min_price_ || price > min_price_ + (levels_.size() - 1) * TICK_SIZE) return 0;
    return levels_[(price - min_price_) / TICK_SIZE].bid_volume;
}

// 获取某价格档位的卖方挂单量
uint64_t FastOrderBook::get_ask_volume_at_price(uint32_t price) const {
    if (price < min_price_ || price > min_price_ + (levels_.size() - 1) * TICK_SIZE) return 0;
    return levels_[(price - min_price_) / TICK_SIZE].ask_volume;
}

// 区间总量查询 (SIMD 友好的内存连续扫描)
// 只统计卖方挂单量
uint64_t FastOrderBook::get_ask_volume_in_range(uint32_t start_price, uint32_t end_price) const {
    // 简单的边界裁剪
    uint32_t max_price = min_price_ + (levels_.size() - 1) * TICK_SIZE;
    if (start_price < min_price_) start_price = min_price_;
    if (end_price > max_price) end_price = max_price;

    if (start_price > end_price) return 0;

    uint32_t start_idx = (start_price - min_price_) / TICK_SIZE;
    uint32_t end_idx = (end_price - min_price_) / TICK_SIZE;

    uint64_t total = 0;

    // 这是一个极度紧凑的循环
    // 编译器会自动进行循环展开 (Loop Unrolling) 和 SIMD 优化
    for (uint32_t i = start_idx; i <= end_idx; ++i) {
        total += levels_[i].ask_volume;
    }

    return total;
}

// 获取买盘前N档 (价格从高到低)
std::vector<std::pair<uint32_t, uint64_t>> FastOrderBook::get_bid_levels(int n) const {
    std::vector<std::pair<uint32_t, uint64_t>> result;
    result.reserve(n);

    // 从 best_bid_idx_ 开始向下扫描
    int32_t idx = best_bid_idx_;
    while (idx >= 0 && (int)result.size() < n) {
        const Level& lvl = levels_[idx];
        // 检查该档位是否有买单
        if (lvl.bid_volume > 0) {
            result.emplace_back(min_price_ + idx * TICK_SIZE, lvl.bid_volume);
        }
        idx--;
    }
    return result;
}

// 获取卖盘前N档 (价格从低到高)
std::vector<std::pair<uint32_t, uint64_t>> FastOrderBook::get_ask_levels(int n) const {
    std::vector<std::pair<uint32_t, uint64_t>> result;
    result.reserve(n);

    int32_t max_idx = (int32_t)levels_.size() - 1;
    // 从 best_ask_idx_ 开始向上扫描
    int32_t idx = best_ask_idx_;
    while (idx >= 0 && idx <= max_idx && (int)result.size() < n) {
        const Level& lvl = levels_[idx];
        // 检查该档位是否有卖单
        if (lvl.ask_volume > 0) {
            result.emplace_back(min_price_ + idx * TICK_SIZE, lvl.ask_volume);
        }
        idx++;
    }
    return result;
}

Level* FastOrderBook::get_level_ptr(uint32_t price) {
    if (price < min_price_ || price > min_price_ + (levels_.size() - 1) * TICK_SIZE) return nullptr;
    return &levels_[(price - min_price_) / TICK_SIZE];
}

// 处理逐笔成交消息
bool FastOrderBook::on_transaction(const MDTransactionStruct& txn) {
    TradeType type = static_cast<TradeType>(txn.tradetype);
    TradeBSFlag bsflag = static_cast<TradeBSFlag>(txn.tradebsflag);

    // 处理撤单类型 (1, 2, 5, 6, 7, 8)
    if (type == TradeType::Cancel ||
        type == TradeType::OtherCancel2 ||
        type == TradeType::OtherCancel5 ||
        type == TradeType::OtherCancel6 ||
        type == TradeType::OtherCancel7 ||
        type == TradeType::OtherCancel8) {
        // 撤单逻辑：根据 bsflag 决定撤哪一方
        if (bsflag == TradeBSFlag::Buy) {
            return cancel_order(txn.tradebuyno, (uint32_t)txn.tradeqty);
        } else {
            return cancel_order(txn.tradesellno, (uint32_t)txn.tradeqty);
        }
    }

    // 成交逻辑 (tradetype == 0)
    bool is_shenzhen = (txn.securityidsource == 102);

    if (is_shenzhen) {
        // 深圳：更新双方订单
        return on_trade(txn.tradebuyno, txn.tradesellno, (uint32_t)txn.tradeqty);
    }

    // 上海
    bool result = true;

    if (bsflag == TradeBSFlag::Buy) {
        // 买方主动成交：只更新卖方订单
        // 如果 tradebuyno 对应委托存在，说明买方委托先于成交到达（乱序）
        auto it = order_index_.find(txn.tradebuyno);
        if (it != order_index_.end()) {
            const OrderNode& order = pool_.get(it->second);
            LOG_M_ERROR("Shanghai out-of-order: tradebuyno exists when bsflag=Buy | "
                "txn: seq={}, security={}, buyno={}, sellno={}, price={}, qty={}, type={} | "
                "order: seq={}, price={}, vol={}, side={}, type={}",
                txn.applseqnum, txn.htscsecurityid, txn.tradebuyno, txn.tradesellno,
                txn.tradeprice, txn.tradeqty, txn.tradetype,
                order.seq, order.sort_price, order.volume, static_cast<int>(order.side), static_cast<int>(order.type));
        }
        result = update_volume_internal(txn.tradesellno, (uint32_t)txn.tradeqty);
    }
    else if (bsflag == TradeBSFlag::Sell) {
        // 卖方主动成交：只更新买方订单
        // 如果 tradesellno 对应委托存在，说明卖方委托先于成交到达（乱序）
        auto it = order_index_.find(txn.tradesellno);
        if (it != order_index_.end()) {
            const OrderNode& order = pool_.get(it->second);
            LOG_M_ERROR("Shanghai out-of-order: tradesellno exists when bsflag=Sell | "
                "txn: seq={}, security={}, buyno={}, sellno={}, price={}, qty={}, type={} | "
                "order: seq={}, price={}, vol={}, side={}, type={}",
                txn.applseqnum, txn.htscsecurityid, txn.tradebuyno, txn.tradesellno,
                txn.tradeprice, txn.tradeqty, txn.tradetype,
                order.seq, order.sort_price, order.volume, static_cast<int>(order.side), static_cast<int>(order.type));
        }
        result = update_volume_internal(txn.tradebuyno, (uint32_t)txn.tradeqty);
    }
    else {
        // 集合竞价：更新双方订单
        result = on_trade(txn.tradebuyno, txn.tradesellno, (uint32_t)txn.tradeqty);
    }

    return result;
}

// 打印N档盘口信息（用于调试）
void FastOrderBook::print_orderbook(int n, const std::string& context) const {
    if (!context.empty()) {
        LOG_M_INFO("========================================");
        LOG_M_INFO("{}", context);
    }

    // 打印卖十档（从低到高价格，Sell1=最优卖价=最低价）
    LOG_M_INFO("--- SELL SIDE (ASK) ---");
    auto ask_levels = get_ask_levels(n);
    for (size_t i = 0; i < ask_levels.size(); ++i) {
        LOG_M_INFO("  Sell{}: Price={} ({}元) Volume={}",
                   i + 1,
                   ask_levels[i].first,
                   ask_levels[i].first / 10000.0,
                   ask_levels[i].second);
    }

    // 打印买十档（从高到低价格）
    LOG_M_INFO("--- BUY SIDE (BID) ---");
    auto bid_levels = get_bid_levels(n);
    for (size_t i = 0; i < bid_levels.size(); ++i) {
        LOG_M_INFO("  Buy{}: Price={} ({}元) Volume={}",
                   i + 1,
                   bid_levels[i].first,
                   bid_levels[i].first / 10000.0,
                   bid_levels[i].second);
    }

    if (!context.empty()) {
        LOG_M_INFO("========================================");
    }
}
