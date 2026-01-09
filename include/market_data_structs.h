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

    // Order book depth - Buy side (fixed size)
    int64_t buypricequeue[10];            // Buy price levels (top 10)
    int64_t buyorderqtyqueue[10];         // Buy order quantities (top 10)

    // Order book depth - Sell side (fixed size)
    int64_t sellpricequeue[10];           // Sell price levels (top 10)
    int64_t sellorderqtyqueue[10];        // Sell order quantities (top 10)

    // Order queue data (variable length)
    int64_t* buyorderqueue;               // Buy order queue (pointer to array)
    size_t buyorderqueue_size;            // Actual size of buy order queue
    int64_t* sellorderqueue;              // Sell order queue (pointer to array)
    size_t sellorderqueue_size;           // Actual size of sell order queue

    // Number of orders at each level (variable length)
    int64_t* buynumordersqueue;           // Buy number of orders queue (pointer to array)
    size_t buynumordersqueue_size;        // Actual size of buy num orders queue
    int64_t* sellnumordersqueue;          // Sell number of orders queue (pointer to array)
    size_t sellnumordersqueue_size;       // Actual size of sell num orders queue

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
    int64_t datatimestamp;                // Data timestamp
    int32_t securityidsource;             // Source of security ID (enum value)

    // Order identification and details
    int64_t orderindex;                   // Order index
    int32_t ordertype;                    // Type of order
    int64_t orderprice;                   // Order price
    int64_t orderqty;                     // Order quantity
    int32_t orderbsflag;                  // Buy/Sell flag
    int32_t channelno;                    // Channel number

    // Order tracking
    int64_t orderno;                      // Order number
    int64_t applseqnum;                   // Application sequence number

    int32_t datamultiplepowerof10;        // Data multiplier power of 10
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
    int32_t tradebsflag;                  // Buy/Sell flag
    int64_t tradeprice;                   // Trade price
    int64_t tradeqty;                     // Trade quantity
    int64_t trademoney;                   // Trade money/value

    // Sequence and channel
    int64_t applseqnum;                   // Application sequence number
    int32_t channelno;                    // Channel number

    int32_t datamultiplepowerof10;        // Data multiplier power of 10

};

#endif // MARKET_DATA_STRUCTS_H
