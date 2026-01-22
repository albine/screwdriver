#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
批量策略回测脚本

用法:
    python script/batch_backtest.py <symbols_file> [--output OUTPUT_FILE]

示例:
    python script/batch_backtest.py signals_20260122_symbols_filtered.txt
    python script/batch_backtest.py signals_20260122_symbols_filtered.txt --output results.csv

输出格式: symbol, strategy, time, price, reason
"""

import argparse
import csv
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent.absolute()

# 要测试的策略列表
STRATEGIES = [
    "HenggouStrategy",
    "OpeningRangeBreakoutStrategy",
    "PercentageGainBreakoutStrategy",
]


def read_symbols(filepath: str) -> list:
    """读取股票代码列表文件"""
    symbols = []
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            symbol = line.split(',')[0].strip()
            if symbol:
                symbols.append(symbol)
    return symbols


def check_data_exists(symbol: str) -> bool:
    """检查本地是否已有数据文件"""
    test_data_dir = PROJECT_ROOT / "test_data"
    order_file = test_data_dir / f"MD_ORDER_StockType_{symbol}.csv"
    txn_file = test_data_dir / f"MD_TRANSACTION_StockType_{symbol}.csv"
    return order_file.exists() and txn_file.exists()


def update_config(symbol: str, strategy: str) -> bool:
    """更新回测配置文件"""
    config_file = PROJECT_ROOT / "config" / "strategy_backtest.conf"

    try:
        # 直接用单行配置覆盖
        with open(config_file, 'w') as f:
            f.write(f"{symbol},{strategy}\n")
        return True
    except Exception as e:
        print(f"  配置更新失败: {e}")
        return False


def run_backtest() -> tuple:
    """运行回测引擎，返回 (成功, 输出)"""
    engine_path = PROJECT_ROOT / "build" / "engine"
    if not engine_path.exists():
        return False, "引擎不存在"

    env = os.environ.copy()
    lib_path = str(PROJECT_ROOT / "fastfish" / "libs")
    env['LD_LIBRARY_PATH'] = f"{lib_path}:{env.get('LD_LIBRARY_PATH', '')}"

    try:
        result = subprocess.run(
            [str(engine_path), "backtest"],
            cwd=PROJECT_ROOT,
            env=env,
            capture_output=True,
            timeout=60
        )
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        return True, output
    except subprocess.TimeoutExpired:
        return False, "超时"
    except Exception as e:
        return False, str(e)


def parse_signals(output: str) -> list:
    """解析回测输出中的信号行，返回信号列表"""
    signals = []

    for line in output.split('\n'):
        # 兼容两种格式: [SIGNAL] 和 [STRA]
        if '[SIGNAL]' not in line and '[STRA]' not in line:
            continue
        if '[STRA]' in line and 'BUY' not in line:
            continue

        signal = {}

        # 新格式: [STRA] 600158.SH | BUY | MARKET_TIME=09:30:00.050 | Price=98750(9.88元) | ...
        match = re.search(r'\[STRA\]\s+(\S+)\s+\|\s+(\w+)\s+\|', line)
        if match:
            signal['symbol'] = match.group(1)
            signal['action'] = match.group(2)

        # 旧格式: [SIGNAL] BUY | 600403.SH | Time=09:33:14.460 | ...
        if not signal:
            match = re.search(r'\[SIGNAL\]\s+(\w+)\s+\|\s+(\S+)\s+\|', line)
            if match:
                signal['action'] = match.group(1)
                signal['symbol'] = match.group(2)

        # 提取时间 (兼容 Time= 和 MARKET_TIME=)
        match = re.search(r'(?:Time|MARKET_TIME)=([^\s|]+)', line)
        if match:
            signal['time'] = match.group(1)

        # 提取价格 Price=70884(7.09元) 或 Price=70884
        match = re.search(r'Price=(\d+)(?:\(([^)]+)\))?', line)
        if match:
            price_int = int(match.group(1))
            signal['price'] = f"{price_int / 10000:.4f}"

        # 提取原因
        match = re.search(r'Reason=(.+?)(?:\s*\||\s*$)', line)
        if match:
            signal['reason'] = match.group(1).strip()

        if signal.get('time'):
            signals.append(signal)

    return signals


def run_single_backtest(symbol: str, strategy: str) -> list:
    """对单个股票运行单个策略回测，返回信号列表"""
    if not update_config(symbol, strategy):
        return []

    success, output = run_backtest()
    if not success:
        return []

    signals = parse_signals(output)

    # 添加策略信息到每个信号
    for s in signals:
        s['strategy'] = strategy

    return signals


def main():
    parser = argparse.ArgumentParser(description='批量策略回测')
    parser.add_argument('symbols_file', help='股票代码列表文件')
    parser.add_argument('--output', '-o', help='输出CSV文件 (默认 batch_backtest_YYYYMMDD.csv)')

    args = parser.parse_args()

    # 读取股票列表
    if not os.path.exists(args.symbols_file):
        print(f"错误: 文件不存在 {args.symbols_file}")
        sys.exit(1)

    symbols = read_symbols(args.symbols_file)
    if not symbols:
        print("错误: 没有找到有效的股票代码")
        sys.exit(1)

    # 检查引擎
    engine_path = PROJECT_ROOT / "build" / "engine"
    if not engine_path.exists():
        print(f"错误: 引擎不存在，请先运行 ./build.sh engine")
        sys.exit(1)

    # 输出文件
    output_file = args.output or f"batch_backtest_{datetime.now().strftime('%Y%m%d')}.csv"

    print(f"批量策略回测")
    print(f"  股票数量: {len(symbols)}")
    print(f"  策略: {', '.join(STRATEGIES)}")
    print(f"  输出文件: {output_file}")
    print()

    all_signals = []
    skipped_symbols = []

    total_tasks = len(symbols) * len(STRATEGIES)
    current_task = 0

    for symbol in symbols:
        # 检查数据是否存在
        if not check_data_exists(symbol):
            print(f"[跳过] {symbol} - 数据不存在")
            skipped_symbols.append(symbol)
            current_task += len(STRATEGIES)
            continue

        for strategy in STRATEGIES:
            current_task += 1
            print(f"[{current_task}/{total_tasks}] {symbol} + {strategy}...", end=" ", flush=True)

            signals = run_single_backtest(symbol, strategy)

            if signals:
                print(f"触发 {len(signals)} 个信号")
                all_signals.extend(signals)
            else:
                print("无信号")

    # 写入CSV
    print()
    print(f"写入结果到 {output_file}...")

    with open(output_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['symbol', 'strategy', 'time', 'price', 'reason'])

        for s in all_signals:
            writer.writerow([
                s.get('symbol', ''),
                s.get('strategy', ''),
                s.get('time', ''),
                s.get('price', ''),
                s.get('reason', ''),
            ])

    print()
    print(f"=" * 60)
    print(f"回测完成")
    print(f"  总信号数: {len(all_signals)}")
    print(f"  跳过股票: {len(skipped_symbols)}")
    print(f"  输出文件: {output_file}")
    print(f"=" * 60)

    if skipped_symbols:
        print(f"\n跳过的股票 (数据不存在):")
        for s in skipped_symbols:
            print(f"  {s}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
