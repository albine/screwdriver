#!/bin/bash
#
# FastOrderBook 十档盘口验证脚本
#
# 用法:
#   ./script/verify_orderbook_rebuild.sh <股票代码> [日期] [时间点]
#
# 示例:
#   ./script/verify_orderbook_rebuild.sh 600185.SH 20260126 92500000
#
# 说明:
#   - 从 ClickHouse 导出指定股票在指定时间之前的 Order/Transaction 数据
#   - 从 ClickHouse 获取指定时间之后的第一个 TICK 快照作为对比基准
#   - 运行 verify_orderbook 程序对比重建结果与交易所快照
#
# 环境要求:
#   - CLICKHOUSE_PASSWORD 环境变量 (或使用 --password 参数)
#   - clickhouse-client 命令可用
#   - build/verify_orderbook 已编译

set -e

# ============================================================================
# 参数解析
# ============================================================================

SYMBOL=$1
DATE=${2:-$(date +%Y%m%d)}
CUTOFF_TIME=${3:-92500000}  # 默认 09:25:00.000

if [ -z "$SYMBOL" ]; then
    echo "用法: $0 <股票代码> [日期] [时间点]"
    echo "示例: $0 600185.SH 20260126 92500000"
    exit 1
fi

# 提取股票代码的数字部分 (用于 LIKE 查询)
SYMBOL_CODE=$(echo "$SYMBOL" | grep -oE '^[0-9]+')
if [ -z "$SYMBOL_CODE" ]; then
    echo "错误: 无法解析股票代码: $SYMBOL"
    exit 1
fi

# ClickHouse 连接参数
CH_HOST="${CLICKHOUSE_HOST:-localhost}"
CH_PORT="${CLICKHOUSE_PORT:-9000}"
CH_DB="${CLICKHOUSE_DB:-default}"
CH_PASSWORD_ARG=""
if [ -n "$CLICKHOUSE_PASSWORD" ]; then
    CH_PASSWORD_ARG="--password $CLICKHOUSE_PASSWORD"
fi

# 临时文件
TMP_DIR="/tmp/verify_orderbook_$$"
mkdir -p "$TMP_DIR"
ORDER_FILE="$TMP_DIR/order.tsv"
TXN_FILE="$TMP_DIR/txn.tsv"
SNAPSHOT_FILE="$TMP_DIR/snapshot.txt"

# 程序路径 (支持本地开发和部署后两种路径)
if [[ -f "./build/verify_orderbook" ]]; then
    VERIFY_BIN="./build/verify_orderbook"
elif [[ -f "./bin/verify_orderbook" ]]; then
    VERIFY_BIN="./bin/verify_orderbook"
else
    echo "错误: 找不到 verify_orderbook 程序"
    echo "本地开发: ./build/verify_orderbook"
    echo "部署后: ./bin/verify_orderbook"
    exit 1
fi

# 清理函数
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# ============================================================================
# 检查依赖
# ============================================================================

echo "============================================================"
echo "FastOrderBook 十档盘口验证"
echo "============================================================"
echo "股票: $SYMBOL"
echo "日期: $DATE"
echo "截止时间: $CUTOFF_TIME"
echo ""

echo "使用验证程序: $VERIFY_BIN"

if ! command -v clickhouse-client &> /dev/null; then
    echo "错误: clickhouse-client 命令不可用"
    exit 1
fi

# ============================================================================
# 步骤 1: 获取交易所快照
# ============================================================================

echo "[1/4] 获取交易所快照 (mdtime >= $CUTOFF_TIME 的第一条)..."

