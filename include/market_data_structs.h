#ifndef MARKET_DATA_STRUCTS_H
#define MARKET_DATA_STRUCTS_H

#include <cstdint>

// Plain C++ struct definitions for market data messages
// These mirror the Protocol Buffer message types from fastfish/mdc_gateway_client

// ============================================================================
// MDStockStruct - Market Data Stock Structure (79 fields)
// ============================================================================
// Contains real-time and historical stock market data including prices,
// volumes, trading statistics, and order book depth.
struct MDStockStruct {
    // Basic identification fields
    char htscsecurityid[40];              // Security identifier
    int32_t mddate;                       // Market data date
    int32_t mdtime;                       // Market data time
    int64_t datatimestamp;                // Data timestamp
    char tradingphasecode;                // Current trading phase
    int32_t securityidsource;             // Source of security ID (enum value)
    int32_t securitytype;                 // Type of security (enum value)

    // Price limits and previous close
    int64_t maxpx;                        // Maximum price (price limit up)
    int64_t minpx;                        // Minimum price (price limit down)
    int64_t preclosepx;                   // Previous close price

    // Trading statistics
    int64_t numtrades;                    // Number of trades
    int64_t totalvolumetrade;             // Total trading volume
    int64_t totalvaluetrade;              // Total trading value

    // Current price data
    int64_t lastpx;                       // Last traded price
    int64_t openpx;                       // Opening price
    int64_t closepx;                      // Closing price
    int64_t highpx;                       // Highest price
    int64_t lowpx;                        // Lowest price

    // Buy/Sell aggregate statistics
    int64_t totalbuyqty;                  // Total buy quantity
    int64_t totalsellqty;                 // Total sell quantity
    int64_t weightedavgbuypx;             // Weighted average buy price
    int64_t weightedavgsellpx;            // Weighted average sell price

    // Withdrawal statistics 只在上海有
    int64_t withdrawbuynumber;            // Number of buy withdrawals
    int64_t withdrawbuyamount;            // Buy withdrawal amount
    int64_t withdrawbuymoney;             // Buy withdrawal money
    int64_t withdrawsellnumber;           // Number of sell withdrawals
    int64_t withdrawsellamount;           // Sell withdrawal amount
    int64_t withdrawsellmoney;            // Sell withdrawal money

    // Order counts 只在上海有
    int64_t totalbuynumber;               // Total buy orders
    int64_t totalsellnumber;              // Total sell orders
    int32_t numbuyorders;                 // Number of buy orders
    int32_t numsellorders;                // Number of sell orders

    // Channel information 只深圳有
    int32_t channelno;                    // Channel number

    // Order book depth - Buy side
    int64_t buypricequeue[10];            // Buy price levels (top 10)
    int64_t buyorderqtyqueue[10];         // Buy order quantities (top 10)

    // Order book depth - Sell side
    int64_t sellpricequeue[10];           // Sell price levels (top 10)
    int64_t sellorderqtyqueue[10];        // Sell order quantities (top 10)

    // Order queue data (top 50)
    // 必须增加 count 字段，否则消费者不知道读到哪里结束
    int64_t buyorderqueue[50];            // Buy order queue
    int64_t sellorderqueue[50];           // Sell order queue
    int32_t buyorderqueue_count; // <--- 新增
    int32_t sellorderqueue_count; // <--- 新增

    // Number of orders at each level (top 50)
    int64_t buynumordersqueue[50];        // Buy number of orders queue
    int64_t sellnumordersqueue[50];       // Sell number of orders queue
    int32_t buynumordersqueue_count; // <--- 新增
    int32_t sellnumordersqueue_count; // <--- 新增

    // Data scaling factor
    int32_t datamultiplepowerof10;        // Data multiplier power of 10
};

// ============================================================================
// MDOrderStruct - Market Data Order Structure (35 fields)
// ============================================================================
// Contains detailed market data for individual orders, capturing order-level
// information in the market.
struct MDOrderStruct {
    // Basic identification fields
    char htscsecurityid[40];              // Security identifier
    int32_t mddate;                       // Market data date
    int32_t mdtime;                       // Market data time
    int32_t securityidsource;             // Source of security ID (enum value)
    int32_t securitytype;                 // Type of security (enum value)

    // Order identification and details
    int64_t orderindex;                   // Order index
    int32_t ordertype;                    // Type of order
    int64_t orderprice;                   // Order price
    int64_t orderqty;                     // Order quantity
    int32_t orderbsflag;                  // Buy/Sell flag
    int32_t channelno;                    // Channel number

    // Order tracking 上海专有orderno tradedqty
    int64_t orderno;                      // Order number
    int64_t tradedqty;                    // 已交易数量，上海专用
    // 注：applseqnum === orderindex
    int64_t applseqnum;                   // Application sequence number

    int32_t datamultiplepowerof10;        // Data multiplier power of 10

    // Security status 上海专有
    char securitystatus[16];              // 证券状态，如 "OCALL"
};

// ============================================================================
// MDTransactionStruct - Market Data Transaction Structure (48 fields)
// ============================================================================
// Contains detailed market data for executed trades/transactions with support
// for multiple asset classes including bonds, fixed income, and complex trades.
struct MDTransactionStruct {
    // Basic identification fields
    char htscsecurityid[40];              // Security identifier
    int32_t mddate;                       // Market data date
    int32_t mdtime;                       // Market data time
    int32_t securityidsource;             // Source of security ID (enum value)
    int32_t securitytype;                 // Type of security (enum value)

    // Trade identification
    int64_t tradeindex;                   // Trade index
    int64_t tradebuyno;                   // Trade buy number
    int64_t tradesellno;                  // Trade sell number

    // Trade details
    int32_t tradetype;                    // trade type
    int32_t tradebsflag;                  // Buy/Sell flag
    int64_t tradeprice;                   // Trade price
    int64_t tradeqty;                     // Trade quantity
    int64_t trademoney;                   // Trade money/value

    // Sequence and channel
    // 注：applseqnum === tradeindex
    int64_t applseqnum;                   // Application sequence number
    int32_t channelno;                    // Channel number

    int32_t datamultiplepowerof10;        // Data multiplier power of 10

};

// ============================================================================
// MDEntryDetailStruct - 盘口档位结构
// ============================================================================
struct MDEntryDetailStruct {
    int32_t level;           // 档位
    int64_t price;           // 价格
    int32_t totalqty;        // 总数量
    int32_t numberoforders;  // 订单数
};

// ============================================================================
// MDOrderbookStruct - OrderBook 快照结构
// ============================================================================
struct MDOrderbookStruct {
    // 基本字段
    char htscsecurityid[40];
    int32_t mddate;
    int32_t mdtime;
    int64_t datatimestamp;
    char tradingphasecode;
    int32_t securityidsource;
    int32_t securitytype;
    int32_t channelno;
    int64_t applseqnum;
    int64_t snapshotmddatetime;

    // 价格和统计
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

    // 买卖统计
    int64_t totalbuyqty;
    int64_t totalsellqty;
    int64_t weightedavgbuypx;
    int64_t weightedavgsellpx;
    int64_t totalbuynumber;
    int64_t totalsellnumber;
    int32_t numbuyorders;
    int32_t numsellorders;

    // 盘口数据 (最多10档)
    MDEntryDetailStruct buyentries[10];
    MDEntryDetailStruct sellentries[10];
    int32_t buyentries_count;
    int32_t sellentries_count;

    // 数据乘数
    int32_t datamultiplepowerof10;
};

#endif // MARKET_DATA_STRUCTS_H
