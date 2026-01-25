#!/usr/bin/env python3
"""
策略回测自动化脚本 - ClickHouse 版本

从 ClickHouse 数据库读取 tick/order/transaction 数据进行回测。

用法:
    python script/run_strategy_backtest_clickhouse.py <股票代码> <策略名称> [日期]

示例:
    python script/run_strategy_backtest_clickhouse.py 603323.SH HenggouStrategy
    python script/run_strategy_backtest_clickhouse.py 000010 PercentageGainBreakoutStrategy 20260123

环境变量:
    CLICKHOUSE_PASSWORD: ClickHouse 密码
    CLICKHOUSE_HOST: ClickHouse 主机 (默认: market-m)

数据表:
    - MDOrderStruct: 逐笔委托
    - MDTransactionStruct: 逐笔成交
    - MDStockStruct: 快照行情
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from datetime import datetime

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.absolute()

# ClickHouse 配置
CLICKHOUSE_HOST = os.environ.get('CLICKHOUSE_HOST', 'market-m')
CLICKHOUSE_PASSWORD = os.environ.get('CLICKHOUSE_PASSWORD', '')

# 可用策略列表
AVAILABLE_STRATEGIES = [
    "OpeningRangeBreakoutStrategy",
    "PercentageGainBreakoutStrategy",
    "BreakoutPriceVolumeStrategy",
    "BreakoutPriceVolumeStrategy_v2",
    "HenggouStrategy",
    "TestOrderbookStrategy",
]


def normalize_symbol(symbol: str) -> str:
    """规范化股票代码，添加交易所后缀"""
    symbol = symbol.strip().upper()

    if symbol.endswith('.SH') or symbol.endswith('.SZ'):
        return symbol

    digits = ''.join(c for c in symbol if c.isdigit())
    if len(digits) != 6:
        raise ValueError(f"无效的股票代码: {symbol}")

    if digits.startswith('6'):
        return f"{digits}.SH"
    else:
        return f"{digits}.SZ"


def get_exchange_code(symbol: str) -> str:
    """获取交易所代码 XSHG/XSHE"""
    if symbol.endswith('.SH') or symbol.startswith('6'):
        return 'XSHG'
    return 'XSHE'


def run_clickhouse_query(query: str, output_file: str = None) -> tuple[bool, str]:
    """在远程服务器上执行 ClickHouse 查询"""
    if not CLICKHOUSE_PASSWORD:
        print("错误: 请设置 CLICKHOUSE_PASSWORD 环境变量")
        return False, ""

    # 转义查询中的特殊字符
    escaped_query = query.replace('"', '\\"')

    cmd = f'ssh {CLICKHOUSE_HOST} \'clickhouse-client --password "$CLICKHOUSE_PASSWORD" -q "{escaped_query}"\''

    if output_file:
        cmd += f' > {output_file}'

    try:
        result = subprocess.run(
            cmd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=120,
            env={**os.environ, 'CLICKHOUSE_PASSWORD': CLICKHOUSE_PASSWORD}
        )
        if result.returncode != 0:
            return False, result.stderr
        return True, result.stdout
    except Exception as e:
        return False, str(e)


def check_data_exists(symbol: str, date: str) -> dict:
    """检查 ClickHouse 中是否有数据"""
    symbol_pattern = symbol.split('.')[0]  # 去掉后缀用于 LIKE 查询

    query = f"""
    SELECT
        'orders' AS type, count(*) AS cnt FROM MDOrderStruct WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    UNION ALL
    SELECT 'transactions', count(*) FROM MDTransactionStruct WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    UNION ALL
    SELECT 'ticks', count(*) FROM MDStockStruct WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    """

    success, output = run_clickhouse_query(query)
    if not success:
        return {'orders': 0, 'transactions': 0, 'ticks': 0, 'error': output}

    counts = {'orders': 0, 'transactions': 0, 'ticks': 0}
    for line in output.strip().split('\n'):
        if line:
            parts = line.split('\t')
            if len(parts) == 2:
                counts[parts[0]] = int(parts[1])

    return counts


def export_orders(symbol: str, date: str, output_file: Path) -> int:
    """导出 Order 数据为 protobuf text 格式"""
    symbol_pattern = symbol.split('.')[0]
    exchange = get_exchange_code(symbol)

    query = f"""
    SELECT
        concat(
            'HTSCSecurityID: \\"', trim(htscsecurityid), '\\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'securityIDSource: {exchange} ',
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
    WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    ORDER BY mdtime, applseqnum
    FORMAT TabSeparatedRaw
    """

    success, output = run_clickhouse_query(query, str(output_file))
    if not success:
        print(f"  导出 Order 失败: {output}")
        return 0

    # 统计行数
    if output_file.exists():
        with open(output_file, 'r') as f:
            return sum(1 for _ in f)
    return 0


def export_transactions(symbol: str, date: str, output_file: Path) -> int:
    """导出 Transaction 数据为 protobuf text 格式"""
    symbol_pattern = symbol.split('.')[0]
    exchange = get_exchange_code(symbol)

    query = f"""
    SELECT
        concat(
            'HTSCSecurityID: \\"', trim(htscsecurityid), '\\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'securityIDSource: {exchange} ',
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
    WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    ORDER BY mdtime, applseqnum
    FORMAT TabSeparatedRaw
    """

    success, output = run_clickhouse_query(query, str(output_file))
    if not success:
        print(f"  导出 Transaction 失败: {output}")
        return 0

    if output_file.exists():
        with open(output_file, 'r') as f:
            return sum(1 for _ in f)
    return 0


def export_ticks(symbol: str, date: str, output_file: Path) -> int:
    """导出 Tick 数据为 protobuf text 格式"""
    symbol_pattern = symbol.split('.')[0]
    exchange = get_exchange_code(symbol)

    query = f"""
    SELECT
        concat(
            'HTSCSecurityID: \\"', trim(htscsecurityid), '\\" ',
            'MDDate: ', toString(mddate), ' ',
            'MDTime: ', toString(mdtime), ' ',
            'TradingPhaseCode: \\"', tradingphasecode, '\\" ',
            'securityIDSource: {exchange} ',
            'securityType: StockType ',
            'MaxPx: ', toString(maxpx), ' ',
            'MinPx: ', toString(minpx), ' ',
            'PreClosePx: ', toString(preclosepx), ' ',
            if(lastpx > 0, concat('LastPx: ', toString(lastpx), ' '), ''),
            if(openpx > 0, concat('OpenPx: ', toString(openpx), ' '), ''),
            if(highpx > 0, concat('HighPx: ', toString(highpx), ' '), ''),
            if(lowpx > 0, concat('LowPx: ', toString(lowpx), ' '), ''),
            'ChannelNo: ', toString(channelno), ' ',
            arrayStringConcat(arrayMap(x -> concat('BuyPriceQueue: ', toString(x)), buypricequeue), ' '), ' ',
            arrayStringConcat(arrayMap(x -> concat('BuyOrderQtyQueue: ', toString(x)), buyorderqtyqueue), ' '), ' ',
            arrayStringConcat(arrayMap(x -> concat('SellPriceQueue: ', toString(x)), sellpricequeue), ' '), ' ',
            arrayStringConcat(arrayMap(x -> concat('SellOrderQtyQueue: ', toString(x)), sellorderqtyqueue), ' '), ' ',
            'BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 BuyNumOrdersQueue: 0 ',
            'SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 SellNumOrdersQueue: 0 ',
            'DataMultiplePowerOf10: 4'
        )
    FROM MDStockStruct
    WHERE htscsecurityid LIKE '{symbol_pattern}%' AND mddate = {date}
    ORDER BY mdtime
    FORMAT TabSeparatedRaw
    """

    success, output = run_clickhouse_query(query, str(output_file))
    if not success:
        print(f"  导出 Tick 失败: {output}")
        return 0

    if output_file.exists():
        with open(output_file, 'r') as f:
            return sum(1 for _ in f)
    return 0


def download_from_clickhouse(symbol: str, date: str) -> bool:
    """从 ClickHouse 下载数据到 test_data 目录"""
    print(f"\n[1/3] 从 ClickHouse 下载 {symbol} ({date}) 数据...")

    # 检查数据是否存在
    counts = check_data_exists(symbol, date)
    if 'error' in counts:
        print(f"  错误: {counts['error']}")
        return False

    print(f"  Orders: {counts['orders']:,}, Transactions: {counts['transactions']:,}, Ticks: {counts['ticks']:,}")

    if counts['orders'] == 0 and counts['transactions'] == 0 and counts['ticks'] == 0:
        print(f"  错误: ClickHouse 中没有 {symbol} 在 {date} 的数据")
        return False

    # 创建输出目录
    test_data_dir = PROJECT_ROOT / "test_data"
    test_data_dir.mkdir(exist_ok=True)

    # 导出数据
    order_file = test_data_dir / f"MD_ORDER_StockType_{symbol}.csv"
    txn_file = test_data_dir / f"MD_TRANSACTION_StockType_{symbol}.csv"
    tick_file = test_data_dir / f"MD_TICK_StockType_{symbol}.csv"

    order_count = export_orders(symbol, date, order_file)
    print(f"  导出 Order: {order_count:,} 条")

    txn_count = export_transactions(symbol, date, txn_file)
    print(f"  导出 Transaction: {txn_count:,} 条")

    tick_count = export_ticks(symbol, date, tick_file)
    print(f"  导出 Tick: {tick_count:,} 条")

    return order_count > 0 or txn_count > 0


def update_config(symbol: str, strategy: str) -> bool:
    """更新回测配置文件"""
    print(f"\n[2/3] 更新配置文件...")

    config_file = PROJECT_ROOT / "config" / "strategy_backtest.conf"

    try:
        with open(config_file, 'r') as f:
            lines = f.readlines()

        new_lines = []
        found_config = False
        for line in lines:
            stripped = line.strip()
            if stripped and not stripped.startswith('#'):
                new_lines.append(f"{symbol},{strategy}\n")
                found_config = True
            else:
                new_lines.append(line)

        if not found_config:
            new_lines.append(f"{symbol},{strategy}\n")

        with open(config_file, 'w') as f:
            f.writelines(new_lines)

        print(f"  配置: {symbol},{strategy}")
        return True
    except Exception as e:
        print(f"错误: 更新配置失败 - {e}")
        return False


def run_backtest() -> tuple[bool, str]:
    """运行回测引擎"""
    print(f"\n[3/3] 运行回测...")

    engine_path = PROJECT_ROOT / "build" / "engine"
    if not engine_path.exists():
        print(f"错误: 引擎不存在，请先运行 ./build.sh engine")
        return False, ""

    env = os.environ.copy()
    lib_path = str(PROJECT_ROOT / "fastfish" / "libs")
    env['LD_LIBRARY_PATH'] = f"{lib_path}:{env.get('LD_LIBRARY_PATH', '')}"

    try:
        result = subprocess.run(
            [str(engine_path), "backtest"],
            cwd=PROJECT_ROOT,
            env=env,
            capture_output=True,
            timeout=300
        )
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        return True, output
    except subprocess.TimeoutExpired:
        print("错误: 回测超时")
        return False, ""
    except Exception as e:
        print(f"错误: {e}")
        return False, ""


def parse_backtest_output(output: str) -> dict:
    """解析回测输出"""
    result = {
        'signal': None,
        'open_info': None,
        'phases': [],
        'error': None,
    }

    lines = output.split('\n')
    for line in lines:
        if '开盘:' in line or 'prev_close=' in line:
            result['open_info'] = line
        if 'Phase1完成' in line or 'Phase2完成' in line or 'Phase1' in line or 'Phase2' in line:
            result['phases'].append(line)
        if '[SIGNAL]' in line or ('[STRA]' in line and 'BUY' in line):
            result['signal'] = line
        if '[ERR!]' in line or 'Unknown strategy' in line:
            result['error'] = line

    return result


def format_signal(signal_line: str) -> str:
    """格式化信号输出"""
    if not signal_line:
        return "无信号触发"

    parts = {}

    match = re.search(r'\[STRA\]\s+(\S+)\s+\|\s+(\w+)\s+\|', signal_line)
    if match:
        parts['symbol'] = match.group(1)
        parts['action'] = match.group(2)

    if not parts:
        match = re.search(r'\[SIGNAL\]\s+(\w+)\s+\|\s+(\S+)\s+\|', signal_line)
        if match:
            parts['action'] = match.group(1)
            parts['symbol'] = match.group(2)

    match = re.search(r'(?:Time|MARKET_TIME)=([^\s|]+)', signal_line)
    if match:
        parts['time'] = match.group(1)

    match = re.search(r'Price=(\d+)\(([^)]+)\)', signal_line)
    if match:
        parts['price_int'] = match.group(1)
        parts['price'] = match.group(2)

    match = re.search(r'Open=([^\s|]+)', signal_line)
    if match:
        parts['open'] = match.group(1)

    match = re.search(r'PrevClose=([^\s|]+)', signal_line)
    if match:
        parts['prev_close'] = match.group(1)

    price_yuan = '?'
    if 'price_int' in parts:
        try:
            price_yuan = f"{int(parts['price_int']) / 10000:.4f}"
        except:
            price_yuan = parts.get('price', '?')

    output = []
    output.append(f"[SIGNAL] {parts.get('action', '?')} | {parts.get('symbol', '?')}")
    output.append(f"  Time      = {parts.get('time', '?')}")
    output.append(f"  Price     = {price_yuan} 元")
    output.append(f"  Open      = {parts.get('open', '?')}")
    output.append(f"  PrevClose = {parts.get('prev_close', '?')}")

    return '\n'.join(output)


def main():
    parser = argparse.ArgumentParser(
        description='策略回测自动化脚本 - ClickHouse 版本',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
    python script/run_strategy_backtest_clickhouse.py 603323.SH HenggouStrategy
    python script/run_strategy_backtest_clickhouse.py 000010 PercentageGainBreakoutStrategy 20260123

环境变量:
    CLICKHOUSE_PASSWORD: ClickHouse 密码 (必需)
    CLICKHOUSE_HOST: ClickHouse 主机 (默认: market-m)
        '''
    )
    parser.add_argument('symbol', help='股票代码 (如 603323.SH 或 603323)')
    parser.add_argument('strategy', help='策略名称')
    parser.add_argument('date', nargs='?', default='20260123', help='日期 (YYYYMMDD格式，默认 20260123)')
    parser.add_argument('--no-download', action='store_true', help='跳过数据下载，使用本地已有数据')
    parser.add_argument('--verbose', '-v', action='store_true', help='显示详细输出')

    args = parser.parse_args()

    # 检查密码
    if not CLICKHOUSE_PASSWORD and not args.no_download:
        print("错误: 请设置 CLICKHOUSE_PASSWORD 环境变量")
        sys.exit(1)

    # 规范化股票代码
    try:
        symbol = normalize_symbol(args.symbol)
    except ValueError as e:
        print(f"错误: {e}")
        sys.exit(1)

    # 验证策略名称
    strategy = args.strategy
    if strategy not in AVAILABLE_STRATEGIES:
        print(f"错误: 未知策略 '{strategy}'")
        print(f"可用策略: {', '.join(AVAILABLE_STRATEGIES)}")
        sys.exit(1)

    print(f"=" * 60)
    print(f"策略回测 (ClickHouse): {symbol} - {strategy}")
    print(f"日期: {args.date}")
    print(f"=" * 60)

    # 1. 下载数据
    if args.no_download:
        print(f"\n[1/3] 跳过下载，使用本地数据")
    else:
        if not download_from_clickhouse(symbol, args.date):
            sys.exit(1)

    # 2. 更新配置
    if not update_config(symbol, strategy):
        sys.exit(1)

    # 3. 运行回测
    success, output = run_backtest()

    if not success:
        sys.exit(1)

    # 4. 显示结果
    print(f"\n{'=' * 60}")
    print("回测结果")
    print(f"{'=' * 60}")

    result = parse_backtest_output(output)

    if result['error']:
        print(f"\n错误: {result['error']}")
        sys.exit(1)

    if result['open_info'] and args.verbose:
        print(f"\n开盘信息:")
        print(f"  {result['open_info']}")

    print(f"\n{format_signal(result['signal'])}")

    if args.verbose and output:
        print(f"\n{'=' * 60}")
        print("完整输出")
        print(f"{'=' * 60}")
        print(output)


if __name__ == '__main__':
    main()
