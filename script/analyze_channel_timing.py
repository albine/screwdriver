#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
分析相同 ChannelNo 的数据时序关系
验证同一 channel 内的数据是否严格按序到达
"""

import subprocess
import sys
from datetime import datetime

SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine/data/raw"

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
    return {'record_count': record_count}

def analyze_order_timing(filepath, max_records=500000):
    """分析 Order 数据，检查同一 channel 内的时序"""

    # channel -> [(mdtime, applseqnum, symbol), ...]
    channel_data = defaultdict(list)

    if not os.path.exists(filepath):
        print("文件不存在: {}".format(filepath))
        return

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        count = min(max_records, header['record_count'])

        for i in range(count):
            offset = HEADER_SIZE + i * SIZE_ORDER
            mm.seek(offset)
            data = mm.read(SIZE_ORDER)

            sym = data[:40].rstrip(b'\\x00').decode('utf-8', errors='ignore')
            mdtime = struct.unpack('<i', data[44:48])[0]
            channelno = struct.unpack('<i', data[92:96])[0]
            applseqnum = struct.unpack('<q', data[112:120])[0]

            channel_data[channelno].append((mdtime, applseqnum, sym, i))

        mm.close()

    print("=" * 70)
    print("Order 数据时序分析 (前 {} 条记录)".format(count))
    print("=" * 70)

    # 分析每个 channel
    for ch in sorted(channel_data.keys()):
        records = channel_data[ch]
        if len(records) < 10:
            continue

        print("\\nChannelNo {}: {} 条记录".format(ch, len(records)))

        # 检查 applseqnum 是否严格递增
        seq_violations = 0
        time_violations = 0
        prev_seq = -1
        prev_time = -1

        for mdtime, seq, sym, idx in records:
            if prev_seq >= 0 and seq <= prev_seq:
                seq_violations += 1
            if prev_time >= 0 and mdtime < prev_time:
                time_violations += 1
            prev_seq = seq
            prev_time = mdtime

        print("  ApplSeqNum 非递增次数: {}".format(seq_violations))
        print("  MDTime 回退次数: {}".format(time_violations))

        # 打印该 channel 的前 5 条和后 5 条记录
        print("  前 5 条记录:")
        for mdtime, seq, sym, idx in records[:5]:
            print("    idx={:<8} time={:<10} seq={:<10} sym={}".format(idx, mdtime, seq, sym))

        if len(records) > 10:
            print("  ... (省略 {} 条) ...".format(len(records) - 10))
            print("  后 5 条记录:")
            for mdtime, seq, sym, idx in records[-5:]:
                print("    idx={:<8} time={:<10} seq={:<10} sym={}".format(idx, mdtime, seq, sym))

def analyze_transaction_timing(filepath, max_records=500000):
    """分析 Transaction 数据，检查同一 channel 内的时序"""

    channel_data = defaultdict(list)

    if not os.path.exists(filepath):
        print("文件不存在: {}".format(filepath))
        return

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        count = min(max_records, header['record_count'])

        for i in range(count):
            offset = HEADER_SIZE + i * SIZE_TRANSACTION
            mm.seek(offset)
            data = mm.read(SIZE_TRANSACTION)

            sym = data[:40].rstrip(b'\\x00').decode('utf-8', errors='ignore')
            mdtime = struct.unpack('<i', data[44:48])[0]
            applseqnum = struct.unpack('<q', data[112:120])[0]
            channelno = struct.unpack('<i', data[120:124])[0]

            channel_data[channelno].append((mdtime, applseqnum, sym, i))

        mm.close()

    print("\\n")
    print("=" * 70)
    print("Transaction 数据时序分析 (前 {} 条记录)".format(count))
    print("=" * 70)

    for ch in sorted(channel_data.keys()):
        records = channel_data[ch]
        if len(records) < 10:
            continue

        print("\\nChannelNo {}: {} 条记录".format(ch, len(records)))

        seq_violations = 0
        time_violations = 0
        prev_seq = -1
        prev_time = -1

        for mdtime, seq, sym, idx in records:
            if prev_seq >= 0 and seq <= prev_seq:
                seq_violations += 1
            if prev_time >= 0 and mdtime < prev_time:
                time_violations += 1
            prev_seq = seq
            prev_time = mdtime

        print("  ApplSeqNum 非递增次数: {}".format(seq_violations))
        print("  MDTime 回退次数: {}".format(time_violations))

        print("  前 5 条记录:")
        for mdtime, seq, sym, idx in records[:5]:
            print("    idx={:<8} time={:<10} seq={:<10} sym={}".format(idx, mdtime, seq, sym))

        if len(records) > 10:
            print("  ... (省略 {} 条) ...".format(len(records) - 10))
            print("  后 5 条记录:")
            for mdtime, seq, sym, idx in records[-5:]:
                print("    idx={:<8} time={:<10} seq={:<10} sym={}".format(idx, mdtime, seq, sym))

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python analyze.py <data_dir>")
        sys.exit(1)

    data_dir = sys.argv[1]

    orders_file = os.path.join(data_dir, 'orders.bin')
    analyze_order_timing(orders_file)

    txn_file = os.path.join(data_dir, 'transactions.bin')
    analyze_transaction_timing(txn_file)
'''


def get_date_path(date_str=None):
    if date_str:
        d = datetime.strptime(date_str, '%Y%m%d')
    else:
        d = datetime.now()
    return d.strftime('%Y/%m/%d')


def main():
    date_str = sys.argv[1] if len(sys.argv) > 1 else None
    date_path = get_date_path(date_str)
    remote_data_dir = f"{REMOTE_DATA_BASE}/{date_path}"

    print(f"分析 ChannelNo 时序关系")
    print(f"  数据目录: {remote_data_dir}")
    print()

    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.py', delete=False) as f:
        f.write(REMOTE_SCRIPT)
        local_script = f.name

    subprocess.run(f'scp {local_script} {SSH_HOST}:/tmp/analyze_timing.py',
                   shell=True, check=True, capture_output=True)

    import os
    os.unlink(local_script)

    result = subprocess.run(
        f'ssh {SSH_HOST} "python /tmp/analyze_timing.py {remote_data_dir}"',
        shell=True, capture_output=True, text=True
    )

    print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)

    subprocess.run(f'ssh {SSH_HOST} "rm -f /tmp/analyze_timing.py"',
                   shell=True, capture_output=True)


if __name__ == '__main__':
    main()
