#ifndef MARKET_DATA_STRUCTS_ALIGNED_H
#define MARKET_DATA_STRUCTS_ALIGNED_H

#include <cstdint>

// Plain C++ struct definitions for market data messages
// These mirror the Protocol Buffer message types from fastfish/mdc_gateway_client

// ============================================================================
// MDStockStruct - Market Data Stock Structure (79 fields)
// ============================================================================
// Contains real-time and historical stock market data including prices,
// volumes, trading statistics, and order book depth.
struct MDStockStruct {
    // 1. 所有的 int64 标量 (136 bytes)
    int64_t local_recv_timestamp;
    int64_t datatimestamp;
    int64_t maxpx;
    int64_t minpx;
    int64_t preclosepx;
    int64_t numtrades;
    int64_t totalvolumetrade;
    int64_t totalvaluetrade;
    int64_t lastpx;
    int64_t openpx;
    int64_t closepx;
    int64_t highpx;
    int64_t lowpx;
    int64_t totalbuyqty;
    int64_t totalsellqty;
    int64_t weightedavgbuypx;
    int64_t weightedavgsellpx;

    // 上海专用 int64 (48 bytes)
    int64_t withdrawbuynumber;
    int64_t withdrawbuyamount;
    int64_t withdrawbuymoney;
    int64_t withdrawsellnumber;
    int64_t withdrawsellamount;
    int64_t withdrawsellmoney;
    
    // 上海专用 int64 (16 bytes)
    int64_t totalbuynumber;
    int64_t totalsellnumber;

    // 2. 所有的 int64 数组 (Bulk Data) - 必须 8 字节对齐
    int64_t buypricequeue[10];
    int64_t buyorderqtyqueue[10];
    int64_t sellpricequeue[10];
    int64_t sellorderqtyqueue[10];
    
    int64_t buyorderqueue[50];
    int64_t sellorderqueue[50];
    int64_t buynumordersqueue[50];
    int64_t sellnumordersqueue[50];

    // 3. 所有的 int32 标量 (48 bytes)
    int32_t mddate;
    int32_t mdtime;
    int32_t securityidsource;
    int32_t securitytype;
    int32_t numbuyorders;
    int32_t numsellorders;
    int32_t channelno;
    int32_t datamultiplepowerof10;
    
    // 新增的 count 字段
    int32_t buyorderqueue_count;
    int32_t sellorderqueue_count;
    int32_t buynumordersqueue_count;
    int32_t sellnumordersqueue_count;

    // 4. 所有的 char 数组 (41 bytes)
    char htscsecurityid[40];
    char tradingphasecode; // 1 byte

    // 5. 显式 Padding (补齐到 8 的倍数)
    // 目前 int32 有 12 个 (48 bytes) -> 8字节对齐 OK
    // char 数组 41 bytes -> 41 % 8 = 1. 需要 7 bytes padding 
    char _pad[7];
};
static_assert(sizeof(MDStockStruct) == 2216, "Size mismatch");

// ============================================================================
// MDOrderStruct - Market Data Order Structure (35 fields)
// ============================================================================
// Contains detailed market data for individual orders, capturing order-level
// information in the market.
struct MDOrderStruct {
    // --- 8 Byte Types (56 bytes) ---
    int64_t local_recv_timestamp;
    int64_t orderindex;                   // orderindex / applseqnum
    int64_t orderprice;
    int64_t orderqty;
    int64_t orderno;
    int64_t tradedqty;
    int64_t applseqnum;

    // --- 4 Byte Types (32 bytes) ---
    int32_t mddate;
    int32_t mdtime;
    int32_t securityidsource;
    int32_t securitytype;
    int32_t ordertype;
    int32_t orderbsflag;
    int32_t channelno;
    int32_t datamultiplepowerof10;

    // --- Arrays (56 bytes) ---
    char htscsecurityid[40];
    char securitystatus[16];
};
static_assert(sizeof(MDOrderStruct) == 144, "Size mismatch");

// ============================================================================
// MDTransactionStruct - Market Data Transaction Structure (48 fields)
// ============================================================================
// Contains detailed market data for executed trades/transactions with support
// for multiple asset classes including bonds, fixed income, and complex trades.
struct MDTransactionStruct {
    // --- 8 Byte Types (64 bytes) ---
    int64_t local_recv_timestamp;
    int64_t tradeindex;                   // tradeindex / applseqnum
    int64_t tradebuyno;
    int64_t tradesellno;
    int64_t tradeprice;
    int64_t tradeqty;
    int64_t trademoney;
    int64_t applseqnum;                   // Redundant in memory but kept for schema compatibility

