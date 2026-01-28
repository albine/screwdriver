#!/bin/bash
#
# FastOrderBook 本地测试脚本
#
# 用法:
#   ./script/verify_orderbook_local.sh <股票代码> [截止时间]
#
# 示例:
#   ./script/verify_orderbook_local.sh 002149.SZ 92542000
#
# 数据文件格式 (protobuf 文本格式):
#   test_data/MD_ORDER_StockType_<symbol>.csv
#   test_data/MD_TRANSACTION_StockType_<symbol>.csv
#   test_data/MD_TICK_StockType_<symbol>.csv

set -e

SYMBOL=$1
CUTOFF_TIME=${2:-92500000}

if [ -z "$SYMBOL" ]; then
    echo "用法: $0 <股票代码> [截止时间]"
    echo "示例: $0 002149.SZ 92542000"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DATA_DIR="$PROJECT_DIR/test_data"

ORDER_FILE="$DATA_DIR/MD_ORDER_StockType_${SYMBOL}.csv"
TXN_FILE="$DATA_DIR/MD_TRANSACTION_StockType_${SYMBOL}.csv"
TICK_FILE="$DATA_DIR/MD_TICK_StockType_${SYMBOL}.csv"

# 检查文件
for f in "$ORDER_FILE" "$TXN_FILE" "$TICK_FILE"; do
    if [ ! -f "$f" ]; then
        echo "错误: 文件不存在: $f"
        exit 1
    fi
done

# 程序路径
VERIFY_BIN="$PROJECT_DIR/build/verify_orderbook"
if [ ! -f "$VERIFY_BIN" ]; then
    echo "错误: 找不到 verify_orderbook, 请先编译: ./build.sh verify_orderbook"
    exit 1
fi

# 临时文件
TMP_DIR="/tmp/verify_orderbook_local_$$"
mkdir -p "$TMP_DIR"
ORDER_TSV="$TMP_DIR/order.tsv"
TXN_TSV="$TMP_DIR/txn.tsv"
SNAPSHOT_FILE="$TMP_DIR/snapshot.txt"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

echo "============================================================"
echo "FastOrderBook 本地验证"
echo "============================================================"
echo "股票: $SYMBOL"
echo "截止时间: $CUTOFF_TIME"
echo ""

