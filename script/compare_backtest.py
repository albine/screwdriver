#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回测结果比对脚本

功能：
1. 读取股票在 strategy_live.conf 和 strategy_live_dealer2.conf 中配置的策略
2. 用配置的策略进行回测
3. 如果多个策略触发，只保留时间最早的
4. 与 signals 文件的 time 字段比对
5. 输出时间差距在 3 秒以上的股票

用法:
    python script/compare_backtest.py <signals_file> [--threshold SECONDS]

示例:
    python script/compare_backtest.py signals_20260122.csv
    python script/compare_backtest.py signals_20260122.csv --threshold 5
"""

import argparse
import csv
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent.absolute()


def load_strategy_config(config_file: Path) -> dict:
    """加载策略配置文件，返回 {symbol_base: [strategies]}"""
    config = {}
    if not config_file.exists():
        return config

    with open(config_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(',')
            if len(parts) >= 2:
                symbol_base = parts[0].strip()
                strategy = parts[1].strip()
                if symbol_base not in config:
                    config[symbol_base] = []
                if strategy not in config[symbol_base]:
                    config[symbol_base].append(strategy)
    return config


def get_strategies_for_symbol(symbol: str) -> list:
    """获取股票配置的所有策略"""
    # 提取股票代码（不带后缀）
    symbol_base = symbol.split('.')[0] if '.' in symbol else symbol

    strategies = []

    # 加载两个配置文件
    config1 = load_strategy_config(PROJECT_ROOT / "config" / "strategy_live.conf")
    config2 = load_strategy_config(PROJECT_ROOT / "config" / "strategy_live_dealer2.conf")

    # 合并策略（去重）
    if symbol_base in config1:
        strategies.extend(config1[symbol_base])
    if symbol_base in config2:
        for s in config2[symbol_base]:
            if s not in strategies:
                strategies.append(s)

    return strategies


def load_signals(signals_file: str) -> dict:
    """加载信号文件，返回 {symbol: {'time': time_str, 'price': price, 'reason': reason}}"""
    signals = {}
    with open(signals_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            symbol = row['symbol']
            # 只保留第一个信号（如果有重复）
            if symbol not in signals:
                signals[symbol] = {
                    'time': row['time'],
                    'price': row.get('price', ''),
                    'reason': row.get('reason', ''),
                }
    return signals


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
            signal['price'] = f"{price_int / 10000:.2f}"

        # 提取原因
        match = re.search(r'Reason=(.+?)(?:\s*\||\s*$)', line)
        if match:
            signal['reason'] = match.group(1).strip()

        if signal.get('time'):
            signals.append(signal)

    return signals


def parse_time(time_str: str) -> float:
    """解析时间字符串为秒数（从00:00:00开始）"""
    # 格式: HH:MM:SS.mmm 或 HH:MM:SS
    try:
        parts = time_str.split(':')
        hours = int(parts[0])
        minutes = int(parts[1])
        sec_parts = parts[2].split('.')
        seconds = int(sec_parts[0])
        millis = int(sec_parts[1]) if len(sec_parts) > 1 else 0
        # 将毫秒标准化到3位
        if len(sec_parts) > 1:
            millis_str = sec_parts[1]
            if len(millis_str) < 3:
                millis = int(millis_str) * (10 ** (3 - len(millis_str)))
            else:
                millis = int(millis_str[:3])
        return hours * 3600 + minutes * 60 + seconds + millis / 1000.0
    except:
        return 0


def time_diff_seconds(time1: str, time2: str) -> float:
    """计算两个时间的差值（秒）"""
    t1 = parse_time(time1)
    t2 = parse_time(time2)
    return abs(t1 - t2)


def run_backtest_for_symbol(symbol: str, strategies: list) -> dict:
    """对单个股票运行多个策略回测，返回最早触发的信号"""
    earliest_signal = None
    earliest_time = None

    for strategy in strategies:
        if not update_config(symbol, strategy):
            continue

        success, output = run_backtest()
        if not success:
            continue

        signals = parse_signals(output)
        for s in signals:
            s['strategy'] = strategy
            signal_time = parse_time(s.get('time', '99:99:99'))
            if earliest_time is None or signal_time < earliest_time:
                earliest_time = signal_time
                earliest_signal = s

    return earliest_signal


def main():
    parser = argparse.ArgumentParser(description='回测结果比对')
    parser.add_argument('signals_file', help='信号文件 (CSV格式)')
    parser.add_argument('--threshold', '-t', type=float, default=3.0,
                        help='时间差阈值（秒），默认 3 秒')
    parser.add_argument('--output', '-o', help='输出文件')

    args = parser.parse_args()

    # 检查文件
    if not os.path.exists(args.signals_file):
        print(f"错误: 文件不存在 {args.signals_file}")
        sys.exit(1)

    # 检查引擎
    engine_path = PROJECT_ROOT / "build" / "engine"
    if not engine_path.exists():
        print(f"错误: 引擎不存在，请先运行 ./build.sh engine")
        sys.exit(1)

    # 加载信号文件
    signals = load_signals(args.signals_file)
    print(f"回测结果比对")
    print(f"  信号文件: {args.signals_file}")
    print(f"  股票数量: {len(signals)}")
    print(f"  时间差阈值: {args.threshold} 秒")
    print()

    results = []
    skipped = []
    no_config = []

    for i, (symbol, expected) in enumerate(signals.items(), 1):
        print(f"[{i}/{len(signals)}] {symbol}...", end=" ", flush=True)

        # 检查数据是否存在
        if not check_data_exists(symbol):
            print("数据不存在，跳过")
            skipped.append(symbol)
            continue

        # 获取配置的策略
        strategies = get_strategies_for_symbol(symbol)
        if not strategies:
            print("未配置策略，跳过")
            no_config.append(symbol)
            continue

        # 运行回测
        backtest_signal = run_backtest_for_symbol(symbol, strategies)

        if backtest_signal is None:
            print(f"无信号触发 (策略: {', '.join(strategies)})")
            results.append({
                'symbol': symbol,
                'expected_time': expected['time'],
                'backtest_time': '-',
                'diff': '-',
                'strategies': ', '.join(strategies),
                'status': '无信号',
            })
            continue

        # 计算时间差
        diff = time_diff_seconds(expected['time'], backtest_signal['time'])
        status = '正常' if diff <= args.threshold else f'差异>{args.threshold}s'

        print(f"预期={expected['time']} 回测={backtest_signal['time']} 差={diff:.3f}s {status}")

        results.append({
            'symbol': symbol,
            'expected_time': expected['time'],
            'backtest_time': backtest_signal['time'],
            'diff': f"{diff:.3f}",
            'strategies': backtest_signal.get('strategy', ''),
            'expected_price': expected.get('price', ''),
            'backtest_price': backtest_signal.get('price', ''),
            'status': status,
        })

    # 统计结果
    print()
    print("=" * 70)
    print("比对结果汇总")
    print("=" * 70)

    # 筛选差异大的
    large_diff = [r for r in results if r['status'].startswith('差异')]
    no_signal = [r for r in results if r['status'] == '无信号']
    normal = [r for r in results if r['status'] == '正常']

    print(f"  正常: {len(normal)}")
    print(f"  时间差异>{args.threshold}s: {len(large_diff)}")
    print(f"  无信号触发: {len(no_signal)}")
    print(f"  数据不存在: {len(skipped)}")
    print(f"  未配置策略: {len(no_config)}")

    if large_diff:
        print()
        print(f"时间差异>{args.threshold}秒的股票:")
        print("-" * 70)
        print(f"{'股票':<12} {'预期时间':<15} {'回测时间':<15} {'差异(秒)':<10} {'策略'}")
        print("-" * 70)
        for r in large_diff:
            print(f"{r['symbol']:<12} {r['expected_time']:<15} {r['backtest_time']:<15} {r['diff']:<10} {r['strategies']}")

    if no_signal:
        print()
        print(f"无信号触发的股票:")
        print("-" * 70)
        for r in no_signal:
            print(f"  {r['symbol']} (预期: {r['expected_time']}, 策略: {r['strategies']})")

    # 输出到文件
    if args.output:
        with open(args.output, 'w', newline='', encoding='utf-8') as f:
            fieldnames = ['symbol', 'expected_time', 'backtest_time', 'diff', 'strategies', 'expected_price', 'backtest_price', 'status']
            writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(results)
        print(f"\n结果已保存到: {args.output}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
