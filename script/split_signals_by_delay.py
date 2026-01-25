#!/usr/bin/env python3
"""
按延迟时间分流信号文件

将 signals CSV 文件按 timestamp 和 time 的差值分成两个文件：
- 延迟 >= 阈值秒的条目
- 延迟 < 阈值秒的条目

用法:
    python script/split_signals_by_delay.py <input_csv> [--threshold 5]

示例:
    python script/split_signals_by_delay.py signals_20260123.csv
    python script/split_signals_by_delay.py signals_20260123.csv --threshold 3
"""

import argparse
import csv
import os
from datetime import datetime
from pathlib import Path


def parse_timestamp(ts_str: str, date_str: str = None) -> datetime:
    """解析 timestamp 字符串 (纳秒精度，截断到微秒)"""
    ts_parts = ts_str.split('.')
    ts_base = ts_parts[0]
    ts_frac = ts_parts[1][:6] if len(ts_parts) > 1 else '0'
    return datetime.strptime(f"{ts_base}.{ts_frac}", "%Y-%m-%d %H:%M:%S.%f")


def parse_time(time_str: str, date_str: str) -> datetime:
    """解析 time 字符串 (毫秒精度)"""
    time_parts = time_str.split('.')
    time_base = time_parts[0]
    time_frac = (time_parts[1] + '000')[:6] if len(time_parts) > 1 else '0'
    return datetime.strptime(f"{date_str} {time_base}.{time_frac}", "%Y-%m-%d %H:%M:%S.%f")


def calculate_delay(row: dict) -> float:
    """计算延迟秒数 (timestamp - time)"""
    ts_str = row['timestamp']
    time_str = row['time']

    ts = parse_timestamp(ts_str)
    date_str = ts_str.split(' ')[0]
    market_time = parse_time(time_str, date_str)

    return (ts - market_time).total_seconds()


def write_csv(filename: str, rows: list, fieldnames: list):
    """写入 CSV 文件"""
    with open(filename, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def print_statistics(delays: list, label: str):
    """打印延迟统计信息"""
    if not delays:
        print(f"\n{label}: 无数据")
        return

    avg_delay = sum(d[0] for d in delays) / len(delays)
    max_item = max(delays, key=lambda x: x[0])
    min_item = min(delays, key=lambda x: x[0])

    print(f"\n{label} ({len(delays)} 条):")
    print(f"  平均延迟: {avg_delay:.3f} 秒")
    print(f"  最大延迟: {max_item[0]:.3f} 秒 ({max_item[1]} @ {max_item[2]})")
    print(f"  最小延迟: {min_item[0]:.3f} 秒 ({min_item[1]} @ {min_item[2]})")


def main():
    parser = argparse.ArgumentParser(
        description='按延迟时间分流信号文件',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
    python script/split_signals_by_delay.py signals_20260123.csv
    python script/split_signals_by_delay.py signals_20260123.csv --threshold 3
        '''
    )
    parser.add_argument('input', help='输入 CSV 文件路径')
    parser.add_argument('--threshold', '-t', type=float, default=5.0,
                        help='延迟阈值(秒)，默认 5.0')
    parser.add_argument('--no-stats', action='store_true',
                        help='不显示统计信息')

    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        print(f"错误: 文件不存在: {args.input}")
        return 1

    threshold = args.threshold

    # 生成输出文件名
    stem = input_path.stem
    suffix = input_path.suffix
    parent = input_path.parent

    ge_file = parent / f"{stem}_delay_ge{int(threshold)}s{suffix}"
    lt_file = parent / f"{stem}_delay_lt{int(threshold)}s{suffix}"

    # 读取并分流
    ge_rows = []  # >= threshold
    lt_rows = []  # < threshold
    ge_delays = []
    lt_delays = []

    with open(input_path, 'r') as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames

        for row in reader:
            delay = calculate_delay(row)
            symbol = row.get('symbol', '')
            time_str = row.get('time', '')

            if delay >= threshold:
                ge_rows.append(row)
                ge_delays.append((delay, symbol, time_str))
            else:
                lt_rows.append(row)
                lt_delays.append((delay, symbol, time_str))

    # 写入文件
    write_csv(str(ge_file), ge_rows, fieldnames)
    write_csv(str(lt_file), lt_rows, fieldnames)

    print(f"输入文件: {input_path}")
    print(f"延迟阈值: {threshold} 秒")
    print(f"\n输出文件:")
    print(f"  {ge_file}: {len(ge_rows)} 行 (延迟 >= {threshold}s)")
    print(f"  {lt_file}: {len(lt_rows)} 行 (延迟 < {threshold}s)")

    # 统计信息
    if not args.no_stats:
        print_statistics(ge_delays, f"延迟 >= {threshold}s")
        print_statistics(lt_delays, f"延迟 < {threshold}s")

    return 0


if __name__ == '__main__':
    exit(main())