# 转换 Order 数据 (protobuf text -> TSV)
echo "[1/4] 转换 Order 数据..."
grep -a "OrderType:" "$ORDER_FILE" | awk '
{
    # 解析各字段
    htscsecurityid = ""; mddate = 0; mdtime = 0; securityidsource = 0; securitytype = 0;
    orderindex = 0; ordertype = 0; orderprice = 0; orderqty = 0; orderbsflag = 0;
    channelno = 0; orderno = 0; tradedqty = 0; applseqnum = 0; datamultiplepowerof10 = 0;
    securitystatus = "";

    for (i = 1; i <= NF; i++) {
        if ($i == "HTSCSecurityID:") { gsub(/"/, "", $(i+1)); htscsecurityid = $(i+1) }
        if ($i == "MDDate:") mddate = $(i+1)
        if ($i == "MDTime:") mdtime = $(i+1)
        if ($i == "securityIDSource:") {
            if ($(i+1) == "XSHE") securityidsource = 102
            else if ($(i+1) == "XSHG") securityidsource = 101
            else securityidsource = $(i+1)
        }
        if ($i == "securityType:") securitytype = ($(i+1) == "StockType" ? 1 : 0)
        if ($i == "OrderIndex:") orderindex = $(i+1)
        if ($i == "OrderType:") ordertype = $(i+1)
        if ($i == "OrderPrice:") orderprice = $(i+1)
        if ($i == "OrderQty:") orderqty = $(i+1)
        if ($i == "OrderBSFlag:") orderbsflag = $(i+1)
        if ($i == "ChannelNo:") channelno = $(i+1)
        if ($i == "OrderNo:") orderno = $(i+1)
        if ($i == "ApplSeqNum:") applseqnum = $(i+1)
        if ($i == "DataMultiplePowerOf10:") datamultiplepowerof10 = $(i+1)
    }

    # 输出 TSV 格式
    print htscsecurityid "\t" mddate "\t" mdtime "\t" securityidsource "\t" securitytype "\t" \
          orderindex "\t" ordertype "\t" orderprice "\t" orderqty "\t" orderbsflag "\t" \
          channelno "\t" orderno "\t" tradedqty "\t" applseqnum "\t" datamultiplepowerof10 "\t" securitystatus
}
' > "$ORDER_TSV"
ORDER_COUNT=$(wc -l < "$ORDER_TSV")
echo "  转换 $ORDER_COUNT 条 Order 记录"

# 转换 Transaction 数据
echo "[2/4] 转换 Transaction 数据..."
grep -a "TradeType:" "$TXN_FILE" | awk '
{
    htscsecurityid = ""; mddate = 0; mdtime = 0; securityidsource = 0; securitytype = 0;
    tradeindex = 0; tradebuyno = 0; tradesellno = 0; tradetype = 0; tradebsflag = 0;
    tradeprice = 0; tradeqty = 0; trademoney = 0; applseqnum = 0; channelno = 0;
    datamultiplepowerof10 = 0;

    for (i = 1; i <= NF; i++) {
        if ($i == "HTSCSecurityID:") { gsub(/"/, "", $(i+1)); htscsecurityid = $(i+1) }
        if ($i == "MDDate:") mddate = $(i+1)
        if ($i == "MDTime:") mdtime = $(i+1)
        if ($i == "securityIDSource:") {
            if ($(i+1) == "XSHE") securityidsource = 102
            else if ($(i+1) == "XSHG") securityidsource = 101
            else securityidsource = $(i+1)
        }
        if ($i == "securityType:") securitytype = ($(i+1) == "StockType" ? 1 : 0)
        if ($i == "TradeIndex:") tradeindex = $(i+1)
        if ($i == "TradeBuyNo:") tradebuyno = $(i+1)
        if ($i == "TradeSellNo:") tradesellno = $(i+1)
        if ($i == "TradeType:") tradetype = $(i+1)
        if ($i == "TradeBSFlag:") tradebsflag = $(i+1)
        if ($i == "TradePrice:") tradeprice = $(i+1)
        if ($i == "TradeQty:") tradeqty = $(i+1)
        if ($i == "TradeMoney:") trademoney = $(i+1)
        if ($i == "ApplSeqNum:") applseqnum = $(i+1)
        if ($i == "ChannelNo:") channelno = $(i+1)
        if ($i == "DataMultiplePowerOf10:") datamultiplepowerof10 = $(i+1)
    }

    print htscsecurityid "\t" mddate "\t" mdtime "\t" securityidsource "\t" securitytype "\t" \
          tradeindex "\t" tradebuyno "\t" tradesellno "\t" tradetype "\t" tradebsflag "\t" \
          tradeprice "\t" tradeqty "\t" trademoney "\t" applseqnum "\t" channelno "\t" datamultiplepowerof10
}
' > "$TXN_TSV"
TXN_COUNT=$(wc -l < "$TXN_TSV")
echo "  转换 $TXN_COUNT 条 Transaction 记录"

# 提取快照数据
echo "[3/4] 提取 TICK 快照 (mdtime >= $CUTOFF_TIME 的第一条)..."
SNAPSHOT_TIME=$(grep -a "BuyPriceQueue:" "$TICK_FILE" | awk -v cutoff="$CUTOFF_TIME" '
BEGIN { found = 0 }
{
    mdtime = 0
    for (i = 1; i <= NF; i++) {
        if ($i == "MDTime:") mdtime = $(i+1)
    }

    if (found == 0 && mdtime >= cutoff) {
        found = 1
        print mdtime
        exit
    }
}
')
echo "  快照时间: $SNAPSHOT_TIME"

grep -a "BuyPriceQueue:" "$TICK_FILE" | awk -v cutoff="$CUTOFF_TIME" '
BEGIN { found = 0 }
{
    mdtime = 0
    for (i = 1; i <= NF; i++) {
        if ($i == "MDTime:") mdtime = $(i+1)
    }

    if (found == 0 && mdtime >= cutoff) {
        found = 1

        # 解析价格范围
        minpx = 0; maxpx = 0
        for (i = 1; i <= NF; i++) {
            if ($i == "MinPx:") minpx = $(i+1)
            if ($i == "MaxPx:") maxpx = $(i+1)
        }

        # 解析买卖十档
        bp_idx = 1; bv_idx = 1; sp_idx = 1; sv_idx = 1
        for (j = 1; j <= 10; j++) { buy_prices[j] = 0; buy_vols[j] = 0; sell_prices[j] = 0; sell_vols[j] = 0 }

        for (i = 1; i <= NF; i++) {
            if ($i == "BuyPriceQueue:" && bp_idx <= 10) buy_prices[bp_idx++] = $(i+1)
            if ($i == "BuyOrderQtyQueue:" && bv_idx <= 10) buy_vols[bv_idx++] = $(i+1)
            if ($i == "SellPriceQueue:" && sp_idx <= 10) sell_prices[sp_idx++] = $(i+1)
            if ($i == "SellOrderQtyQueue:" && sv_idx <= 10) sell_vols[sv_idx++] = $(i+1)
        }

        # 输出格式: minpx maxpx bid1_price bid1_vol ... ask10_vol
        printf "%d %d", minpx, maxpx
        for (j = 1; j <= 10; j++) printf " %d %d", buy_prices[j], buy_vols[j]
        for (j = 1; j <= 10; j++) printf " %d %d", sell_prices[j], sell_vols[j]
        printf "\n"
        exit
    }
}
' > "$SNAPSHOT_FILE"

if [ ! -s "$SNAPSHOT_FILE" ]; then
    echo "错误: 无法找到 mdtime >= $CUTOFF_TIME 的快照"
    exit 1
fi

# 运行验证
echo "[4/4] 运行验证程序..."
echo ""

"$VERIFY_BIN" "$SYMBOL" "$ORDER_TSV" "$TXN_TSV" "$CUTOFF_TIME" "$SNAPSHOT_FILE"
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo "验证通过!"
else
    echo "验证失败 (exit code: $EXIT_CODE)"
fi

exit $EXIT_CODE
