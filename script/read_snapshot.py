#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从服务器读取 snapshots.bin 中的订单簿快照

用法:
    python script/read_snapshot.py <symbol> [applseqnum] [date]

示例:
    python script/read_snapshot.py 600759.SH              # 列出所有快照
    python script/read_snapshot.py 600759.SH 4621748      # 查找指定 applseqnum 附近的快照
    python script/read_snapshot.py 600759.SH 4621748 20260120
"""

import os
import sys
import subprocess
import tempfile

SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine.backup.20260120_131843/data/raw"

# 远程读取脚本
REMOTE_SCRIPT = '''#!/usr/bin/env python
# -*- coding: utf-8 -*-
from __future__ import print_function
import sys
import struct
import mmap
import os

HEADER_SIZE = 64
SIZE_SNAPSHOT = 728
SIZE_ENTRY = 24  # MDEntryDetailStruct

def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {'record_count': record_count}

def format_time(mdtime):
    h = mdtime // 10000000
    m = (mdtime // 100000) % 100
    s = (mdtime // 1000) % 100
    ms = mdtime % 1000
    return "%02d:%02d:%02d.%03d" % (h, m, s, ms)

def read_entry(data, offset):
    """读取 MDEntryDetailStruct"""
    level = struct.unpack('<i', data[offset:offset+4])[0]
    price = struct.unpack('<q', data[offset+8:offset+16])[0]
    totalqty = struct.unpack('<i', data[offset+16:offset+20])[0]
    numorders = struct.unpack('<i', data[offset+20:offset+24])[0]
    return {'level': level, 'price': price, 'qty': totalqty, 'orders': numorders}

def read_snapshot(mm, idx):
    """读取单个快照记录"""
    offset = HEADER_SIZE + idx * SIZE_SNAPSHOT
    mm.seek(offset)
    data = mm.read(SIZE_SNAPSHOT)

    sym = data[:40].rstrip(b'\\x00')
    mddate, mdtime = struct.unpack('<ii', data[40:48])
    applseqnum = struct.unpack('<q', data[76:84])[0]
    lastpx = struct.unpack('<q', data[116:124])[0]

    # 读取买卖档位
    buyentries = []
    sellentries = []
    for i in range(10):
        buy_entry = read_entry(data, 240 + i * SIZE_ENTRY)
        sell_entry = read_entry(data, 480 + i * SIZE_ENTRY)
        if buy_entry['price'] > 0:
            buyentries.append(buy_entry)
        if sell_entry['price'] > 0:
            sellentries.append(sell_entry)

    buycount = struct.unpack('<i', data[720:724])[0]
    sellcount = struct.unpack('<i', data[724:728])[0]

    return {
        'symbol': sym.decode('utf-8') if isinstance(sym, bytes) else sym,
        'date': mddate,
        'time': mdtime,
        'applseqnum': applseqnum,
        'lastpx': lastpx,
        'buyentries': buyentries[:buycount] if buycount > 0 else buyentries,
        'sellentries': sellentries[:sellcount] if sellcount > 0 else sellentries,
    }

def main():
    if len(sys.argv) < 3:
        print("Usage: python read_snapshot_remote.py <data_dir> <symbol> [target_applseqnum]")
        sys.exit(1)

    data_dir = sys.argv[1]
    symbol = sys.argv[2]
    target_seq = int(sys.argv[3]) if len(sys.argv) > 3 else None

    symbol_base = symbol.split('.')[0]
    filepath = os.path.join(data_dir, 'snapshots.bin')

    if not os.path.exists(filepath):
        print("File not found: %s" % filepath)
        sys.exit(1)

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        print("Total records: %d" % header['record_count'])

        found_snapshots = []

        for i in range(header['record_count']):
            offset = HEADER_SIZE + i * SIZE_SNAPSHOT
            mm.seek(offset)
            sym_bytes = mm.read(40).rstrip(b'\\x00')
            sym_str = sym_bytes.decode('utf-8') if isinstance(sym_bytes, bytes) else sym_bytes

            if symbol_base not in sym_str:
                continue

            snap = read_snapshot(mm, i)
            found_snapshots.append(snap)

        mm.close()

    print("Found %d snapshots for %s" % (len(found_snapshots), symbol))

    if target_seq:
        # 找最接近的快照
        closest = None
        min_diff = float('inf')
        for snap in found_snapshots:
            diff = abs(snap['applseqnum'] - target_seq)
            if diff < min_diff:
                min_diff = diff
                closest = snap
            # 也找第一个 >= target_seq 的
            if snap['applseqnum'] >= target_seq and (closest is None or snap['applseqnum'] < closest['applseqnum']):
                closest = snap
                break

        if closest:
            print("")
            print("=== Closest snapshot to applseqnum %d ===" % target_seq)
            print("ApplSeqNum: %d (diff: %d)" % (closest['applseqnum'], closest['applseqnum'] - target_seq))
            print("Time: %s" % format_time(closest['time']))
            print("LastPx: %.2f" % (closest['lastpx'] / 10000.0))
            print("")
            print("--- BUY SIDE ---")
            for i, e in enumerate(closest['buyentries']):
                print("  Buy%d: Price=%d (%.2f) Qty=%d" % (i+1, e['price'], e['price']/10000.0, e['qty']))
            print("")
            print("--- SELL SIDE ---")
            for i, e in enumerate(closest['sellentries']):
                print("  Sell%d: Price=%d (%.2f) Qty=%d" % (i+1, e['price'], e['price']/10000.0, e['qty']))
    else:
        # 列出前10个和后10个
        print("")
        print("First 5 snapshots:")
        for snap in found_snapshots[:5]:
            print("  ApplSeqNum=%d Time=%s" % (snap['applseqnum'], format_time(snap['time'])))
        if len(found_snapshots) > 10:
            print("...")
            print("Last 5 snapshots:")
            for snap in found_snapshots[-5:]:
                print("  ApplSeqNum=%d Time=%s" % (snap['applseqnum'], format_time(snap['time'])))

if __name__ == '__main__':
    main()
'''

def get_date_path(date_str=None):
    from datetime import datetime
    if date_str:
        d = datetime.strptime(date_str, '%Y%m%d')
    else:
        d = datetime.now()
    return d.strftime('%Y/%m/%d')

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    symbol = sys.argv[1]
    target_seq = sys.argv[2] if len(sys.argv) > 2 else None
    date_str = sys.argv[3] if len(sys.argv) > 3 else None

    if '.' not in symbol:
        symbol = f"{symbol}.SH" if symbol.startswith('6') else f"{symbol}.SZ"

    date_path = get_date_path(date_str)
    remote_data_dir = f"{REMOTE_DATA_BASE}/{date_path}"

    print(f"查询 {symbol} 的订单簿快照")
    print(f"  数据目录: {remote_data_dir}")
    print()

    # 上传脚本
    with tempfile.NamedTemporaryFile(mode='w', suffix='.py', delete=False) as f:
        f.write(REMOTE_SCRIPT)
        local_script = f.name

    subprocess.run(f'scp {local_script} {SSH_HOST}:/tmp/read_snapshot_remote.py',
                   shell=True, capture_output=True)
    os.unlink(local_script)

    # 运行远程脚本
    cmd = f'ssh {SSH_HOST} "python /tmp/read_snapshot_remote.py {remote_data_dir} {symbol}'
    if target_seq:
        cmd += f' {target_seq}'
    cmd += '"'

    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    print(result.stdout)
    if result.stderr:
        print("Error:", result.stderr)

    # 清理
    subprocess.run(f'ssh {SSH_HOST} "rm -f /tmp/read_snapshot_remote.py"',
                   shell=True, capture_output=True)

if __name__ == '__main__':
    main()