    // --- 4 Byte Types (32 bytes) ---
    int32_t mddate;
    int32_t mdtime;
    int32_t securityidsource;
    int32_t securitytype;
    int32_t tradetype;
    int32_t tradebsflag;
    int32_t channelno;
    int32_t datamultiplepowerof10;

    // --- Arrays (40 bytes) ---
    char htscsecurityid[40];              // 40 is divisible by 8, keeps alignment
};
static_assert(sizeof(MDTransactionStruct) == 136, "Size mismatch");

// ============================================================================
// MDEntryDetailStruct - 盘口档位结构
// ============================================================================
struct MDEntryDetailStruct {
    int64_t price;           // 8 bytes (Move to top)
    int32_t level;           // 4 bytes
    int32_t totalqty;        // 4 bytes
    int32_t numberoforders;  // 4 bytes
    int32_t _pad;            // 4 bytes (Explicit padding to reach 24 bytes and 8-byte alignment)
};
static_assert(sizeof(MDEntryDetailStruct) == 24, "Size mismatch");

// ============================================================================
// MDOrderbookStruct - OrderBook 快照结构
// ============================================================================
struct MDOrderbookStruct {
    // ==========================================================
    // 1. 嵌套结构体数组 (Largest alignment requirement: 8 bytes)
    // ==========================================================
    // Total: 480 bytes
    // 注意：MDEntryDetailStruct 必须是优化后的 24 字节版本
    MDEntryDetailStruct buyentries[10];
    MDEntryDetailStruct sellentries[10];

    // ==========================================================
    // 2. int64_t 字段 (8 bytes)
    // ==========================================================
    // Total: 168 bytes
    int64_t local_recv_timestamp;
    int64_t datatimestamp;
    int64_t applseqnum;
    int64_t snapshotmddatetime;
    int64_t numtrades;
    int64_t totalvolumetrade;
    int64_t totalvaluetrade;
    int64_t lastpx;
    int64_t highpx;
    int64_t lowpx;
    int64_t maxpx;
    int64_t minpx;
    int64_t preclosepx;
    int64_t openpx;
    int64_t closepx;
    int64_t totalbuyqty;
    int64_t totalsellqty;
    int64_t weightedavgbuypx;
    int64_t weightedavgsellpx;
    int64_t totalbuynumber;
    int64_t totalsellnumber;

    // ==========================================================
    // 3. char 数组 (按 8 字节对齐处理)
    // ==========================================================
    // Total: 40 bytes
    char htscsecurityid[40]; 

    // ==========================================================
    // 4. int32_t 字段 (4 bytes)
    // ==========================================================
    // Total: 40 bytes
    int32_t mddate;
    int32_t mdtime;
    int32_t securityidsource;
    int32_t securitytype;
    int32_t channelno;
    int32_t numbuyorders;
    int32_t numsellorders;
    int32_t buyentries_count;
    int32_t sellentries_count;
    int32_t datamultiplepowerof10;

    // ==========================================================
    // 5. char/int8 字段 (1 byte)
    // ==========================================================
    // Total: 1 byte
    char tradingphasecode;

    // ==========================================================
    // 6. Explicit Padding (显式填充)
    // ==========================================================
    // 当前 offset: 721. 目标: 728. 缺: 7.
    char _pad[7]; 
};

// 静态检查确保大小精确为 728 且无隐式 padding
static_assert(sizeof(MDOrderbookStruct) == 736, "MDOrderbookStruct size mismatch - update ClickHouse schema!");

// ============================================================================
// Magic values for aligned mmap format (V2)
// ============================================================================
// V2 格式特征：
// 1. 结构体对齐优化（8字节对齐）
// 2. 新增 local_recv_timestamp 字段
// 3. 使用新 Magic 区分 V1 和 V2 格式
constexpr uint32_t MAGIC_ORDER_V2 = 0x4F524432;       // "ORD2"
constexpr uint32_t MAGIC_TRANSACTION_V2 = 0x54584E32; // "TXN2"
constexpr uint32_t MAGIC_TICK_V2 = 0x54494B32;        // "TIK2"
constexpr uint32_t MAGIC_ORDERBOOK_V2 = 0x4F424B32;   // "OBK2"

#endif // MARKET_DATA_STRUCTS_ALIGNED_H
