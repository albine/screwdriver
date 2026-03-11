#!/bin/bash
#
# 逐笔数据完整性验证脚本
#
# 检查项:
#   1. 每笔成交的 tradebuyno/tradesellno 是否能匹配到 Order 表中的订单
#   2. Order 总量 vs Transaction 总量的平衡
#   3. applseqnum 连续性检查
#
# 用法:
#   ./script/verify_data_integrity.sh <股票代码> [日期]
#
# 示例:
#   ./script/verify_data_integrity.sh 000669.SZ 20260311

set -e

SYMBOL=$1
DATE=${2:-$(date +%Y%m%d)}

if [ -z "$SYMBOL" ]; then
    echo "用法: $0 <股票代码> [日期]"
    exit 1
fi

SYMBOL_CODE=$(echo "$SYMBOL" | grep -oE '^[0-9]+')

# 判断交易所
if [[ "$SYMBOL" == *.SH ]] || [[ "$SYMBOL_CODE" == 6* ]]; then
    EXCHANGE="SH"
    SOURCE=101
else
    EXCHANGE="SZ"
    SOURCE=102
fi

CH="clickhouse-client --password \$CLICKHOUSE_PASSWORD -d default"

echo "============================================================"
echo "逐笔数据完整性验证"
echo "============================================================"
echo "股票: $SYMBOL ($EXCHANGE)"
echo "日期: $DATE"
echo ""

# ============================================================
# 1. 基础统计
# ============================================================
echo "=== 1. 基础统计 ==="

ssh market-m "$CH -q \"
SELECT
  'Order' as table,
  count() as cnt,
  min(applseqnum) as min_seq,
  max(applseqnum) as max_seq,
  max(applseqnum) - min(applseqnum) + 1 as expected_cnt,
  max(applseqnum) - min(applseqnum) + 1 - count() as missing_cnt
FROM MDOrderStruct
WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
\""

ssh market-m "$CH -q \"
SELECT
  'Transaction' as table,
  count() as cnt,
  min(applseqnum) as min_seq,
  max(applseqnum) as max_seq,
  max(applseqnum) - min(applseqnum) + 1 as expected_cnt,
  max(applseqnum) - min(applseqnum) + 1 - count() as missing_cnt
FROM MDTransactionStruct
WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
\""

# ============================================================
# 2. SZ: applseqnum 全局连续性 (Order+Transaction 共享序列)
# ============================================================
if [ "$EXCHANGE" = "SZ" ]; then
    echo ""
    echo "=== 2. 深圳 applseqnum 全局连续性 (Order+Transaction 共享序列) ==="

    ssh market-m "$CH -q \"
    WITH all_seqs AS (
      SELECT applseqnum FROM MDOrderStruct
      WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      UNION ALL
      SELECT applseqnum FROM MDTransactionStruct
      WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
    )
    SELECT
      count() as total_events,
      min(applseqnum) as min_seq,
      max(applseqnum) as max_seq,
      max(applseqnum) - min(applseqnum) + 1 as expected,
      max(applseqnum) - min(applseqnum) + 1 - count() as gaps
    FROM all_seqs
    \""
fi

# ============================================================
# 3. 成交引用的订单匹配率
# ============================================================
echo ""
echo "=== 3. 成交引用的订单匹配率 ==="

if [ "$EXCHANGE" = "SZ" ]; then
    # 深圳: tradebuyno/tradesellno 对应 orderindex
    # tradetype=0 是成交, 其他是撤单
    echo "--- 成交 (tradetype=0): tradebuyno -> orderindex ---"
    ssh market-m "$CH -q \"
    SELECT
      count() as total_trades,
      countIf(tradebuyno IN (
        SELECT orderindex FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      )) as buy_matched,
      countIf(tradesellno IN (
        SELECT orderindex FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      )) as sell_matched
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND tradetype = 0
    \""

    echo ""
    echo "--- 撤单 (tradetype!=0): tradebuyno/tradesellno -> orderindex ---"
    ssh market-m "$CH -q \"
    SELECT
      count() as total_cancels,
      countIf(tradebsflag = 1 AND tradebuyno IN (
        SELECT orderindex FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      )) as buy_cancel_matched,
      countIf(tradebsflag = 2 AND tradesellno IN (
        SELECT orderindex FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      )) as sell_cancel_matched
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND tradetype != 0
    \""
else
    # 上海: tradebuyno/tradesellno 对应 orderno
    echo "--- 成交 (tradetype=0): tradebuyno -> orderno ---"
    ssh market-m "$CH -q \"
    SELECT
      count() as total_trades,
      countIf(tradebuyno IN (
        SELECT orderno FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND orderno > 0
      )) as buy_matched,
      countIf(tradesellno IN (
        SELECT orderno FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND orderno > 0
      )) as sell_matched
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND tradetype = 0
    \""
fi

# ============================================================
# 4. 买单量 vs 消耗量平衡检查 (涨停价)
# ============================================================
echo ""
echo "=== 4. 涨停价上的量平衡 ==="

MAXPX=$(ssh market-m "$CH -q \"
SELECT max(maxpx) FROM MDStockStruct
WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE AND maxpx > 0
\"")
echo "涨停价: $MAXPX"

echo "--- 涨停价买单 ---"
ssh market-m "$CH -q \"
SELECT
  count() as order_cnt,
  sum(orderqty) as total_qty
FROM MDOrderStruct
WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
  AND orderbsflag = 1 AND orderprice = $MAXPX
\""

echo "--- 涨停价成交 (消耗买单) ---"
ssh market-m "$CH -q \"
SELECT
  count() as trade_cnt,
  sum(tradeqty) as total_qty
FROM MDTransactionStruct
WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
  AND tradeprice = $MAXPX AND tradetype = 0
\""

echo "--- 涨停价撤单 (买方) ---"
if [ "$EXCHANGE" = "SZ" ]; then
    ssh market-m "$CH -q \"
    SELECT
      count() as cancel_cnt,
      sum(tradeqty) as total_qty
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      AND tradetype != 0 AND tradebsflag = 1
      AND tradebuyno IN (
        SELECT orderindex FROM MDOrderStruct
        WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
          AND orderbsflag = 1 AND orderprice = $MAXPX
      )
    \""
else
    ssh market-m "$CH -q \"
    SELECT
      count() as cancel_cnt,
      sum(orderqty) as total_qty
    FROM MDOrderStruct
    WHERE htscsecurityid LIKE '${SYMBOL_CODE}%' AND mddate = $DATE
      AND orderbsflag = 1 AND orderprice = $MAXPX AND ordertype = 10
    \""
fi

echo ""
echo "============================================================"
echo "验证完成"
echo "============================================================"
