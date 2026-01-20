#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
读取 mmap 持久化文件，检查特定股票的数据
兼容 Python 2.7

用法:
    python read_mmap.py <data_dir> <symbol> [tick|order|txn] [page] [page_size]
    python read_mmap.py <data_dir> --tail [tick|order|txn] [count]  # 查看末尾记录
"""

from __future__ import print_function
import sys
import struct
import mmap
import os

# Magic 定义
MAGIC_ORDER = 0x4D444F52  # "MDOR"
MAGIC_TRANSACTION = 0x4D445458  # "MDTX"
MAGIC_TICK = 0x4D44544B  # "MDTK"
MAGIC_SNAPSHOT = 0x4D444F42  # "MDOB"

# 结构体大小
SIZE_ORDER = 144
SIZE_TRANSACTION = 128
SIZE_TICK = 2216
SIZE_SNAPSHOT = 728

# Header 大小
HEADER_SIZE = 64


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
    }


def get_symbol_from_record(mm, offset):
    """从记录中提取股票代码 (前 40 字节)"""
    mm.seek(HEADER_SIZE + offset)
    symbol_bytes = mm.read(40)
    null_idx = symbol_bytes.find(b'\x00')
    if null_idx >= 0:
        return symbol_bytes[:null_idx]
    return symbol_bytes.rstrip(b'\x00')


def format_time(mdtime):
    """格式化时间 HHMMSSMMM -> HH:MM:SS.mmm"""
    h = mdtime // 10000000
    m = (mdtime // 100000) % 100
    s = (mdtime // 1000) % 100
    ms = mdtime % 1000
    return "%02d:%02d:%02d.%03d" % (h, m, s, ms)


def format_price(price):
    """格式化价格 (价格 * 10000)"""
    return "%.2f" % (price / 10000.0)


def read_tick_record(mm, offset):
    """读取 tick 记录

    MDStockStruct 布局 (考虑对齐):
      0-39:   htscsecurityid[40]
      40-43:  mddate (int32)
      44-47:  mdtime (int32)
      48-55:  datatimestamp (int64)
      56:     tradingphasecode (char)
      57-59:  [padding 3 bytes]
      60-63:  securityidsource (int32)
      64-67:  securitytype (int32)
      68-71:  [padding 4 bytes for int64 alignment]
      72-79:  maxpx (int64)
      80-87:  minpx (int64)
      88-95:  preclosepx (int64)
      96-103: numtrades (int64)
      104-111: totalvolumetrade (int64)
      112-119: totalvaluetrade (int64)
      120-127: lastpx (int64)
      128-135: openpx (int64)
      ...
      Total: 2216 bytes
    """
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TICK)

    symbol = data[:40].rstrip(b'\x00')
    mddate, mdtime = struct.unpack('<ii', data[40:48])
    # 修正偏移: maxpx 在 72, minpx 在 80, preclosepx 在 88
    maxpx, minpx, preclosepx = struct.unpack('<qqq', data[72:96])
    # lastpx 在 120
    lastpx = struct.unpack('<q', data[120:128])[0]

    return {
        'symbol': symbol,
        'date': mddate,
        'time': mdtime,
        'lastpx': lastpx,
        'maxpx': maxpx,
        'minpx': minpx,
        'preclosepx': preclosepx,
    }


def read_order_record(mm, offset):
    """读取 order 记录"""
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_ORDER)

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
        'orderindex': orderindex,
        'ordertype': ordertype,
        'orderprice': orderprice,
        'orderqty': orderqty,
        'orderbsflag': orderbsflag,
    }


def read_txn_record(mm, offset):
    """读取 transaction 记录"""
    mm.seek(HEADER_SIZE + offset)
    data = mm.read(SIZE_TRANSACTION)

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
        'tradeindex': tradeindex,
        'tradebuyno': tradebuyno,
        'tradesellno': tradesellno,
        'tradetype': tradetype,
        'tradebsflag': tradebsflag,
        'tradeprice': tradeprice,
        'tradeqty': tradeqty,
    }


def search_symbol(filepath, target_symbol, record_size, read_func, page=1, page_size=50):
    """搜索特定股票的记录，支持分页"""
    results = []

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

        header = read_header(mm)
        print("File: %s" % filepath)
        print("  Magic: 0x%08X" % header['magic'])
        print("  Version: %d" % header['version'])
        print("  Struct size: %d" % header['struct_size'])
        print("  Record count: %d" % header['record_count'])
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
            symbol = get_symbol_from_record(mm, offset)

            if target_symbol in symbol:
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
        print("File: %s" % filepath)
        print("  Magic: 0x%08X" % header['magic'])
        print("  Version: %d" % header['version'])
        print("  Struct size: %d" % header['struct_size'])
        print("  Record count: %d" % header['record_count'])
        print("")

        if header['record_count'] == 0:
            print("  No records in file")
            mm.close()
            return results

        # 读取最后 count 条记录
        start_idx = max(0, header['record_count'] - count)
        for i in range(start_idx, header['record_count']):
            offset = i * record_size
            record = read_func(mm, offset)
            record['_idx'] = i
            results.append(record)

        mm.close()

    return results


def print_record(r, dtype, idx):
    """打印单条记录"""
    if dtype == 'tick':
        print("[%d] %s | %s | last=%s | max=%s min=%s preclose=%s" % (
            idx, r['symbol'], format_time(r['time']),
            format_price(r['lastpx']),
            format_price(r['maxpx']), format_price(r['minpx']),
            format_price(r['preclosepx'])))
    elif dtype == 'order':
        bs = 'B' if r['orderbsflag'] == 1 else 'S' if r['orderbsflag'] == 2 else '?'
        print("[%d] %s | %s | idx=%d | %s | price=%s | qty=%d" % (
            idx, r['symbol'], format_time(r['time']),
            r['orderindex'], bs, format_price(r['orderprice']),
            r['orderqty']))
    elif dtype == 'txn':
        bs = 'B' if r['tradebsflag'] == 1 else 'S' if r['tradebsflag'] == 2 else '?'
        print("[%d] %s | %s | idx=%d | %s | price=%s | qty=%d" % (
            idx, r['symbol'], format_time(r['time']),
            r['tradeindex'], bs, format_price(r['tradeprice']),
            r['tradeqty']))


def main():
    if len(sys.argv) < 3:
        print("用法:")
        print("  python read_mmap.py <data_dir> <symbol> [tick|order|txn] [page] [page_size]")
        print("  python read_mmap.py <data_dir> --tail [tick|order|txn] [count]")
        print("")
        print("示例:")
        print("  python read_mmap.py /data/raw/2026/01/20 300758")
        print("  python read_mmap.py /data/raw/2026/01/20 300758 tick")
        print("  python read_mmap.py /data/raw/2026/01/20 300758 order 2 100")
        print("  python read_mmap.py /data/raw/2026/01/20 --tail order 50")
        sys.exit(1)

    data_dir = sys.argv[1]

    file_configs = {
        'tick': ('ticks.bin', SIZE_TICK, MAGIC_TICK, read_tick_record),
        'order': ('orders.bin', SIZE_ORDER, MAGIC_ORDER, read_order_record),
        'txn': ('transactions.bin', SIZE_TRANSACTION, MAGIC_TRANSACTION, read_txn_record),
    }

    # --tail 模式：查看文件末尾记录
    if sys.argv[2] == '--tail':
        dtype = sys.argv[3] if len(sys.argv) > 3 else 'order'
        count = int(sys.argv[4]) if len(sys.argv) > 4 else 20

        if dtype not in file_configs:
            print("Unknown type: %s" % dtype)
            sys.exit(1)

        filename, record_size, magic, read_func = file_configs[dtype]
        filepath = os.path.join(data_dir, filename)

        if not os.path.exists(filepath):
            print("File not found: %s" % filepath)
            sys.exit(1)

        print("")
        print("=" * 70)
        print("TAIL: Last %d %s records" % (count, dtype.upper()))
        print("=" * 70)

        results = read_tail_records(filepath, record_size, read_func, count)

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
            print("Unknown type: %s" % dt)
            continue

        filename, record_size, magic, read_func = file_configs[dt]
        filepath = os.path.join(data_dir, filename)

        if not os.path.exists(filepath):
            print("File not found: %s" % filepath)
            continue

        print("")
        print("=" * 70)
        print("Checking %s data for %s (page %d, size %d)" % (dt.upper(), target_symbol, page, page_size))
        print("=" * 70)

        results, total_found = search_symbol(filepath, target_symbol, record_size, read_func, page, page_size)

        total_pages = (total_found + page_size - 1) // page_size if total_found > 0 else 0
        print("Found %d total records for %s" % (total_found, target_symbol))
        print("Showing page %d of %d (records %d-%d)" % (
            page, total_pages,
            (page - 1) * page_size + 1,
            min(page * page_size, total_found)
        ))
        print("")

        if results:
            for i, r in enumerate(results):
                idx = (page - 1) * page_size + i + 1
                print_record(r, dt, idx)


if __name__ == '__main__':
    main()
