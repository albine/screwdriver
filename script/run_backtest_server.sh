#!/bin/bash
#
# 在服务器上运行策略回测
#
# 用法:
#   ./script/run_backtest_server.sh <股票代码> <策略名称> [日期]
#
# 示例:
#   ./script/run_backtest_server.sh 603323.SH HenggouStrategy 20260126
#   ./script/run_backtest_server.sh 000010 PercentageGainBreakoutStrategy
#

set -e

# 默认值
SYMBOL=${1:-""}
STRATEGY=${2:-"OpeningRangeBreakoutStrategy"}
DATE=${3:-$(date +%Y%m%d)}

REMOTE_HOST="market-m"
REMOTE_DIR="/home/jiace/project/trading-engine"

# 检查参数
if [[ -z "$SYMBOL" ]]; then
    echo "用法: $0 <股票代码> <策略名称> [日期]"
    echo ""
    echo "示例:"
    echo "  $0 603323.SH HenggouStrategy 20260126"
    echo "  $0 000010 PercentageGainBreakoutStrategy"
    echo ""
    echo "可用策略:"
    echo "  - OpeningRangeBreakoutStrategy"
    echo "  - PercentageGainBreakoutStrategy"
    echo "  - BreakoutPriceVolumeStrategy"
    echo "  - HenggouStrategy"
    exit 1
fi