# 查询快照: minpx maxpx + 买卖十档
# 注意: ClickHouse 数组从 1 开始索引
clickhouse-client --host "$CH_HOST" --port "$CH_PORT" -d "$CH_DB" $CH_PASSWORD_ARG \
    --query "
    SELECT
        minpx, maxpx,
        buypricequeue[1], buyorderqtyqueue[1],
        buypricequeue[2], buyorderqtyqueue[2],
        buypricequeue[3], buyorderqtyqueue[3],
        buypricequeue[4], buyorderqtyqueue[4],
        buypricequeue[5], buyorderqtyqueue[5],
        buypricequeue[6], buyorderqtyqueue[6],
        buypricequeue[7], buyorderqtyqueue[7],
        buypricequeue[8], buyorderqtyqueue[8],
        buypricequeue[9], buyorderqtyqueue[9],
        buypricequeue[10], buyorderqtyqueue[10],
        sellpricequeue[1], sellorderqtyqueue[1],
        sellpricequeue[2], sellorderqtyqueue[2],
        sellpricequeue[3], sellorderqtyqueue[3],
        sellpricequeue[4], sellorderqtyqueue[4],
        sellpricequeue[5], sellorderqtyqueue[5],
        sellpricequeue[6], sellorderqtyqueue[6],
        sellpricequeue[7], sellorderqtyqueue[7],
        sellpricequeue[8], sellorderqtyqueue[8],
        sellpricequeue[9], sellorderqtyqueue[9],
        sellpricequeue[10], sellorderqtyqueue[10]
    FROM MDStockStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%'
      AND mddate = $DATE
      AND mdtime >= $CUTOFF_TIME
    ORDER BY mdtime
    LIMIT 1
    FORMAT TabSeparated
" > "$SNAPSHOT_FILE"

if [ ! -s "$SNAPSHOT_FILE" ]; then
    echo "错误: 无法获取交易所快照数据"
    echo "检查: SYMBOL=$SYMBOL, DATE=$DATE, CUTOFF_TIME=$CUTOFF_TIME"
    exit 1
fi

# 显示快照的时间
SNAPSHOT_TIME=$(clickhouse-client --host "$CH_HOST" --port "$CH_PORT" -d "$CH_DB" $CH_PASSWORD_ARG \
    --query "
    SELECT mdtime
    FROM MDStockStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%'
      AND mddate = $DATE
      AND mdtime >= $CUTOFF_TIME
    ORDER BY mdtime
    LIMIT 1
")
echo "  快照时间: $SNAPSHOT_TIME"

# ============================================================================
# 步骤 2: 导出 Order 数据
# ============================================================================

echo "[2/4] 导出 Order 数据 (mdtime < $CUTOFF_TIME)..."

clickhouse-client --host "$CH_HOST" --port "$CH_PORT" -d "$CH_DB" $CH_PASSWORD_ARG \
    --query "
    SELECT
        htscsecurityid,
        mddate,
        mdtime,
        securityidsource,
        securitytype,
        orderindex,
        ordertype,
        orderprice,
        orderqty,
        orderbsflag,
        channelno,
        orderno,
        tradedqty,
        applseqnum,
        datamultiplepowerof10,
        securitystatus
    FROM MDOrderStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%'
      AND mddate = $DATE
      AND mdtime < $CUTOFF_TIME
    ORDER BY applseqnum
    FORMAT TabSeparated
" > "$ORDER_FILE"

ORDER_COUNT=$(wc -l < "$ORDER_FILE")
echo "  导出 $ORDER_COUNT 条 Order 记录"

# ============================================================================
# 步骤 3: 导出 Transaction 数据
# ============================================================================

echo "[3/4] 导出 Transaction 数据 (mdtime < $CUTOFF_TIME)..."

clickhouse-client --host "$CH_HOST" --port "$CH_PORT" -d "$CH_DB" $CH_PASSWORD_ARG \
    --query "
    SELECT
        htscsecurityid,
        mddate,
        mdtime,
        securityidsource,
        securitytype,
        tradeindex,
        tradebuyno,
        tradesellno,
        tradetype,
        tradebsflag,
        tradeprice,
        tradeqty,
        trademoney,
        applseqnum,
        channelno,
        datamultiplepowerof10
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%'
      AND mddate = $DATE
      AND mdtime < $CUTOFF_TIME
    ORDER BY applseqnum
    FORMAT TabSeparated
" > "$TXN_FILE"

TXN_COUNT=$(wc -l < "$TXN_FILE")
echo "  导出 $TXN_COUNT 条 Transaction 记录"

# ============================================================================
# 步骤 4: 运行验证程序
# ============================================================================

echo "[4/4] 运行验证程序..."
echo ""

"$VERIFY_BIN" "$SYMBOL" "$ORDER_FILE" "$TXN_FILE" "$CUTOFF_TIME" "$SNAPSHOT_FILE"
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "验证通过!"
else
    echo "验证失败 (exit code: $EXIT_CODE)"
fi

exit $EXIT_CODE
