-- ============================================================================
-- ClickHouse 表定义 - 高频市场数据存储
-- ============================================================================
-- 注意：这些表用于盘后导入，需要与 C++ struct 布局匹配
-- 创建时运行: clickhouse-client < create_clickhouse_tables.sql
-- ============================================================================

-- ============================================================================
-- 逐笔委托表 (对应 MDOrderStruct, 144 bytes)
-- ============================================================================
-- C++ struct 布局 (含 padding):
--   htscsecurityid[40]      offset 0,   40 bytes
--   mddate                  offset 40,  4 bytes
--   mdtime                  offset 44,  4 bytes
--   securityidsource        offset 48,  4 bytes
--   securitytype            offset 52,  4 bytes
--   orderindex              offset 56,  8 bytes
--   ordertype               offset 64,  4 bytes
--   [padding]               offset 68,  4 bytes
--   orderprice              offset 72,  8 bytes
--   orderqty                offset 80,  8 bytes
--   orderbsflag             offset 88,  4 bytes
--   channelno               offset 92,  4 bytes
--   orderno                 offset 96,  8 bytes
--   tradedqty               offset 104, 8 bytes
--   applseqnum              offset 112, 8 bytes
--   datamultiplepowerof10   offset 120, 4 bytes
--   securitystatus[16]      offset 124, 16 bytes
--   [padding]               offset 140, 4 bytes
--
CREATE TABLE IF NOT EXISTS market_data.orders
(
    -- 时间相关字段
    mddate              Date,                   -- 交易日期
    mdtime              UInt32,                 -- 时间 (HHMMSS * 1000 + ms)

    -- 证券标识
    htscsecurityid      String,                 -- 证券代码 (如 "600000.SH")
    securityidsource    UInt32,                 -- 市场 (101=XSHG, 102=XSHE)
    securitytype        UInt32,                 -- 证券类型

    -- 委托数据
    orderindex          UInt64,                 -- 委托序号
    ordertype           UInt32,                 -- 委托类型
    orderprice          Int64,                  -- 委托价格 (整数，乘以 10000)
    orderqty            Int64,                  -- 委托数量
    orderbsflag         UInt32,                 -- 买卖标志 (1=买, 2=卖)
    channelno           UInt32,                 -- 通道号

    -- 上海专有字段
    orderno             Int64,                  -- 委托号
    tradedqty           Int64,                  -- 已成交数量
    applseqnum          Int64,                  -- 应用序列号
    datamultiplepowerof10 Int32,                -- 数据乘数幂
    securitystatus      String                  -- 证券状态
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(mddate)
ORDER BY (htscsecurityid, mdtime, orderindex)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- 逐笔成交表 (对应 MDTransactionStruct, 128 bytes)
-- ============================================================================
-- C++ struct 布局:
--   htscsecurityid[40]      offset 0,   40 bytes
--   mddate                  offset 40,  4 bytes
--   mdtime                  offset 44,  4 bytes
--   securityidsource        offset 48,  4 bytes
--   securitytype            offset 52,  4 bytes
--   tradeindex              offset 56,  8 bytes
--   tradebuyno              offset 64,  8 bytes
--   tradesellno             offset 72,  8 bytes
--   tradetype               offset 80,  4 bytes
--   tradebsflag             offset 84,  4 bytes
--   tradeprice              offset 88,  8 bytes
--   tradeqty                offset 96,  8 bytes
--   trademoney              offset 104, 8 bytes
--   applseqnum              offset 112, 8 bytes
--   channelno               offset 120, 4 bytes
--   datamultiplepowerof10   offset 124, 4 bytes
--
CREATE TABLE IF NOT EXISTS market_data.transactions
(
    -- 时间相关字段
    mddate              Date,
    mdtime              UInt32,

    -- 证券标识
    htscsecurityid      String,
    securityidsource    UInt32,
    securitytype        UInt32,

    -- 成交数据
    tradeindex          UInt64,                 -- 成交序号
    tradebuyno          Int64,                  -- 买方委托号
    tradesellno         Int64,                  -- 卖方委托号
    tradetype           UInt32,                 -- 成交类型
    tradebsflag         UInt32,                 -- 买卖标志
    tradeprice          Int64,                  -- 成交价格 (整数，乘以 10000)
    tradeqty            Int64,                  -- 成交数量
    trademoney          Int64,                  -- 成交金额
    applseqnum          Int64,                  -- 应用序列号
    channelno           UInt32,
    datamultiplepowerof10 Int32
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(mddate)
ORDER BY (htscsecurityid, mdtime, tradeindex)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- Tick 快照表 (对应 MDStockStruct, 2216 bytes)
-- ============================================================================
-- 字段较多，仅列出主要字段
CREATE TABLE IF NOT EXISTS market_data.ticks
(
    -- 时间相关字段
    mddate              Date,
    mdtime              UInt32,
    datatimestamp       Int64,

    -- 证券标识
    htscsecurityid      String,
    securityidsource    UInt32,
    securitytype        UInt32,
    tradingphasecode    String,

    -- 价格数据
    lastpx              Int64,                  -- 最新价
    openpx              Int64,                  -- 开盘价
    closepx             Int64,                  -- 收盘价
    highpx              Int64,                  -- 最高价
    lowpx               Int64,                  -- 最低价
    preclosepx          Int64,                  -- 昨收价
    maxpx               Int64,                  -- 涨停价
    minpx               Int64,                  -- 跌停价

    -- 成交统计
    numtrades           Int64,                  -- 成交笔数
    totalvolumetrade    Int64,                  -- 成交量
    totalvaluetrade     Int64,                  -- 成交额

    -- 买卖统计
    totalbuyqty         Int64,
    totalsellqty        Int64,
    weightedavgbuypx    Int64,
    weightedavgsellpx   Int64,

    -- 十档盘口 (买方)
    buypricequeue       Array(Int64),           -- 买价数组 [10]
    buyorderqtyqueue    Array(Int64),           -- 买量数组 [10]

    -- 十档盘口 (卖方)
    sellpricequeue      Array(Int64),           -- 卖价数组 [10]
    sellorderqtyqueue   Array(Int64),           -- 卖量数组 [10]

    channelno           UInt32,
    datamultiplepowerof10 Int32
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(mddate)
ORDER BY (htscsecurityid, mdtime)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- OrderBook 快照表 (对应 MDOrderbookStruct, 728 bytes)
-- ============================================================================
CREATE TABLE IF NOT EXISTS market_data.orderbook_snapshots
(
    -- 时间相关字段
    mddate              Date,
    mdtime              UInt32,
    datatimestamp       Int64,
    snapshotmddatetime  Int64,

    -- 证券标识
    htscsecurityid      String,
    securityidsource    UInt32,
    securitytype        UInt32,
    tradingphasecode    String,
    channelno           UInt32,
    applseqnum          Int64,

    -- 价格数据
    lastpx              Int64,
    openpx              Int64,
    closepx             Int64,
    highpx              Int64,
    lowpx               Int64,
    preclosepx          Int64,
    maxpx               Int64,
    minpx               Int64,

    -- 成交统计
    numtrades           Int64,
    totalvolumetrade    Int64,
    totalvaluetrade     Int64,

    -- 买卖统计
    totalbuyqty         Int64,
    totalsellqty        Int64,
    weightedavgbuypx    Int64,
    weightedavgsellpx   Int64,
    totalbuynumber      Int64,
    totalsellnumber     Int64,
    numbuyorders        UInt32,
    numsellorders       UInt32,

    -- 买盘十档 (Nested 结构)
    buyentries Nested (
        level           Int32,
        price           Int64,
        totalqty        Int32,
        numberoforders  Int32
    ),

    -- 卖盘十档 (Nested 结构)
    sellentries Nested (
        level           Int32,
        price           Int64,
        totalqty        Int32,
        numberoforders  Int32
    ),

    datamultiplepowerof10 Int32
)
ENGINE = MergeTree()
PARTITION BY toYYYYMMDD(mddate)
ORDER BY (htscsecurityid, mdtime, applseqnum)
SETTINGS index_granularity = 8192;


-- ============================================================================
-- 创建数据库 (如果不存在)
-- ============================================================================
-- 运行顺序: 先创建数据库，再创建表
-- CREATE DATABASE IF NOT EXISTS market_data;

-- ============================================================================
-- 用法示例
-- ============================================================================
-- 1. 创建数据库和表:
--    clickhouse-client --query "CREATE DATABASE IF NOT EXISTS market_data"
--    clickhouse-client --database market_data < create_clickhouse_tables.sql
--
-- 2. 查询示例:
--    -- 查看某日某股票的成交量
--    SELECT sum(tradeqty) FROM market_data.transactions
--    WHERE mddate = '2026-01-18' AND htscsecurityid = '600000.SH';
--
--    -- 查看十档盘口
--    SELECT htscsecurityid, mdtime, buypricequeue, buyorderqtyqueue
--    FROM market_data.ticks WHERE mddate = '2026-01-18' LIMIT 10;