# 规范化股票代码
normalize_symbol() {
    local sym=$1
    sym=${sym^^}  # 转大写

    if [[ $sym == *.SH ]] || [[ $sym == *.SZ ]]; then
        echo "$sym"
        return
    fi

    # 去除非数字
    local digits=$(echo "$sym" | tr -cd '0-9')

    if [[ ${#digits} -ne 6 ]]; then
        echo "错误: 无效的股票代码 $sym" >&2
        exit 1
    fi

    if [[ $digits == 6* ]]; then
        echo "${digits}.SH"
    else
        echo "${digits}.SZ"
    fi
}

SYMBOL=$(normalize_symbol "$SYMBOL")

echo "============================================================"
echo "服务器端策略回测"
echo "============================================================"
echo "股票: $SYMBOL"
echo "策略: $STRATEGY"
echo "日期: $DATE"
echo "服务器: $REMOTE_HOST"
echo ""

# 在服务器上执行回测
ssh $REMOTE_HOST bash -s << EOF
set -e

cd $REMOTE_DIR

echo "[1/3] 从 ClickHouse 导出数据..."

# 检查数据是否存在
SYMBOL_CODE=\$(echo "$SYMBOL" | cut -d. -f1)
COUNT=\$(clickhouse-client --password "\$CLICKHOUSE_PASSWORD" --query "
    SELECT count() FROM MDOrderStruct
    WHERE htscsecurityid LIKE '\${SYMBOL_CODE}%' AND mddate = $DATE
" 2>/dev/null)

if [[ "\$COUNT" == "0" ]]; then
    echo "错误: ClickHouse 中没有 $SYMBOL 在 $DATE 的数据"
    exit 1
fi
echo "  找到 \$COUNT 条 Order 记录"

# 确定交易所
if [[ "$SYMBOL" == *.SH ]]; then
    EXCHANGE="XSHG"
else
    EXCHANGE="XSHE"
fi

# 创建输出目录
mkdir -p test_data

# 导出 Order 数据
echo "  导出 Order 数据..."
clickhouse-client --password "\$CLICKHOUSE_PASSWORD" --query "
    SELECT
        concat(
            'HTSCSecurityID: \"', trim(htscsecurityid), '\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'securityIDSource: \$EXCHANGE ',
            'securityType: StockType ',
            'OrderIndex: ', toString(orderindex), ' ',
            if(orderno > 0, concat('OrderNO: ', toString(orderno), ' '), ''),
            'OrderType: ', toString(ordertype), ' ',
            'OrderPrice: ', toString(orderprice), ' ',
            'OrderQty: ', toString(orderqty), ' ',
            'OrderBSFlag: ', toString(orderbsflag), ' ',
            'ChannelNo: ', toString(channelno), ' ',
            'ApplSeqNum: ', toString(applseqnum), ' ',
            'DataMultiplePowerOf10: 4'
        )
    FROM MDOrderStruct
    WHERE htscsecurityid LIKE '\${SYMBOL_CODE}%' AND mddate = $DATE
    ORDER BY mdtime, applseqnum
    FORMAT TabSeparatedRaw
" > test_data/MD_ORDER_StockType_$SYMBOL.csv

ORDER_COUNT=\$(wc -l < test_data/MD_ORDER_StockType_$SYMBOL.csv)
echo "  Order: \$ORDER_COUNT 条"

# 导出 Transaction 数据
echo "  导出 Transaction 数据..."
clickhouse-client --password "\$CLICKHOUSE_PASSWORD" --query "
    SELECT
        concat(
            'HTSCSecurityID: \"', trim(htscsecurityid), '\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'securityIDSource: \$EXCHANGE ',
            'securityType: StockType ',
            'TradeIndex: ', toString(tradeindex), ' ',
            if(tradebuyno > 0, concat('TradeBuyNo: ', toString(tradebuyno), ' '), ''),
            if(tradesellno > 0, concat('TradeSellNo: ', toString(tradesellno), ' '), ''),
            'TradeType: ', toString(tradetype), ' ',
            'TradeBSFlag: ', toString(tradebsflag), ' ',
            if(tradeprice > 0, concat('TradePrice: ', toString(tradeprice), ' '), ''),
            'TradeQty: ', toString(tradeqty), ' ',
            if(trademoney > 0, concat('TradeMoney: ', toString(trademoney), ' '), ''),
            'ApplSeqNum: ', toString(applseqnum), ' ',
            'ChannelNo: ', toString(channelno), ' ',
            'DataMultiplePowerOf10: 4'
        )
    FROM MDTransactionStruct
    WHERE htscsecurityid LIKE '\${SYMBOL_CODE}%' AND mddate = $DATE
    ORDER BY mdtime, applseqnum
    FORMAT TabSeparatedRaw
" > test_data/MD_TRANSACTION_StockType_$SYMBOL.csv

TXN_COUNT=\$(wc -l < test_data/MD_TRANSACTION_StockType_$SYMBOL.csv)
echo "  Transaction: \$TXN_COUNT 条"

# 导出 Tick 数据
echo "  导出 Tick 数据..."
clickhouse-client --password "\$CLICKHOUSE_PASSWORD" --query "
    SELECT
        concat(
            'HTSCSecurityID: \"', trim(htscsecurityid), '\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'DataTimeStamp: ', toString(datatimestamp), ' ',
            'securityIDSource: \$EXCHANGE ',
            'securityType: StockType ',
            'MaxPx: ', toString(maxpx), ' ',
            'MinPx: ', toString(minpx), ' ',
            'PreClosePx: ', toString(preclosepx), ' ',
            'LastPx: ', toString(lastpx), ' ',
            'OpenPx: ', toString(openpx), ' ',
            'ClosePx: ', toString(closepx), ' ',
            'HighPx: ', toString(highpx), ' ',
            'LowPx: ', toString(lowpx), ' ',
            'NumTrades: ', toString(numtrades), ' ',
            'TotalVolumeTrade: ', toString(totalvolumetrade), ' ',
            'TotalValueTrade: ', toString(totalvaluetrade), ' ',
            'ChannelNo: ', toString(channelno), ' ',
            'DataMultiplePowerOf10: 4'
        )
    FROM MDStockStruct
    WHERE htscsecurityid LIKE '\${SYMBOL_CODE}%' AND mddate = $DATE
    ORDER BY mdtime
    FORMAT TabSeparatedRaw
" > test_data/MD_TICK_StockType_$SYMBOL.csv 2>/dev/null || echo "" > test_data/MD_TICK_StockType_$SYMBOL.csv

TICK_COUNT=\$(wc -l < test_data/MD_TICK_StockType_$SYMBOL.csv)
echo "  Tick: \$TICK_COUNT 条"

echo ""
echo "[2/3] 更新配置文件..."
echo "$SYMBOL,$STRATEGY" > config/strategy_backtest.conf
cat config/strategy_backtest.conf

echo ""
echo "[3/3] 运行回测..."

# 运行回测并保存完整日志
LOG_FILE="/tmp/backtest_\$\$.log"
./run.sh backtest > "\$LOG_FILE" 2>&1

# 提取关键信息
echo ""
echo "--- 策略初始化 ---"
grep -E "(OnFirstTick|prev_close|开盘|初始化)" "\$LOG_FILE" | head -5

echo ""
echo "--- 交易信号 ---"
SIGNALS=\$(grep -E "(SIGNAL|BUY|SELL|\[ORDR\]|\[STRA\])" "\$LOG_FILE" 2>/dev/null | grep -v "DBUG" || true)
if [[ -n "\$SIGNALS" ]]; then
    echo "\$SIGNALS"
else
    echo "(无交易信号 - 未满足触发条件)"
fi

echo ""
echo "--- 错误/警告 ---"
ERRORS=\$(grep -E "(\[ERR|\[WARN)" "\$LOG_FILE" 2>/dev/null || true)
if [[ -n "\$ERRORS" ]]; then
    echo "\$ERRORS" | head -10
else
    echo "(无错误)"
fi

# 显示策略状态变化（诊断用）
echo ""
echo "--- 策略状态 ---"
grep -E "(绿开翻红|highest_price|触发|target|突破)" "\$LOG_FILE" 2>/dev/null | tail -10 || echo "(无状态日志)"

# 统计
echo ""
echo "============================================================"
echo "回测结果统计"
echo "============================================================"
SIGNAL_COUNT=\$(grep -c "\[ORDR\]" "\$LOG_FILE" 2>/dev/null || true)
SIGNAL_COUNT=\${SIGNAL_COUNT:-0}
if [[ "\$SIGNAL_COUNT" -gt 0 ]]; then
    echo "✓ 触发信号: \$SIGNAL_COUNT 个"
else
    echo "✗ 未触发信号"
fi

# 显示关键价格信息
echo ""
OPEN_PRICE=\$(grep "open=" "\$LOG_FILE" 2>/dev/null | head -1 | grep -oP "open=[\d.]+" | cut -d= -f2 || true)
PREV_CLOSE=\$(grep "prev_close=" "\$LOG_FILE" 2>/dev/null | head -1 | grep -oP "prev_close=[\d.]+" | cut -d= -f2 || true)
if [[ -n "\$OPEN_PRICE" ]] && [[ -n "\$PREV_CLOSE" ]]; then
    echo "开盘价: \$OPEN_PRICE  昨收价: \$PREV_CLOSE"
fi

rm -f "\$LOG_FILE"
EOF
