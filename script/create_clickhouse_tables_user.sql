CREATE TABLE IF NOT EXISTS MDStockStruct
(
    -- ===== 原结构体字段：int64 标量 =====
    local_recv_timestamp       Int64  CODEC(Delta, ZSTD(1)),
    datatimestamp              Int64  CODEC(T64,   ZSTD(1)),  -- 目前全 0：T64 更合适
    maxpx                      Int64  CODEC(Delta, ZSTD(1)),
    minpx                      Int64  CODEC(Delta, ZSTD(1)),
    preclosepx                 Int64  CODEC(Delta, ZSTD(1)),
    numtrades                  Int64  CODEC(Delta, ZSTD(1)),
    totalvolumetrade           Int64  CODEC(Delta, ZSTD(1)),
    totalvaluetrade            Int64  CODEC(Delta, ZSTD(1)),
    lastpx                     Int64  CODEC(Delta, ZSTD(1)),
    openpx                     Int64  CODEC(Delta, ZSTD(1)),
    closepx                    Int64  CODEC(Delta, ZSTD(1)),
    highpx                     Int64  CODEC(Delta, ZSTD(1)),
    lowpx                      Int64  CODEC(Delta, ZSTD(1)),
    totalbuyqty                Int64  CODEC(Delta, ZSTD(1)),
    totalsellqty               Int64  CODEC(Delta, ZSTD(1)),
    weightedavgbuypx           Int64  CODEC(Delta, ZSTD(1)),
    weightedavgsellpx          Int64  CODEC(Delta, ZSTD(1)),

    withdrawbuynumber          Int64  CODEC(Delta, ZSTD(1)),
    withdrawbuyamount          Int64  CODEC(Delta, ZSTD(1)),
    withdrawbuymoney           Int64  CODEC(Delta, ZSTD(1)),
    withdrawsellnumber         Int64  CODEC(Delta, ZSTD(1)),
    withdrawsellamount         Int64  CODEC(Delta, ZSTD(1)),
    withdrawsellmoney          Int64  CODEC(Delta, ZSTD(1)),

    totalbuynumber             Int64  CODEC(Delta, ZSTD(1)),
    totalsellnumber            Int64  CODEC(Delta, ZSTD(1)),

    -- ===== 原结构体字段：int64 数组 =====
    buypricequeue              Array(Int64) CODEC(ZSTD(1)),
    buyorderqtyqueue           Array(Int64) CODEC(ZSTD(1)),
    sellpricequeue             Array(Int64) CODEC(ZSTD(1)),
    sellorderqtyqueue          Array(Int64) CODEC(ZSTD(1)),

    buyorderqueue              Array(Int64) CODEC(ZSTD(1)),  -- 50 队列（你说是订单数量）
    sellorderqueue             Array(Int64) CODEC(ZSTD(1)),
    buynumordersqueue          Array(Int64) CODEC(ZSTD(1)),
    sellnumordersqueue         Array(Int64) CODEC(ZSTD(1)),

    -- ===== 原结构体字段：int32 标量（对账关键） =====
    mddate                     Int32,
    mdtime                     Int32,
    securityidsource           Int32,
    securitytype               Int32,
    numbuyorders               Int32,
    numsellorders              Int32,
    channelno                  Int32,
    datamultiplepowerof10      Int32,

    buyorderqueue_count        Int32,
    sellorderqueue_count       Int32,
    buynumordersqueue_count    Int32,
    sellnumordersqueue_count   Int32,

    -- ===== 原结构体字段：char 数组 / char =====
    htscsecurityid             FixedString(40),
    tradingphasecode           FixedString(1),

    -- ===== 原结构体字段：padding（如你确实不想落库，可删，但会影响“字段映射全量检查”）=====
    -- _pad                       FixedString(7),

    -- ===== 额外列（不在 struct 里）：物化时间戳 =====
    event_ts DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        makeDateTime64(
            toUInt16(toUInt32(mddate) / 10000),
            toUInt8((toUInt32(mddate) / 100) % 100),
            toUInt8(toUInt32(mddate) % 100),
            toUInt8(toUInt32(mdtime) / 10000000),
            toUInt8((toUInt32(mdtime) / 100000) % 100),
            toUInt8((toUInt32(mdtime) / 1000) % 100),
            toUInt32(toUInt32(mdtime) % 1000),
            3,
            'Asia/Shanghai'
        ),

    local_recv_ts DateTime64(9, 'Asia/Shanghai') MATERIALIZED
        fromUnixTimestamp64Nano(local_recv_timestamp, 'Asia/Shanghai')
)
ENGINE = MergeTree
PARTITION BY mddate
ORDER BY (htscsecurityid, channelno, mdtime)
SETTINGS index_granularity = 8192;

