#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 Order 结构中 ChannelNo 的正确偏移量
"""

import subprocess
import sys
from datetime import datetime

SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine/data/raw"

# 远程验证脚本
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

def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {'record_count': record_count, 'struct_size': struct_size}

def verify_order_offsets(filepath, max_records=10):
    """读取前几条记录，打印所有可能的 int32 值来找到 ChannelNo"""

    if not os.path.exists(filepath):
        print("文件不存在: {}".format(filepath))
        return

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        print("文件记录数: {}".format(header['record_count']))
        print("结构大小: {} bytes".format(header['struct_size']))
        print()

        count = min(max_records, header['record_count'])

        for i in range(count):
            offset = HEADER_SIZE + i * SIZE_ORDER
            mm.seek(offset)
            data = mm.read(SIZE_ORDER)

            sym = data[:40].rstrip(b'\\x00').decode('utf-8', errors='ignore')
            mddate = struct.unpack('<i', data[40:44])[0]
            mdtime = struct.unpack('<i', data[44:48])[0]

            print("=" * 60)
            print("Record {}: symbol={}, date={}, time={}".format(i, sym, mddate, mdtime))
            print("-" * 60)

            # 根据 MDOrderStruct 定义打印字段
            print("偏移 48-52  securityidsource:  {}".format(struct.unpack('<i', data[48:52])[0]))
            print("偏移 52-56  securitytype:      {}".format(struct.unpack('<i', data[52:56])[0]))
            print("偏移 56-64  orderindex:        {}".format(struct.unpack('<q', data[56:64])[0]))
            print("偏移 64-68  ordertype:         {}".format(struct.unpack('<i', data[64:68])[0]))
            print("偏移 68-72  [padding]:         {}".format(struct.unpack('<i', data[68:72])[0]))
            print("偏移 72-80  orderprice:        {}".format(struct.unpack('<q', data[72:80])[0]))
            print("偏移 80-88  orderqty:          {}".format(struct.unpack('<q', data[80:88])[0]))
            print("偏移 88-92  orderbsflag:       {}".format(struct.unpack('<i', data[88:92])[0]))
            print("偏移 92-96  channelno (正确):  {}".format(struct.unpack('<i', data[92:96])[0]))
            print("偏移 96-104 orderno:           {}".format(struct.unpack('<q', data[96:104])[0]))
            print("偏移 104-112 tradedqty:        {}".format(struct.unpack('<q', data[104:112])[0]))
            print("偏移 112-120 applseqnum:       {}".format(struct.unpack('<q', data[112:120])[0]))
            print("偏移 120-124 datamultipowerof10: {}".format(struct.unpack('<i', data[120:124])[0]))
            print("偏移 124-140 securitystatus:   {}".format(data[124:140].rstrip(b'\\x00')))
            print()

        mm.close()

def analyze_orders_correct_offset(filepath):
    """使用正确的偏移量 (92) 统计 ChannelNo"""

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

            # 正确的偏移: 92
            channelno = struct.unpack('<i', data[92:96])[0]

            channel_counts[channelno] += 1

            if sym not in results:
                results[sym] = channelno

        mm.close()

    return results, channel_counts

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python verify.py <data_dir>")
        sys.exit(1)

    data_dir = sys.argv[1]
    orders_file = os.path.join(data_dir, 'orders.bin')

    print("=" * 60)
    print("1. 验证前 5 条 Order 记录的字段值")
    print("=" * 60)
    verify_order_offsets(orders_file, 5)

    print()
    print("=" * 60)
    print("2. 使用正确偏移 (92) 统计 ChannelNo")
    print("=" * 60)

    order_results, order_channel_counts = analyze_orders_correct_offset(orders_file)

    if order_results:
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

    print(f"验证 Order ChannelNo 偏移量")
    print(f"  数据目录: {remote_data_dir}")
    print()

    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.py', delete=False) as f:
        f.write(REMOTE_SCRIPT)
        local_script = f.name

    subprocess.run(f'scp {local_script} {SSH_HOST}:/tmp/verify_offset.py',
                   shell=True, check=True, capture_output=True)

    import os
    os.unlink(local_script)

    result = subprocess.run(
        f'ssh {SSH_HOST} "python /tmp/verify_offset.py {remote_data_dir}"',
        shell=True, capture_output=True, text=True
    )

    print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)

    subprocess.run(f'ssh {SSH_HOST} "rm -f /tmp/verify_offset.py"',
                   shell=True, capture_output=True)


if __name__ == '__main__':
    main()
