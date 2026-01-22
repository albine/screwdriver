#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
批量下载市场数据 - 使用服务器端批量导出加速

加速原理：
  原来：每个股票遍历一次 mmap 文件（159GB × N 次）
  现在：所有股票一次遍历（159GB × 1 次）

  115 个股票：~10 小时 -> ~15 分钟

用法:
    python script/batch_download_data.py <symbols_file> [--date YYYYMMDD] [--end-time HHMMSS]

示例:
    python script/batch_download_data.py signals_20260122_symbols_filtered.txt
    python script/batch_download_data.py signals_20260122_symbols_filtered.txt --date 20260122
    python script/batch_download_data.py signals_20260122_symbols_filtered.txt --end-time 094500
"""

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent.absolute()

# SSH 配置
SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine/data/raw"
LOCAL_TEST_DATA = PROJECT_ROOT / "test_data"


def read_symbols(filepath: str) -> list:
    """读取股票代码列表文件"""
    symbols = []
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            # 支持每行只有代码，或者逗号分隔格式
            symbol = line.split(',')[0].strip()
            if symbol:
                symbols.append(symbol)
    return symbols


def run_cmd(cmd, check=True, timeout=None, stream_output=False):
    """运行命令并返回结果"""
    print(f"  > {cmd}")

    if stream_output:
        # 实时显示输出（用于长时间运行的命令）
        process = subprocess.Popen(
            cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1
        )
        output_lines = []
        try:
            for line in process.stdout:
                print(f"    {line.rstrip()}")
                output_lines.append(line)
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            print("Error: Command timed out")
            return None

        if check and process.returncode != 0:
            print(f"Error: Command failed with code {process.returncode}")
            return None

        # 返回一个模拟的 result 对象
        class Result:
            def __init__(self, returncode, stdout):
                self.returncode = returncode
                self.stdout = stdout
                self.stderr = ""
        return Result(process.returncode, ''.join(output_lines))
    else:
        result = subprocess.run(
            cmd, shell=True, capture_output=True, text=True, timeout=timeout
        )
        if check and result.returncode != 0:
            print(f"Error: {result.stderr}")
            return None
        return result


def get_date_path(date_str=None):
    """获取日期路径 YYYY/MM/DD"""
    if date_str:
        d = datetime.strptime(date_str, '%Y%m%d')
    else:
        d = datetime.now()
    return d.strftime('%Y/%m/%d')


def batch_download(symbols_file: str, date: str = None, end_time: str = '094500'):
    """批量下载：使用服务器端批量导出"""

    # 读取股票列表
    symbols = read_symbols(symbols_file)
    if not symbols:
        print("错误: 没有找到有效的股票代码")
        return False

    date_path = get_date_path(date)
    remote_data_dir = f"{REMOTE_DATA_BASE}/{date_path}"

    print(f"批量下载市场数据（优化模式）")
    print(f"  股票数量: {len(symbols)}")
    print(f"  日期: {date or '今天'}")
    print(f"  截止时间: {end_time}")
    print(f"  服务器: {SSH_HOST}")
    print(f"  数据目录: {remote_data_dir}")
    print()

    start_time = time.time()

    # 1. 检查远程数据目录是否存在
    print("1. 检查远程数据目录...")
    result = run_cmd(f'ssh {SSH_HOST} "ls -la {remote_data_dir}/*.bin 2>/dev/null || echo NO_DATA"', check=False)
    if result is None or 'NO_DATA' in result.stdout:
        print(f"错误: 远程目录 {remote_data_dir} 不存在或没有数据文件")
        return False
    print("  ✓ 数据目录存在")

    # 2. 上传股票列表到服务器
    print("2. 上传股票列表到服务器...")
    remote_symbols_file = "/tmp/batch_symbols.txt"
    result = run_cmd(f'scp {symbols_file} {SSH_HOST}:{remote_symbols_file}')
    if result is None:
        return False
    print("  ✓ 股票列表已上传")

    # 3. 上传批量导出脚本到服务器
    print("3. 上传批量导出脚本到服务器...")
    local_script = PROJECT_ROOT / "script" / "batch_export_server.py"
    remote_script = "/tmp/batch_export_server.py"
    result = run_cmd(f'scp {local_script} {SSH_HOST}:{remote_script}')
    if result is None:
        return False
    print("  ✓ 导出脚本已上传")

    # 4. 在服务器上运行批量导出
    print("4. 在服务器上运行批量导出...")
    remote_output_dir = "/tmp/batch_export_output"
    # 使用 -t 强制分配 tty，确保输出不被缓冲
    export_cmd = f'ssh -t {SSH_HOST} "rm -rf {remote_output_dir} && mkdir -p {remote_output_dir} && python -u {remote_script} {remote_data_dir} {remote_symbols_file} {remote_output_dir}'
    if end_time:
        export_cmd += f' {end_time}'
    export_cmd += '"'

    # 批量导出可能需要较长时间，设置超时为 2 小时，并实时显示输出
    result = run_cmd(export_cmd, timeout=7200, stream_output=True)
    if result is None:
        return False
    print("  ✓ 批量导出完成")

    # 5. 服务器端打包压缩
    print("5. 服务器端打包压缩...")
    remote_tar = "/tmp/batch_export.tar.gz"
    result = run_cmd(f'ssh {SSH_HOST} "cd {remote_output_dir} && tar czf {remote_tar} *.csv"')
    if result is None:
        return False

    # 获取压缩包大小
    result = run_cmd(f'ssh {SSH_HOST} "ls -lh {remote_tar}"', check=False)
    if result:
        print(f"  压缩包大小: {result.stdout.split()[4]}")
    print("  ✓ 打包完成")

    # 6. 下载压缩包
    print("6. 下载压缩包...")
    local_tar = PROJECT_ROOT / "batch_export.tar.gz"
    result = run_cmd(f'scp {SSH_HOST}:{remote_tar} {local_tar}')
    if result is None:
        return False
    print("  ✓ 下载完成")

    # 7. 本地解压
    print("7. 本地解压...")
    LOCAL_TEST_DATA.mkdir(parents=True, exist_ok=True)
    result = run_cmd(f'tar xzf {local_tar} -C {LOCAL_TEST_DATA}')
    if result is None:
        return False

    # 删除本地压缩包
    local_tar.unlink()
    print("  ✓ 解压完成")

    # 8. 清理服务器临时文件
    print("8. 清理服务器临时文件...")
    run_cmd(f'ssh {SSH_HOST} "rm -rf {remote_output_dir} {remote_tar} {remote_symbols_file} {remote_script}"', check=False)
    print("  ✓ 清理完成")

    elapsed = time.time() - start_time

    # 9. 统计下载结果
    print()
    print("=" * 60)
    print(f"批量下载完成！")
    print(f"  耗时: {elapsed:.1f} 秒 ({elapsed/60:.1f} 分钟)")
    print(f"  数据目录: {LOCAL_TEST_DATA}")
    print()

    # 统计每种文件
    order_files = list(LOCAL_TEST_DATA.glob("MD_ORDER_*.csv"))
    txn_files = list(LOCAL_TEST_DATA.glob("MD_TRANSACTION_*.csv"))
    tick_files = list(LOCAL_TEST_DATA.glob("MD_TICK_*.csv"))

    print(f"  ORDER 文件: {len(order_files)} 个")
    print(f"  TRANSACTION 文件: {len(txn_files)} 个")
    print(f"  TICK 文件: {len(tick_files)} 个")
    print("=" * 60)

    return True


def main():
    parser = argparse.ArgumentParser(description='批量下载市场数据（优化模式）')
    parser.add_argument('symbols_file', help='股票代码列表文件')
    parser.add_argument('--date', help='日期 (YYYYMMDD格式，默认今天)')
    parser.add_argument('--end-time', dest='end_time', default='094500',
                        help='截止时间 HHMMSS (默认 094500，即9:45)')

    args = parser.parse_args()

    # 检查文件是否存在
    if not os.path.exists(args.symbols_file):
        print(f"错误: 文件不存在 {args.symbols_file}")
        sys.exit(1)

    success = batch_download(args.symbols_file, args.date, args.end_time)

    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
