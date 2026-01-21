#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
统计 mmap 数据中不同的 ChannelNo

用法:
    python script/analyze_channel_no.py [date]

示例:
    python script/analyze_channel_no.py          # 今天
    python script/analyze_channel_no.py 20260120 # 指定日期
"""

import subprocess
import sys
from datetime import datetime

SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine/data/raw"

# 远程统计脚本
REMOTE_SCRIPT = '''#!/usr/bin/env python
# -*- coding: utf-8 -*-
from __future__ import print_function
import struct
import mmap
import os
import sys
from collections import defaultdict

HEADER_SIZE = 64
SIZE_ORDER = 144
SIZE_TRANSACTION = 128

def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {'record_count': record_count, 'struct_size': struct_size}

def analyze_orders(filepath):
    """分析 orders 文件，返回 {symbol: first_channel_no}"""
    results = {}
    channel_counts = defaultdict(int)

    if not os.path.exists(filepath):
        return results, channel_counts

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        for i in range(header['record_count']):
            offset = HEADER_SIZE + i * SIZE_ORDER
            mm.seek(offset)
            data = mm.read(SIZE_ORDER)

            sym = data[:40].rstrip(b'\\x00').decode('utf-8', errors='ignore')
            if not sym:
                continue

            channelno = struct.unpack('<i', data[104:108])[0]

            # 统计每个 channel 的记录数
            channel_counts[channelno] += 1

            # 记录每只股票的第一条记录的 channel
            if sym not in results:
                results[sym] = channelno

        mm.close()

    return results, channel_counts

def analyze_transactions(filepath):
    """分析 transactions 文件，返回 {symbol: first_channel_no}"""
    results = {}
    channel_counts = defaultdict(int)

    if not os.path.exists(filepath):
        return results, channel_counts

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        for i in range(header['record_count']):
            offset = HEADER_SIZE + i * SIZE_TRANSACTION
            mm.seek(offset)
            data = mm.read(SIZE_TRANSACTION)

            sym = data[:40].rstrip(b'\\x00').decode('utf-8', errors='ignore')
            if not sym:
                continue

            channelno = struct.unpack('<i', data[120:124])[0]

            # 统计每个 channel 的记录数
            channel_counts[channelno] += 1

            # 记录每只股票的第一条记录的 channel
            if sym not in results:
                results[sym] = channelno

        mm.close()

    return results, channel_counts

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python analyze.py <data_dir>")
        sys.exit(1)

    data_dir = sys.argv[1]

    print("=" * 60)
    print("Orders 分析")
    print("=" * 60)

    orders_file = os.path.join(data_dir, 'orders.bin')
    order_results, order_channel_counts = analyze_orders(orders_file)

    if order_results:
        # 统计每只股票第一条记录的 channel 分布
        first_channel_dist = defaultdict(list)
        for sym, ch in order_results.items():
            first_channel_dist[ch].append(sym)

        print("\\n每只股票第一条 Order 的 ChannelNo 分布:")
        for ch in sorted(first_channel_dist.keys()):
            symbols = first_channel_dist[ch]
            print("  ChannelNo {}: {} 只股票".format(ch, len(symbols)))
            if len(symbols) <= 10:
                print("    {}".format(", ".join(sorted(symbols))))
            else:
                print("    {} ...".format(", ".join(sorted(symbols)[:10])))

        print("\\n所有 Order 记录的 ChannelNo 统计:")
        for ch in sorted(order_channel_counts.keys()):
            print("  ChannelNo {}: {} 条记录".format(ch, order_channel_counts[ch]))

        print("\\nOrder 唯一 ChannelNo 数量: {}".format(len(order_channel_counts)))
    else:
        print("无 orders 数据")

    print()
    print("=" * 60)
    print("Transactions 分析")
    print("=" * 60)

    txn_file = os.path.join(data_dir, 'transactions.bin')
    txn_results, txn_channel_counts = analyze_transactions(txn_file)

    if txn_results:
        # 统计每只股票第一条记录的 channel 分布
        first_channel_dist = defaultdict(list)
        for sym, ch in txn_results.items():
            first_channel_dist[ch].append(sym)

        print("\\n每只股票第一条 Transaction 的 ChannelNo 分布:")
        for ch in sorted(first_channel_dist.keys()):
            symbols = first_channel_dist[ch]
            print("  ChannelNo {}: {} 只股票".format(ch, len(symbols)))
            if len(symbols) <= 10:
                print("    {}".format(", ".join(sorted(symbols))))
            else:
                print("    {} ...".format(", ".join(sorted(symbols)[:10])))

        print("\\n所有 Transaction 记录的 ChannelNo 统计:")
        for ch in sorted(txn_channel_counts.keys()):
            print("  ChannelNo {}: {} 条记录".format(ch, txn_channel_counts[ch]))

        print("\\nTransaction 唯一 ChannelNo 数量: {}".format(len(txn_channel_counts)))
    else:
        print("无 transactions 数据")
'''


def get_date_path(date_str=None):
    """获取日期路径 YYYY/MM/DD"""
    if date_str:
        d = datetime.strptime(date_str, '%Y%m%d')
    else:
        d = datetime.now()
    return d.strftime('%Y/%m/%d')


def main():
    date_str = sys.argv[1] if len(sys.argv) > 1 else None
    date_path = get_date_path(date_str)
    remote_data_dir = f"{REMOTE_DATA_BASE}/{date_path}"

    print(f"分析 ChannelNo 分布")
    print(f"  服务器: {SSH_HOST}")
    print(f"  数据目录: {remote_data_dir}")
    print()

    # 1. 上传分析脚本
    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.py', delete=False) as f:
        f.write(REMOTE_SCRIPT)
        local_script = f.name

    subprocess.run(f'scp {local_script} {SSH_HOST}:/tmp/analyze_channel.py',
                   shell=True, check=True, capture_output=True)

    import os
    os.unlink(local_script)

    # 2. 运行分析脚本
    result = subprocess.run(
        f'ssh {SSH_HOST} "python /tmp/analyze_channel.py {remote_data_dir}"',
        shell=True, capture_output=True, text=True
    )

    print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)

    # 3. 清理
    subprocess.run(f'ssh {SSH_HOST} "rm -f /tmp/analyze_channel.py"',
                   shell=True, capture_output=True)


if __name__ == '__main__':
    main()
