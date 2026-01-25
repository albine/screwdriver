#!/usr/bin/env python3
"""
从服务器下载biz日志并解析信号行，输出到CSV

支持两种信号格式:
- [SIGNAL] BUY | 600403.SH | Time=09:33:14.460 | Price=70884(7.09元) | Reason=...
- [STRA] 600158.SH | BUY | MARKET_TIME=09:30:00.050 | Price=98750(9.88元) | Reason=...

特性:
- 自动下载同一日期的所有日志文件（live_biz_YYYYMMDD_*.log）
- 解析所有文件并按时间顺序合并输出

用法:
    python script/download_and_parse_signals.py [日期YYYYMMDD] [--local FILE ...]

示例:
    python script/download_and_parse_signals.py              # 默认当天，下载所有日志
    python script/download_and_parse_signals.py 20260122     # 指定日期，下载所有日志
    python script/download_and_parse_signals.py --local log1.log log2.log  # 处理多个本地文件
"""

import argparse
import csv
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional, Dict

# 服务器配置 (可通过命令行参数覆盖)
CONFIG = {
    "host": "market-m",  # SSH host (在 ~/.ssh/config 中配置)
    "log_dir": "/home/jiace/project/trading-engine.backup.20260123_111520/logs"
}


def download_logs(date_str: str, local_dir: str) -> list:
    """从服务器下载所有匹配的日志文件"""
    # 文件名格式: live_biz_20260122_*.log，需要先查找实际文件名
    remote_pattern = f"{CONFIG['log_dir']}/live_biz_{date_str}_*.log"

    print(f"正在从 {CONFIG['host']} 查找 {remote_pattern} ...")

    # 确保本地目录存在
    os.makedirs(local_dir, exist_ok=True)

    try:
        # 查找所有匹配的文件
        find_cmd = f"ls -t {remote_pattern} 2>/dev/null"
        result = subprocess.run(
            ["ssh", CONFIG['host'], find_cmd],
            capture_output=True,
            text=True
        )

        if result.returncode != 0 or not result.stdout.strip():
            print(f"未找到匹配的日志文件: {remote_pattern}")
            return []

        remote_files = [f.strip() for f in result.stdout.strip().split('\n') if f.strip()]
        print(f"找到 {len(remote_files)} 个日志文件")

        # 下载每个文件
        local_paths = []
        for remote_file in remote_files:
            filename = os.path.basename(remote_file)
            local_path = os.path.join(local_dir, filename)

            print(f"正在下载 {filename} ...")
            result = subprocess.run(
                ["scp", f"{CONFIG['host']}:{remote_file}", local_path],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                print(f"下载失败: {result.stderr}")
                continue

            local_paths.append(local_path)
            print(f"  -> {local_path}")

        print(f"共下载 {len(local_paths)} 个文件")
        return local_paths

    except Exception as e:
        print(f"下载出错: {e}")
        return []


def parse_signal_line(line: str) -> Optional[Dict]:
    """解析信号行，兼容 [SIGNAL] 和 [STRA] 两种格式"""
    # 检查是否为信号行
    is_signal = "[SIGNAL]" in line
    is_stra = "[STRA]" in line

    if not is_signal and not is_stra:
        return None

    # [STRA] 格式只处理 BUY 信号
    if is_stra and "BUY" not in line:
        return None

    # 提取日志时间戳 (行首)
    ts_match = re.match(r'^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)', line)
    if not ts_match:
        return None
    timestamp = ts_match.group(1)

    # 提取股票代码
    symbol = ""
    if is_stra:
        # 新格式: [STRA] 600158.SH | BUY | ...
        symbol_match = re.search(r'\[STRA\]\s+(\d{6}\.[A-Z]{2})\s+\|', line)
        if symbol_match:
            symbol = symbol_match.group(1)
    else:
        # 旧格式: [SIGNAL] BUY | 600158.SH | ...
        symbol_match = re.search(r'\[SIGNAL\] BUY \| (\d{6}\.[A-Z]{2}) \|', line)
        if symbol_match:
            symbol = symbol_match.group(1)

    # 提取Time字段 (兼容 Time= 和 MARKET_TIME=)
    time_val = ""
    time_match = re.search(r'(?:Time|MARKET_TIME)=(\d{2}:\d{2}:\d{2}\.\d+)', line)
    if time_match:
        time_val = time_match.group(1)

    # 提取Price字段 - 取元为单位的值
    price = ""
    price_match = re.search(r'Price=\d+\(([0-9.]+)元\)', line)
    if price_match:
        price = price_match.group(1)
    else:
        # 如果没有括号格式，尝试直接解析整数价格
        price_match = re.search(r'Price=(\d+)', line)
        if price_match:
            price_int = int(price_match.group(1))
            price = f"{price_int / 10000:.2f}"

    # 提取Reason字段
    reason = ""
    reason_match = re.search(r'Reason=(.+?)(?:\s*\||\s*$)', line)
    if reason_match:
        reason = reason_match.group(1).strip()
    else:
        # 没有Reason的信号（如Breakout策略），使用默认值
        reason = "手动添加突破"

    return {
        "timestamp": timestamp,
        "symbol": symbol,
        "time": time_val,
        "price": price,
        "reason": reason
    }


def extract_symbol_from_ordr(line: str) -> str:
    """从[ORDR]行提取股票代码"""
    # 格式: [ORDR] [LIVE] 600868.SH_Breakout | 600868.SH | BUY @ ...
    match = re.search(r'\| (\d{6}\.[A-Z]{2}) \|', line)
    if match:
        return match.group(1)
    return ""


def parse_single_log_file(log_path: str) -> list:
    """解析单个日志文件，返回信号列表"""
    signals = []

    lines = open(log_path, 'r', encoding='utf-8').readlines()

    for i, line in enumerate(lines):
        # 兼容 [SIGNAL] 和 [STRA] 两种格式
        if "[SIGNAL]" in line or "[STRA]" in line:
            parsed = parse_signal_line(line)
            if parsed:
                # 如果没有股票代码，尝试从下一行[ORDR]行提取
                if not parsed["symbol"] and i + 1 < len(lines):
                    next_line = lines[i + 1]
                    if "[ORDR]" in next_line:
                        parsed["symbol"] = extract_symbol_from_ordr(next_line)
                signals.append(parsed)

    return signals


def process_log_files(log_paths: list, output_path: str) -> int:
    """处理多个日志文件，合并输出CSV（按时间排序）"""
    all_signals = []

    # 解析每个日志文件
    for log_path in log_paths:
        print(f"正在解析 {os.path.basename(log_path)} ...")
        signals = parse_single_log_file(log_path)
        print(f"  -> {len(signals)} 条信号")
        all_signals.extend(signals)

    # 按时间戳排序
    all_signals.sort(key=lambda x: x["timestamp"])

    # 写入信号CSV
    with open(output_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=["timestamp", "symbol", "time", "price", "reason"])
        writer.writeheader()
        writer.writerows(all_signals)

    # 写入去重的股票代码文件（全部）
    symbols_path = output_path.replace('.csv', '_symbols.txt')
    unique_symbols = []
    seen = set()
    for s in all_signals:
        if s["symbol"] and s["symbol"] not in seen:
            seen.add(s["symbol"])
            unique_symbols.append(s["symbol"])

    with open(symbols_path, 'w', encoding='utf-8') as f:
        for sym in unique_symbols:
            f.write(sym + '\n')

    print(f"股票代码文件: {symbols_path} ({len(unique_symbols)} 只)")

    # 写入去重的股票代码文件（排除"手动添加突破"）
    filtered_path = output_path.replace('.csv', '_symbols_filtered.txt')
    filtered_symbols = []
    seen_filtered = set()
    for s in all_signals:
        if s["symbol"] and s["reason"] != "手动添加突破" and s["symbol"] not in seen_filtered:
            seen_filtered.add(s["symbol"])
            filtered_symbols.append(s["symbol"])

    with open(filtered_path, 'w', encoding='utf-8') as f:
        for sym in filtered_symbols:
            f.write(sym + '\n')

    print(f"过滤后股票代码: {filtered_path} ({len(filtered_symbols)} 只)")

    return len(all_signals)


def main():
    parser = argparse.ArgumentParser(description="下载并解析biz日志中的信号行（支持[SIGNAL]和[STRA]格式）")
    parser.add_argument("date", nargs="?", help="日期 YYYYMMDD (默认当天)")
    parser.add_argument("--local", "-l", nargs="+", help="使用本地日志文件 (跳过下载)，支持多个文件")
    parser.add_argument("--output", "-o", help="输出CSV文件路径")
    parser.add_argument("--host", help=f"远程主机 (默认: {CONFIG['host']})")
    parser.add_argument("--remote-dir", help=f"远程日志目录 (默认: {CONFIG['log_dir']})")

    args = parser.parse_args()

    # 更新配置
    if args.host:
        CONFIG['host'] = args.host
    if args.remote_dir:
        CONFIG['log_dir'] = args.remote_dir

    # 确定日志文件路径
    if args.local:
        # 本地模式：使用指定的本地文件
        log_paths = args.local
        date_match = re.search(r'(\d{8})', log_paths[0])
        date_str = date_match.group(1) if date_match else datetime.now().strftime("%Y%m%d")

        # 检查文件存在
        for log_path in log_paths:
            if not os.path.exists(log_path):
                print(f"错误: 日志文件不存在: {log_path}")
                sys.exit(1)
    else:
        # 下载模式：从服务器下载所有匹配的日志文件
        date_str = args.date if args.date else datetime.now().strftime("%Y%m%d")
        local_dir = f"/tmp/biz_logs_{date_str}"

        print(f"日期: {date_str}")

        # 下载所有日志文件
        log_paths = download_logs(date_str, local_dir)
        if not log_paths:
            print("未能下载任何日志文件")
            sys.exit(1)

    # 确定输出路径
    if args.output:
        output_path = args.output
    else:
        output_path = f"signals_{date_str}.csv"

    # 处理日志（支持多个文件）
    print(f"\n正在处理 {len(log_paths)} 个日志文件...")
    count = process_log_files(log_paths, output_path)

    print(f"\n完成! 共解析 {count} 条信号，输出到 {output_path}")


if __name__ == "__main__":
    main()