CREATE TABLE IF NOT EXISTS MDOrderStruct
(
    -- ===== 原结构体字段：8 byte =====
    local_recv_timestamp      Int64  CODEC(Delta, ZSTD(1)),
    orderindex                Int64  CODEC(Delta, ZSTD(1)),
    orderprice                Int64  CODEC(Delta, ZSTD(1)),
    orderqty                  Int64  CODEC(Delta, ZSTD(1)),
    orderno                   Int64  CODEC(Delta, ZSTD(1)),
    tradedqty                 Int64  CODEC(Delta, ZSTD(1)),
    applseqnum                Int64  CODEC(Delta, ZSTD(1)),

    -- ===== 原结构体字段：4 byte（对账关键）=====
    mddate                    Int32,
    mdtime                    Int32,
    securityidsource          Int32,
    securitytype              Int32,
    ordertype                 Int32,
    orderbsflag               Int32,
    channelno                 Int32,
    datamultiplepowerof10     Int32,

    -- ===== 原结构体字段：char arrays =====
    htscsecurityid            FixedString(40),
    securitystatus            FixedString(16),

    -- ===== 额外列：物化时间戳 =====
    event_ts DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        makeDateTime64(
            toUInt16(toUInt32(mddate) / 10000),
            toUInt8((toUInt32(mddate) / 100) % 100),
            toUInt8(toUInt32(mddate) % 100),
            toUInt8(toUInt32(mdtime) / 10000000),
            toUInt8((toUInt32(mdtime) / 100000) % 100),
            toUInt8((toUInt32(mdtime) / 1000) % 100),
            toUInt32(toUInt32(mdtime) % 1000),
            3,
            'Asia/Shanghai'
        ),

    local_recv_ts DateTime64(9, 'Asia/Shanghai') MATERIALIZED
        fromUnixTimestamp64Nano(local_recv_timestamp, 'Asia/Shanghai')
)
ENGINE = MergeTree
PARTITION BY mddate
ORDER BY (htscsecurityid, channelno, mdtime, applseqnum)
SETTINGS index_granularity = 8192;

CREATE TABLE IF NOT EXISTS MDTransactionStruct
(
    -- ===== 原结构体字段：8 byte =====
    local_recv_timestamp      Int64  CODEC(Delta, ZSTD(1)),
    tradeindex                Int64  CODEC(Delta, ZSTD(1)),
    tradebuyno                Int64  CODEC(Delta, ZSTD(1)),
    tradesellno               Int64  CODEC(Delta, ZSTD(1)),
    tradeprice                Int64  CODEC(Delta, ZSTD(1)),
    tradeqty                  Int64  CODEC(Delta, ZSTD(1)),
    trademoney                Int64  CODEC(Delta, ZSTD(1)),
    applseqnum                Int64  CODEC(Delta, ZSTD(1)),

    -- ===== 原结构体字段：4 byte（对账关键）=====
    mddate                    Int32,
    mdtime                    Int32,
    securityidsource          Int32,
    securitytype              Int32,
    tradetype                 Int32,
    tradebsflag               Int32,
    channelno                 Int32,
    datamultiplepowerof10     Int32,

    -- ===== 原结构体字段：char array =====
    htscsecurityid            FixedString(40),

    -- ===== 额外列：物化时间戳 =====
    event_ts DateTime64(3, 'Asia/Shanghai') MATERIALIZED
        makeDateTime64(
            toUInt16(toUInt32(mddate) / 10000),
            toUInt8((toUInt32(mddate) / 100) % 100),
            toUInt8(toUInt32(mddate) % 100),
            toUInt8(toUInt32(mdtime) / 10000000),
            toUInt8((toUInt32(mdtime) / 100000) % 100),
            toUInt8((toUInt32(mdtime) / 1000) % 100),
            toUInt32(toUInt32(mdtime) % 1000),
            3,
            'Asia/Shanghai'
        ),

    local_recv_ts DateTime64(9, 'Asia/Shanghai') MATERIALIZED
        fromUnixTimestamp64Nano(local_recv_timestamp, 'Asia/Shanghai')
)
ENGINE = MergeTree
PARTITION BY mddate
ORDER BY (htscsecurityid, channelno, mdtime, applseqnum)
SETTINGS index_granularity = 8192;
