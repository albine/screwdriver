-- ============================================================================
-- ClickHouse 表定义 - 高频市场数据存储 (V2 格式)
-- ============================================================================
-- 对应 include/market_data_structs_aligned.h 中的 V2 结构体
--
-- 用法:
--   clickhouse-client --password <pwd> -q "CREATE DATABASE IF NOT EXISTS market_data"
--   clickhouse-client --password <pwd> --database market_data < create_clickhouse_tables_v2.sql
-- ============================================================================

-- ============================================================================
-- 1. 逐笔委托表 (MDOrderStruct V2, 144 bytes)
-- ============================================================================
-- C++ struct 布局:
--   int64_t local_recv_timestamp     offset 0
--   int64_t orderindex               offset 8
--   int64_t orderprice               offset 16
--   int64_t orderqty                 offset 24
--   int64_t orderno                  offset 32
--   int64_t tradedqty                offset 40
--   int64_t applseqnum               offset 48
--   int32_t mddate                   offset 56
--   int32_t mdtime                   offset 60
--   int32_t securityidsource         offset 64
--   int32_t securitytype             offset 68
--   int32_t ordertype                offset 72
--   int32_t orderbsflag              offset 76
--   int32_t channelno                offset 80
--   int32_t datamultiplepowerof10    offset 84
--   char    htscsecurityid[40]       offset 88
--   char    securitystatus[16]       offset 128
-- Total: 144 bytes
--
DROP TABLE IF EXISTS market_data.orders;
CREATE TABLE market_data.orders
(
    -- 时间字段（真实列）
    mddate                UInt32,                   -- YYYYMMDD 格式
    mdtime                UInt32,                   -- HHMMSSmmm 格式

    -- 合成时间戳（MATERIALIZED，自动存储）
    event_ts              DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        toDateTime64(
            concat(
                toString(mddate DIV 10000), '-',
                leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
                leftPad(toString(mddate MOD 100), 2, '0'), ' ',
                leftPad(toString(mdtime DIV 10000000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 10000000) DIV 100000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 100000) DIV 1000), 2, '0'), '.',
                leftPad(toString(mdtime MOD 1000), 3, '0')
            ), 3, 'Asia/Shanghai'
        ),

    -- 证券标识
    htscsecurityid        String,                   -- 证券代码 (如 "600000.SH")
    securityidsource      UInt32,                   -- 市场 (101=XSHG, 102=XSHE)
    securitytype          UInt32,                   -- 证券类型
    securitystatus        String,                   -- 证券状态

    -- 委托数据
    orderindex            Int64,                    -- 委托序号
    ordertype             UInt32,                   -- 委托类型
    orderprice            Int64,                    -- 委托价格 (整数，乘以 10000)
    orderqty              Int64,                    -- 委托数量
    orderbsflag           UInt32,                   -- 买卖标志 (1=买, 2=卖)
    channelno             UInt32,                   -- 通道号
    orderno               Int64,                    -- 委托号
    tradedqty             Int64,                    -- 已成交数量
    applseqnum            Int64,                    -- 应用序列号
    datamultiplepowerof10 Int32,                    -- 数据乘数幂

    -- 本地接收时间戳（纳秒）
    local_recv_timestamp  Int64
)
ENGINE = MergeTree()
PARTITION BY toYYYYMM(toDate(
    concat(
        toString(mddate DIV 10000), '-',
        leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
        leftPad(toString(mddate MOD 100), 2, '0')
    )
))
ORDER BY (htscsecurityid, mddate, mdtime, orderindex)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- 2. 逐笔成交表 (MDTransactionStruct V2, 136 bytes)
-- ============================================================================
-- C++ struct 布局:
--   int64_t local_recv_timestamp     offset 0
--   int64_t tradeindex               offset 8
--   int64_t tradebuyno               offset 16
--   int64_t tradesellno              offset 24
--   int64_t tradeprice               offset 32
--   int64_t tradeqty                 offset 40
--   int64_t trademoney               offset 48
--   int64_t applseqnum               offset 56
--   int32_t mddate                   offset 64
--   int32_t mdtime                   offset 68
--   int32_t securityidsource         offset 72
--   int32_t securitytype             offset 76
--   int32_t tradetype                offset 80
--   int32_t tradebsflag              offset 84
--   int32_t channelno                offset 88
--   int32_t datamultiplepowerof10    offset 92
--   char    htscsecurityid[40]       offset 96
-- Total: 136 bytes
--
DROP TABLE IF EXISTS market_data.transactions;
CREATE TABLE market_data.transactions
(
    -- 时间字段（真实列）
    mddate                UInt32,
    mdtime                UInt32,

    -- 合成时间戳（MATERIALIZED）
    event_ts              DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        toDateTime64(
            concat(
                toString(mddate DIV 10000), '-',
                leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
                leftPad(toString(mddate MOD 100), 2, '0'), ' ',
                leftPad(toString(mdtime DIV 10000000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 10000000) DIV 100000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 100000) DIV 1000), 2, '0'), '.',
                leftPad(toString(mdtime MOD 1000), 3, '0')
            ), 3, 'Asia/Shanghai'
        ),

    -- 证券标识
    htscsecurityid        String,
    securityidsource      UInt32,
    securitytype          UInt32,

    -- 成交数据
    tradeindex            Int64,                    -- 成交序号
    tradebuyno            Int64,                    -- 买方委托号
    tradesellno           Int64,                    -- 卖方委托号
    tradetype             UInt32,                   -- 成交类型
    tradebsflag           UInt32,                   -- 买卖标志
    tradeprice            Int64,                    -- 成交价格 (整数，乘以 10000)
    tradeqty              Int64,                    -- 成交数量
    trademoney            Int64,                    -- 成交金额
    applseqnum            Int64,                    -- 应用序列号
    channelno             UInt32,
    datamultiplepowerof10 Int32,

    -- 本地接收时间戳（纳秒）
    local_recv_timestamp  Int64
)
ENGINE = MergeTree()
PARTITION BY toYYYYMM(toDate(
    concat(
        toString(mddate DIV 10000), '-',
        leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
        leftPad(toString(mddate MOD 100), 2, '0')
    )
))
ORDER BY (htscsecurityid, mddate, mdtime, tradeindex)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- 3. Tick 快照表 (MDStockStruct V2, 2216 bytes)
-- ============================================================================
-- C++ struct 布局 (主要字段):
--   int64_t local_recv_timestamp     offset 0
--   int64_t datatimestamp            offset 8
--   int64_t maxpx                    offset 16
--   ... (更多 int64 标量)
--   int64_t buypricequeue[10]        offset 200
--   int64_t buyorderqtyqueue[10]     offset 280
--   int64_t sellpricequeue[10]       offset 360
--   int64_t sellorderqtyqueue[10]    offset 440
--   int64_t buyorderqueue[50]        offset 520
--   int64_t sellorderqueue[50]       offset 920
--   int64_t buynumordersqueue[50]    offset 1320
--   int64_t sellnumordersqueue[50]   offset 1720
--   int32_t mddate                   offset 2120
--   int32_t mdtime                   offset 2124
--   ... (更多 int32)
--   char    htscsecurityid[40]       offset 2168
--   char    tradingphasecode         offset 2208
--   char    _pad[7]                  offset 2209
-- Total: 2216 bytes
--
DROP TABLE IF EXISTS market_data.ticks;
CREATE TABLE market_data.ticks
(
    -- 时间字段（真实列）
    mddate                UInt32,
    mdtime                UInt32,

    -- 合成时间戳（MATERIALIZED）
    event_ts              DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        toDateTime64(
            concat(
                toString(mddate DIV 10000), '-',
                leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
                leftPad(toString(mddate MOD 100), 2, '0'), ' ',
                leftPad(toString(mdtime DIV 10000000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 10000000) DIV 100000), 2, '0'), ':',
                leftPad(toString((mdtime MOD 100000) DIV 1000), 2, '0'), '.',
                leftPad(toString(mdtime MOD 1000), 3, '0')
            ), 3, 'Asia/Shanghai'
        ),

    -- 证券标识
    htscsecurityid        String,
    securityidsource      UInt32,
    securitytype          UInt32,
    tradingphasecode      String,                   -- 交易阶段代码
    channelno             UInt32,

    -- 时间戳
    datatimestamp         Int64,                    -- 数据时间戳
    local_recv_timestamp  Int64,                    -- 本地接收时间戳（纳秒）

    -- 价格数据
    lastpx                Int64,                    -- 最新价
    openpx                Int64,                    -- 开盘价
    closepx               Int64,                    -- 收盘价
    highpx                Int64,                    -- 最高价
    lowpx                 Int64,                    -- 最低价
    preclosepx            Int64,                    -- 昨收价
    maxpx                 Int64,                    -- 涨停价
    minpx                 Int64,                    -- 跌停价

    -- 成交统计
    numtrades             Int64,                    -- 成交笔数
    totalvolumetrade      Int64,                    -- 成交量
    totalvaluetrade       Int64,                    -- 成交额

    -- 买卖统计
    totalbuyqty           Int64,
    totalsellqty          Int64,
    weightedavgbuypx      Int64,
    weightedavgsellpx     Int64,
    numbuyorders          UInt32,
    numsellorders         UInt32,

    -- 上海专用字段
    withdrawbuynumber     Int64,
    withdrawbuyamount     Int64,
    withdrawbuymoney      Int64,
    withdrawsellnumber    Int64,
    withdrawsellamount    Int64,
    withdrawsellmoney     Int64,
    totalbuynumber        Int64,
    totalsellnumber       Int64,

    -- 十档盘口价格和数量
    buypricequeue         Array(Int64),             -- 买价数组 [10]
    buyorderqtyqueue      Array(Int64),             -- 买量数组 [10]
    sellpricequeue        Array(Int64),             -- 卖价数组 [10]
    sellorderqtyqueue     Array(Int64),             -- 卖量数组 [10]

    -- 50 档订单队列（可选，数据量大）
    buyorderqueue         Array(Int64),             -- 买单队列 [50]
    sellorderqueue        Array(Int64),             -- 卖单队列 [50]
    buynumordersqueue     Array(Int64),             -- 买单笔数队列 [50]
    sellnumordersqueue    Array(Int64),             -- 卖单笔数队列 [50]

    -- 队列计数
    buyorderqueue_count   UInt32,
    sellorderqueue_count  UInt32,
    buynumordersqueue_count UInt32,
    sellnumordersqueue_count UInt32,

    datamultiplepowerof10 Int32
)
ENGINE = MergeTree()
PARTITION BY toYYYYMM(toDate(
    concat(
        toString(mddate DIV 10000), '-',
        leftPad(toString((mddate MOD 10000) DIV 100), 2, '0'), '-',
        leftPad(toString(mddate MOD 100), 2, '0')
    )
))
ORDER BY (htscsecurityid, mddate, mdtime)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- 查询示例
-- ============================================================================
--
-- 1. 查看某日某股票的逐笔委托:
--    SELECT event_ts, htscsecurityid, orderprice/10000.0 as price, orderqty, orderbsflag
--    FROM market_data.orders
--    WHERE mddate = 20260123 AND htscsecurityid = '600000.SH'
--    ORDER BY mdtime LIMIT 10;
--
-- 2. 查看成交汇总:
--    SELECT htscsecurityid, sum(tradeqty) as total_qty, sum(trademoney)/10000.0 as total_amount
--    FROM market_data.transactions
--    WHERE mddate = 20260123
--    GROUP BY htscsecurityid;
--
-- 3. 查看 Tick 十档盘口:
--    SELECT event_ts, htscsecurityid,
--           arrayMap(x -> x/10000.0, buypricequeue) as bid_prices,
--           buyorderqtyqueue as bid_qtys
--    FROM market_data.ticks
--    WHERE mddate = 20260123 AND htscsecurityid = '600759.SH'
--    LIMIT 5;
--
