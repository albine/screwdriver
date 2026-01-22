#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读取 mmap 持久化文件 (V2 格式，带 local_recv_timestamp)

V2 格式特点:
- 使用对齐结构体 (8字节对齐)
- 新增 local_recv_timestamp 字段 (纳秒)
- 使用新 Magic 区分: ORD2, TXN2, TIK2, OBK2

用法:
    python read_mmap_v2.py <data_dir> <symbol> [tick|order|txn|snap] [page] [page_size]
    python read_mmap_v2.py <data_dir> --tail [tick|order|txn|snap] [count]
    python read_mmap_v2.py <data_dir> --info  # 显示文件信息
"""

import sys
import struct
import mmap
import os
from datetime import datetime

# ============================================================================
# Magic 定义 - V1 和 V2 格式
# ============================================================================
# V1 Magic (旧格式)
MAGIC_ORDER_V1 = 0x4D444F52       # "MDOR"
MAGIC_TRANSACTION_V1 = 0x4D445458 # "MDTX"
MAGIC_TICK_V1 = 0x4D44544B        # "MDTK"
MAGIC_SNAPSHOT_V1 = 0x4D444F42    # "MDOB"

# V2 Magic (新格式，带 local_recv_timestamp)
MAGIC_ORDER_V2 = 0x4F524432       # "ORD2"
MAGIC_TRANSACTION_V2 = 0x54584E32 # "TXN2"
MAGIC_TICK_V2 = 0x54494B32        # "TIK2"
MAGIC_ORDERBOOK_V2 = 0x4F424B32   # "OBK2"

# ============================================================================
# 结构体大小
# ============================================================================
# V1 大小
SIZE_ORDER_V1 = 144
SIZE_TRANSACTION_V1 = 128
SIZE_TICK_V1 = 2216
SIZE_SNAPSHOT_V1 = 728

# V2 大小 (对齐优化后)
SIZE_ORDER_V2 = 144
SIZE_TRANSACTION_V2 = 136
SIZE_TICK_V2 = 2216
SIZE_SNAPSHOT_V2 = 736

# Header 大小
HEADER_SIZE = 64


def magic_to_str(magic):
    """将 magic 转换为字符串"""
    chars = []
    for i in range(4):
        c = (magic >> (i * 8)) & 0xFF
        if 32 <= c <= 126:
            chars.append(chr(c))
        else:
            chars.append('.')
    return ''.join(chars)


def detect_version(magic):
    """检测文件版本"""
    v1_magics = {MAGIC_ORDER_V1, MAGIC_TRANSACTION_V1, MAGIC_TICK_V1, MAGIC_SNAPSHOT_V1}
    v2_magics = {MAGIC_ORDER_V2, MAGIC_TRANSACTION_V2, MAGIC_TICK_V2, MAGIC_ORDERBOOK_V2}

    if magic in v1_magics:
        return 1
    elif magic in v2_magics:
        return 2
    return 0


def read_header(mm):
    """读取 mmap 文件头"""
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {
        'magic': magic,
        'version': version,
        'struct_size': struct_size,
        'record_count': record_count,
        'write_offset': write_offset,
        'format_version': detect_version(magic),
    }


def format_time(mdtime):
    """格式化时间 HHMMSSMMM -> HH:MM:SS.mmm"""
    h = mdtime // 10000000
    m = (mdtime // 100000) % 100
    s = (mdtime // 1000) % 100
    ms = mdtime % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"


def format_price(price):
    """格式化价格 (价格 * 10000)"""
    return f"{price / 10000.0:.2f}"


def format_ns_timestamp(ns):
    """格式化纳秒时间戳"""
    if ns == 0:
        return "N/A"
    ts_sec = ns // 1_000_000_000
    ts_ms = (ns % 1_000_000_000) // 1_000_000
    dt = datetime.fromtimestamp(ts_sec)
    return dt.strftime("%H:%M:%S") + f".{ts_ms:03d}"


def calc_latency_ms(local_recv_ns, datatimestamp_ms):
    """计算延迟 (毫秒)

    local_recv_ns: 本地接收时间 (纳秒)
    datatimestamp_ms: 交易所时间戳 (毫秒)
    """
    if local_recv_ns == 0 or datatimestamp_ms == 0:
        return None
    local_ms = local_recv_ns // 1_000_000
    return local_ms - datatimestamp_ms


# ============================================================================
# V2 记录读取函数
# ============================================================================

def read_tick_v2(mm, offset):
    """读取 V2 tick 记录

    MDStockStruct V2 布局:
      0-7:    local_recv_timestamp (int64)
      8-15:   datatimestamp (int64)
      16-23:  maxpx (int64)
      24-31:  minpx (int64)
      32-39:  preclosepx (int64)
      40-47:  numtrades (int64)
      48-55:  totalvolumetrade (int64)
      56-63:  totalvaluetrade (int64)
      64-71:  lastpx (int64)
      72-79:  openpx (int64)
      80-87:  closepx (int64)
      88-95:  highpx (int64)
      96-103: lowpx (int64)
      ...
      2120-2123: mddate (int32)
      2124-2127: mdtime (int32)
      ...
      2168-2207: htscsecurityid[40]
      2208:   tradingphasecode (char)
    """
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TICK_V2)

    local_recv_ts = struct.unpack('<q', data[0:8])[0]
    datatimestamp = struct.unpack('<q', data[8:16])[0]
    maxpx = struct.unpack('<q', data[16:24])[0]
    minpx = struct.unpack('<q', data[24:32])[0]
    preclosepx = struct.unpack('<q', data[32:40])[0]
    lastpx = struct.unpack('<q', data[64:72])[0]

    mddate = struct.unpack('<i', data[2120:2124])[0]
    mdtime = struct.unpack('<i', data[2124:2128])[0]

    symbol = data[2168:2208].rstrip(b'\x00')

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': local_recv_ts,
        'datatimestamp': datatimestamp,
        'lastpx': lastpx,
        'maxpx': maxpx,
        'minpx': minpx,
        'preclosepx': preclosepx,
    }


def read_order_v2(mm, offset):
    """读取 V2 order 记录

    MDOrderStruct V2 布局:
      0-7:    local_recv_timestamp (int64)
      8-15:   orderindex (int64)
      16-23:  orderprice (int64)
      24-31:  orderqty (int64)
      32-39:  orderno (int64)
      40-47:  tradedqty (int64)
      48-55:  applseqnum (int64)
      56-59:  mddate (int32)
      60-63:  mdtime (int32)
      64-67:  securityidsource (int32)
      68-71:  securitytype (int32)
      72-75:  ordertype (int32)
      76-79:  orderbsflag (int32)
      80-83:  channelno (int32)
      84-87:  datamultiplepowerof10 (int32)
      88-127: htscsecurityid[40]
      128-143: securitystatus[16]
    """
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_ORDER_V2)

    local_recv_ts = struct.unpack('<q', data[0:8])[0]
    orderindex = struct.unpack('<q', data[8:16])[0]
    orderprice = struct.unpack('<q', data[16:24])[0]
    orderqty = struct.unpack('<q', data[24:32])[0]
    orderno = struct.unpack('<q', data[32:40])[0]
    tradedqty = struct.unpack('<q', data[40:48])[0]
    applseqnum = struct.unpack('<q', data[48:56])[0]
    mddate = struct.unpack('<i', data[56:60])[0]
    mdtime = struct.unpack('<i', data[60:64])[0]
    ordertype = struct.unpack('<i', data[72:76])[0]
    orderbsflag = struct.unpack('<i', data[76:80])[0]

    symbol = data[88:128].rstrip(b'\x00')

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': local_recv_ts,
        'orderindex': orderindex,
        'ordertype': ordertype,
        'orderprice': orderprice,
        'orderqty': orderqty,
        'orderbsflag': orderbsflag,
        'orderno': orderno,
        'applseqnum': applseqnum,
    }


def read_txn_v2(mm, offset):
    """读取 V2 transaction 记录

    MDTransactionStruct V2 布局:
      0-7:    local_recv_timestamp (int64)
      8-15:   tradeindex (int64)
      16-23:  tradebuyno (int64)
      24-31:  tradesellno (int64)
      32-39:  tradeprice (int64)
      40-47:  tradeqty (int64)
      48-55:  trademoney (int64)
      56-63:  applseqnum (int64)
      64-67:  mddate (int32)
      68-71:  mdtime (int32)
      72-75:  securityidsource (int32)
      76-79:  securitytype (int32)
      80-83:  tradetype (int32)
      84-87:  tradebsflag (int32)
      88-91:  channelno (int32)
      92-95:  datamultiplepowerof10 (int32)
      96-135: htscsecurityid[40]
    """
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TRANSACTION_V2)

    local_recv_ts = struct.unpack('<q', data[0:8])[0]
    tradeindex = struct.unpack('<q', data[8:16])[0]
    tradebuyno = struct.unpack('<q', data[16:24])[0]
    tradesellno = struct.unpack('<q', data[24:32])[0]
    tradeprice = struct.unpack('<q', data[32:40])[0]
    tradeqty = struct.unpack('<q', data[40:48])[0]
    trademoney = struct.unpack('<q', data[48:56])[0]
    applseqnum = struct.unpack('<q', data[56:64])[0]
    mddate = struct.unpack('<i', data[64:68])[0]
    mdtime = struct.unpack('<i', data[68:72])[0]
    tradetype = struct.unpack('<i', data[80:84])[0]
    tradebsflag = struct.unpack('<i', data[84:88])[0]

    symbol = data[96:136].rstrip(b'\x00')

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': local_recv_ts,
        'tradeindex': tradeindex,
        'tradebuyno': tradebuyno,
        'tradesellno': tradesellno,
        'tradetype': tradetype,
        'tradebsflag': tradebsflag,
        'tradeprice': tradeprice,
        'tradeqty': tradeqty,
        'applseqnum': applseqnum,
    }


def read_snap_v2(mm, offset):
    """读取 V2 orderbook snapshot 记录

    MDOrderbookStruct V2 布局:
      0-239:    buyentries[10] (每个 24 bytes)
      240-479:  sellentries[10]
      480-487:  local_recv_timestamp (int64)
      488-495:  datatimestamp (int64)
      ...
      688-691:  mddate (int32)
      692-695:  mdtime (int32)
      ...
      728-767:  htscsecurityid[40] (注意这不对，重新检查)
    """
    # 注意：MDOrderbookStruct 的布局需要重新确认
    # 这里先按照计划中的布局实现
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_SNAPSHOT_V2)

    # buyentries/sellentries 占 480 bytes (20 * 24)
    # local_recv_timestamp 在 480
    local_recv_ts = struct.unpack('<q', data[480:488])[0]
    datatimestamp = struct.unpack('<q', data[488:496])[0]
    lastpx = struct.unpack('<q', data[536:544])[0]

    # htscsecurityid 在固定位置 (根据结构体定义)
    # 需要根据实际布局调整
    mddate = struct.unpack('<i', data[688:692])[0]
    mdtime = struct.unpack('<i', data[692:696])[0]

    symbol = data[648:688].rstrip(b'\x00')

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': local_recv_ts,
        'datatimestamp': datatimestamp,
        'lastpx': lastpx,
    }


# ============================================================================
# V1 记录读取函数 (向后兼容)
# ============================================================================

def read_tick_v1(mm, offset):
    """读取 V1 tick 记录"""
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TICK_V1)

    symbol = data[:40].rstrip(b'\x00')
    mddate, mdtime = struct.unpack('<ii', data[40:48])
    maxpx, minpx, preclosepx = struct.unpack('<qqq', data[72:96])
    lastpx = struct.unpack('<q', data[120:128])[0]

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': 0,  # V1 没有此字段
        'datatimestamp': 0,
        'lastpx': lastpx,
        'maxpx': maxpx,
        'minpx': minpx,
        'preclosepx': preclosepx,
    }


def read_order_v1(mm, offset):
    """读取 V1 order 记录"""
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_ORDER_V1)

    symbol = data[:40].rstrip(b'\x00')
    mddate, mdtime = struct.unpack('<ii', data[40:48])
    orderindex = struct.unpack('<q', data[56:64])[0]
    ordertype = struct.unpack('<i', data[64:68])[0]
    orderprice = struct.unpack('<q', data[72:80])[0]
    orderqty = struct.unpack('<q', data[80:88])[0]
    orderbsflag = struct.unpack('<i', data[88:92])[0]

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': 0,  # V1 没有此字段
        'orderindex': orderindex,
        'ordertype': ordertype,
        'orderprice': orderprice,
        'orderqty': orderqty,
        'orderbsflag': orderbsflag,
        'orderno': 0,
        'applseqnum': orderindex,
    }


def read_txn_v1(mm, offset):
    """读取 V1 transaction 记录"""
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TRANSACTION_V1)

    symbol = data[:40].rstrip(b'\x00')
    mddate, mdtime = struct.unpack('<ii', data[40:48])
    tradeindex = struct.unpack('<q', data[56:64])[0]
    tradebuyno = struct.unpack('<q', data[64:72])[0]
    tradesellno = struct.unpack('<q', data[72:80])[0]
    tradetype = struct.unpack('<i', data[80:84])[0]
    tradebsflag = struct.unpack('<i', data[84:88])[0]
    tradeprice = struct.unpack('<q', data[88:96])[0]
    tradeqty = struct.unpack('<q', data[96:104])[0]

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'local_recv_ts': 0,  # V1 没有此字段
        'tradeindex': tradeindex,
        'tradebuyno': tradebuyno,
        'tradesellno': tradesellno,
        'tradetype': tradetype,
        'tradebsflag': tradebsflag,
        'tradeprice': tradeprice,
        'tradeqty': tradeqty,
        'applseqnum': tradeindex,
    }


# ============================================================================
# 通用搜索和打印函数
# ============================================================================

def get_symbol_from_record_v2(mm, offset, dtype):
    """从 V2 记录中提取股票代码"""
    mm.seek(HEADER_SIZE + offset)

    if dtype == 'tick':
        mm.seek(HEADER_SIZE + offset + 2168)
        symbol_bytes = mm.read(40)
    elif dtype == 'order':
        mm.seek(HEADER_SIZE + offset + 88)
        symbol_bytes = mm.read(40)
    elif dtype == 'txn':
        mm.seek(HEADER_SIZE + offset + 96)
        symbol_bytes = mm.read(40)
    elif dtype == 'snap':
        mm.seek(HEADER_SIZE + offset + 648)
        symbol_bytes = mm.read(40)
    else:
        return b''

    null_idx = symbol_bytes.find(b'\x00')
    if null_idx >= 0:
        return symbol_bytes[:null_idx]
    return symbol_bytes.rstrip(b'\x00')


def get_symbol_from_record_v1(mm, offset):
    """从 V1 记录中提取股票代码 (前 40 字节)"""
    mm.seek(HEADER_SIZE + offset)
    symbol_bytes = mm.read(40)
    null_idx = symbol_bytes.find(b'\x00')
    if null_idx >= 0:
        return symbol_bytes[:null_idx]
    return symbol_bytes.rstrip(b'\x00')


def search_symbol(filepath, target_symbol, record_size, read_func, dtype, format_version, page=1, page_size=50):
    """搜索特定股票的记录，支持分页"""
    results = []
    target_bytes = target_symbol.encode('utf-8') if isinstance(target_symbol, str) else target_symbol

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

        header = read_header(mm)
        print(f"File: {filepath}")
        print(f"  Magic: 0x{header['magic']:08X} ({magic_to_str(header['magic'])})")
        print(f"  Format: V{header['format_version']}")
        print(f"  Version: {header['version']}")
        print(f"  Struct size: {header['struct_size']}")
        print(f"  Record count: {header['record_count']}")
        print("")

        if header['record_count'] == 0:
            print("  No records in file")
            mm.close()
            return results, 0

        skip_count = (page - 1) * page_size
        found = 0
        skipped = 0

        for i in range(header['record_count']):
            offset = i * record_size

            if format_version == 2:
                symbol = get_symbol_from_record_v2(mm, offset, dtype)
            else:
                symbol = get_symbol_from_record_v1(mm, offset)

            if target_bytes in symbol:
                found += 1
                if skipped < skip_count:
                    skipped += 1
                    continue

                if len(results) < page_size:
                    record = read_func(mm, offset)
                    record['_idx'] = i
                    results.append(record)

        mm.close()

    return results, found


def read_tail_records(filepath, record_size, read_func, count=20):
    """读取文件末尾的记录"""
    results = []

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

        header = read_header(mm)
        print(f"File: {filepath}")
        print(f"  Magic: 0x{header['magic']:08X} ({magic_to_str(header['magic'])})")
        print(f"  Format: V{header['format_version']}")
        print(f"  Version: {header['version']}")
        print(f"  Struct size: {header['struct_size']}")
        print(f"  Record count: {header['record_count']}")
        print("")

        if header['record_count'] == 0:
            print("  No records in file")
            mm.close()
            return results, header['format_version']

        start_idx = max(0, header['record_count'] - count)
        for i in range(start_idx, header['record_count']):
            offset = i * record_size
            record = read_func(mm, offset)
            record['_idx'] = i
            results.append(record)

        mm.close()

    return results, header['format_version']


def print_record(r, dtype, idx, show_latency=True):
    """打印单条记录"""
    # 计算延迟
    latency_str = ""
    if show_latency and r.get('local_recv_ts', 0) > 0:
        latency_str = f" | recv={format_ns_timestamp(r['local_recv_ts'])}"
        if r.get('datatimestamp', 0) > 0:
            latency_ms = calc_latency_ms(r['local_recv_ts'], r['datatimestamp'])
            if latency_ms is not None:
                latency_str += f" | latency={latency_ms:.1f}ms"

    symbol_str = r['symbol'].decode('utf-8', errors='ignore') if isinstance(r['symbol'], bytes) else r['symbol']

    if dtype == 'tick':
        print(f"[{idx}] {symbol_str} | {format_time(r['time'])} | last={format_price(r['lastpx'])}"
              f" | max={format_price(r['maxpx'])} min={format_price(r['minpx'])}"
              f" | preclose={format_price(r['preclosepx'])}{latency_str}")

    elif dtype == 'order':
        bs = 'B' if r['orderbsflag'] == 1 else 'S' if r['orderbsflag'] == 2 else '?'
        print(f"[{idx}] {symbol_str} | {format_time(r['time'])} | idx={r['orderindex']}"
              f" | {bs} | price={format_price(r['orderprice'])} | qty={r['orderqty']}{latency_str}")

    elif dtype == 'txn':
        bs = 'B' if r['tradebsflag'] == 1 else 'S' if r['tradebsflag'] == 2 else '?'
        print(f"[{idx}] {symbol_str} | {format_time(r['time'])} | idx={r['tradeindex']}"
              f" | {bs} | price={format_price(r['tradeprice'])} | qty={r['tradeqty']}{latency_str}")

    elif dtype == 'snap':
        print(f"[{idx}] {symbol_str} | {format_time(r['time'])} | last={format_price(r['lastpx'])}{latency_str}")


def show_file_info(data_dir):
    """显示目录下所有 mmap 文件的信息"""
    files = [
        ('orders.bin', 'order'),
        ('transactions.bin', 'txn'),
        ('ticks.bin', 'tick'),
        ('snapshots.bin', 'snap'),
    ]

    print(f"\n{'='*80}")
    print(f"Data directory: {data_dir}")
    print(f"{'='*80}\n")

    for filename, dtype in files:
        filepath = os.path.join(data_dir, filename)
        if not os.path.exists(filepath):
            print(f"{filename}: NOT FOUND")
            continue

        try:
            with open(filepath, 'rb') as f:
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                header = read_header(mm)
                mm.close()

            file_size = os.path.getsize(filepath)
            file_size_mb = file_size / (1024 * 1024)

            print(f"{filename}:")
            print(f"  Format:       V{header['format_version']} (Magic: 0x{header['magic']:08X} = {magic_to_str(header['magic'])})")
            print(f"  Records:      {header['record_count']:,}")
            print(f"  Struct size:  {header['struct_size']} bytes")
            print(f"  File size:    {file_size_mb:.2f} MB")
            print("")
        except Exception as e:
            print(f"{filename}: ERROR - {e}")
            print("")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    data_dir = sys.argv[1]

    # --info 模式：显示文件信息
    if sys.argv[2] == '--info':
        show_file_info(data_dir)
        return

    # 文件配置 (V1 和 V2)
    file_configs = {
        'tick': {
            'filename': 'ticks.bin',
            1: (SIZE_TICK_V1, MAGIC_TICK_V1, read_tick_v1),
            2: (SIZE_TICK_V2, MAGIC_TICK_V2, read_tick_v2),
        },
        'order': {
            'filename': 'orders.bin',
            1: (SIZE_ORDER_V1, MAGIC_ORDER_V1, read_order_v1),
            2: (SIZE_ORDER_V2, MAGIC_ORDER_V2, read_order_v2),
        },
        'txn': {
            'filename': 'transactions.bin',
            1: (SIZE_TRANSACTION_V1, MAGIC_TRANSACTION_V1, read_txn_v1),
            2: (SIZE_TRANSACTION_V2, MAGIC_TRANSACTION_V2, read_txn_v2),
        },
        'snap': {
            'filename': 'snapshots.bin',
            1: (SIZE_SNAPSHOT_V1, MAGIC_SNAPSHOT_V1, read_tick_v1),  # V1 snap 用 tick reader 占位
            2: (SIZE_SNAPSHOT_V2, MAGIC_ORDERBOOK_V2, read_snap_v2),
        },
    }

    # --tail 模式：查看文件末尾记录
    if sys.argv[2] == '--tail':
        dtype = sys.argv[3] if len(sys.argv) > 3 else 'order'
        count = int(sys.argv[4]) if len(sys.argv) > 4 else 20

        if dtype not in file_configs:
            print(f"Unknown type: {dtype}")
            sys.exit(1)

        filepath = os.path.join(data_dir, file_configs[dtype]['filename'])

        if not os.path.exists(filepath):
            print(f"File not found: {filepath}")
            sys.exit(1)

        # 检测文件版本
        with open(filepath, 'rb') as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            header = read_header(mm)
            mm.close()

        format_version = header['format_version']
        if format_version == 0:
            print(f"Unknown file format (magic: 0x{header['magic']:08X})")
            sys.exit(1)

        record_size, _, read_func = file_configs[dtype][format_version]

        print("")
        print("=" * 80)
        print(f"TAIL: Last {count} {dtype.upper()} records (V{format_version} format)")
        print("=" * 80)

        results, _ = read_tail_records(filepath, record_size, read_func, count)

        if results:
            print("")
            for r in results:
                print_record(r, dtype, r['_idx'])
        return

    # 正常模式：搜索特定股票
    target_symbol = sys.argv[2]
    dtype = sys.argv[3] if len(sys.argv) > 3 else 'all'
    page = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    page_size = int(sys.argv[5]) if len(sys.argv) > 5 else 50

    if dtype == 'all':
        types_to_check = ['tick', 'order', 'txn']
    else:
        types_to_check = [dtype]

    for dt in types_to_check:
        if dt not in file_configs:
            print(f"Unknown type: {dt}")
            continue

        filepath = os.path.join(data_dir, file_configs[dt]['filename'])

        if not os.path.exists(filepath):
            print(f"File not found: {filepath}")
            continue

        # 检测文件版本
        with open(filepath, 'rb') as f:
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            header = read_header(mm)
            mm.close()

        format_version = header['format_version']
        if format_version == 0:
            print(f"Unknown file format (magic: 0x{header['magic']:08X})")
            continue

        record_size, _, read_func = file_configs[dt][format_version]

        print("")
        print("=" * 80)
        print(f"Checking {dt.upper()} data for {target_symbol} (V{format_version} format, page {page})")
        print("=" * 80)

        results, total_found = search_symbol(
            filepath, target_symbol, record_size, read_func, dt, format_version, page, page_size
        )

        total_pages = (total_found + page_size - 1) // page_size if total_found > 0 else 0
        print(f"Found {total_found} total records for {target_symbol}")
        print(f"Showing page {page} of {total_pages}")
        print("")

        if results:
            for i, r in enumerate(results):
                idx = (page - 1) * page_size + i + 1
                print_record(r, dt, idx)


if __name__ == '__main__':
    main()
