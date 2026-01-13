#include <vector>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <string>
#include "market_data_structs.h"
#include "ObjectPool.h"
#include "logger.h"

// 强类型枚举，单字节存储
enum class OrderType : int32_t {
    Limit = 1,
    Market = 2,
    Best = 3,
    ShanghaiCancel = 10
};

enum class Side : int32_t {
    Buy = 1,
    Sell = 2
};

// 成交类型
enum class TradeType : int32_t {
    Trade = 0,        // 成交
    Cancel = 1,       // 撤单
    OtherCancel2 = 2, // 其他撤单
    OtherCancel5 = 5, // 其他撤单
    OtherCancel6 = 6, // 其他撤单
    OtherCancel7 = 7, // 其他撤单
    OtherCancel8 = 8  // 其他撤单
};

// 成交买卖标志
enum class TradeBSFlag : int32_t {
    Unknown = 0,  // 不明
    Buy = 1,      // 买
    Sell = 2      // 卖
};

// ==========================================
// 1. 订单节点 (OrderNode) - POD 类型
// ==========================================
// 存放在 Object Pool 中，构成变长链表的节点
struct alignas(32) OrderNode {
    // --- 业务标识 ---
    uint64_t seq;           // 委托流水号

    // --- 价格语义分离 ---
    uint32_t original_price;// 原始委托价 (用户输入，市价单可能为0)
    uint32_t sort_price;    // 物理排序价 (实际挂入哪个 Level，用于系统定位)

    // --- 核心数据 ---
    uint32_t volume;        // 当前剩余未成交量 (热数据)

    // --- 链表索引 (核心机制) ---
    // 指向 ObjectPool 中的下标，-1 表示空
    int32_t next_idx;       
    int32_t prev_idx;       

    // --- 元数据 (压缩存储) ---
    OrderType type;         
    Side side;              
    
    // --- Padding ---
    // 自动填充以保证 32 字节对齐，利用 padding 空间不造成浪费
    uint8_t _padding[2];    
};

// ==========================================
// 2. 价格档位 (Level) - POD 类型
// ==========================================
// 存放在 Fixed Array 中，作为链表的表头
struct alignas(16) Level {
    uint32_t price;         // 价格 (冗余校验，实际由数组下标决定)
    uint32_t total_volume;  // 该档位总挂单量 (聚合统计)

    // 链表入口
    int32_t head_order_idx; // 链表头 (用于撮合成交)
    int32_t tail_order_idx; // 链表尾 (用于新单排队)
};



// ==========================================
// 4. 高性能订单簿引擎 (FastOrderBook)
// ==========================================
class FastOrderBook {
public:
    // 构造函数：需要传入全剧唯一的内存池引用
    // min_price/max_price 用于预分配 Level 数组的大小 (Offset Mapping)
    FastOrderBook(uint32_t code, ObjectPool<OrderNode>& pool, uint32_t min_price, uint32_t max_price);

    // 禁止拷贝，仅允许移动 (Resource handle)
    FastOrderBook(const FastOrderBook&) = delete;
    FastOrderBook& operator=(const FastOrderBook&) = delete;

    // --------------------------------------------------------
    // 核心写接口 (Hot Path)
    // --------------------------------------------------------

    // 处理逐笔委托消息
    bool on_order(const MDOrderStruct& order);

    // 处理逐笔成交消息
    bool on_transaction(const MDTransactionStruct& transaction);

    // --------------------------------------------------------
    // 核心读接口 (Query Path)
    // --------------------------------------------------------

    // 获取买一/卖一价 (O(1) 复杂度，无需遍历)
    std::optional<uint32_t> get_best_bid() const;
    std::optional<uint32_t> get_best_ask() const;

    // 获取某价格档位的总挂单量
    uint64_t get_volume_at_price(uint32_t price) const;

    // 计算价格区间内的累积挂单量 (用于打板策略，极速数组扫描)
    uint64_t get_ask_volume_in_range(uint32_t start_price, uint32_t end_price) const;

    // 获取买卖N档数据 (价格, 量)
    std::vector<std::pair<uint32_t, uint64_t>> get_bid_levels(int n) const;
    std::vector<std::pair<uint32_t, uint64_t>> get_ask_levels(int n) const;

    // 打印N档盘口信息（用于调试）
    void print_orderbook(int n = 10, const std::string& context = "") const;

private:
    // --------------------------------------------------------
    // 内部数据成员
    // --------------------------------------------------------
    
    const uint32_t stock_code_;
    ObjectPool<OrderNode>& pool_;

    // [核心优化] 价格档位数组 (Direct Array Mapping)
    // 访问方式: levels_[price - min_price_]
    std::vector<Level> levels_;
    uint32_t min_price_; // 价格偏移量 (Base Price)

    // [核心优化] 维护当前的 best 指针，避免每次从头扫描
    // 当 best Level 被打穿(空)时，向下/向上移动指针寻找下一个非空 Level
    int32_t best_bid_idx_ = -1; 
    int32_t best_ask_idx_ = -1;

    // 市价单队列 (不入 Level，独立排队)
    std::vector<int32_t> market_orders_;

    // 订单索引: Seq -> Pool Index
    // 生产环境建议替换为 flat_hash_map 以减少 Cache Miss
    std::unordered_map<uint64_t, int32_t> order_index_;

    // --------------------------------------------------------
    // 内部写操作 (由 on_order/on_transaction 调用)
    // --------------------------------------------------------

    // 新增订单
    bool add_order(uint64_t seq, OrderType type, Side side, uint32_t price, uint32_t volume);

    // 收到成交回报
    bool on_trade(uint64_t bid_seq, uint64_t ask_seq, uint32_t volume);

    // 收到撤单回报
    bool cancel_order(uint64_t seq, uint32_t cancel_volume);

    // --------------------------------------------------------
    // 内部辅助函数
    // --------------------------------------------------------

    // 通用的量更新逻辑 (成交/撤单共用)
    bool update_volume_internal(uint64_t seq, uint32_t delta_vol);

    // 链表操作：挂载节点到 Level 尾部
    void add_node_to_level(Level& lvl, int32_t node_idx, OrderNode& node);

    // 链表操作：从 Level 中物理摘除节点
    void remove_node_from_level(Level& lvl, int32_t node_idx, const OrderNode& node);

    // 状态维护：当最优价档位空了之后，寻找下一个最优价
    void update_best_bid_cursor();
    void update_best_ask_cursor();

    // 辅助：安全获取 Level 引用
    Level* get_level_ptr(uint32_t price);
};