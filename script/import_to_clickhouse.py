#!/usr/bin/env python3
"""
高频市场数据导入 ClickHouse

将 mmap 二进制文件转换并导入 ClickHouse

用法:
    python import_to_clickhouse.py /data/raw/2026/01/18 [--dry-run]
"""

import struct
import os
import sys
import subprocess
from datetime import datetime
from typing import Iterator, Tuple, Any

# 文件 Magic 定义
MAGIC_ORDER = 0x4D444F52       # "MDOR"
MAGIC_TRANSACTION = 0x4D445458  # "MDTX"
MAGIC_TICK = 0x4D44544B        # "MDTK"
MAGIC_SNAPSHOT = 0x4D444F42    # "MDOB"

# Header 大小
HEADER_SIZE = 64

# Struct 大小
ORDER_SIZE = 144
TRANSACTION_SIZE = 128
TICK_SIZE = 2216
SNAPSHOT_SIZE = 728

# Struct 格式定义 (使用 native byte order with padding)
# MDOrderStruct (144 bytes)
ORDER_FORMAT = '<40s i i i i q i xxxx q q i i q q q i 16s xxxx'
ORDER_FIELDS = [
    'htscsecurityid', 'mddate', 'mdtime', 'securityidsource', 'securitytype',
    'orderindex', 'ordertype', 'orderprice', 'orderqty', 'orderbsflag',
    'channelno', 'orderno', 'tradedqty', 'applseqnum', 'datamultiplepowerof10',
    'securitystatus'
]

# MDTransactionStruct (128 bytes)
TXN_FORMAT = '<40s i i i i q q q i i q q q q i i'
TXN_FIELDS = [
    'htscsecurityid', 'mddate', 'mdtime', 'securityidsource', 'securitytype',
    'tradeindex', 'tradebuyno', 'tradesellno', 'tradetype', 'tradebsflag',
    'tradeprice', 'tradeqty', 'trademoney', 'applseqnum', 'channelno',
    'datamultiplepowerof10'
]


def read_header(f) -> Tuple[int, int, int, int, int]:
    """读取文件头，返回 (magic, version, struct_size, record_count, write_offset)"""
    header_data = f.read(HEADER_SIZE)
    if len(header_data) < HEADER_SIZE:
        raise ValueError("File too small for header")

    # Header: magic(4) + version(2) + struct_size(2) + record_count(8) + write_offset(8) + reserved(40)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', header_data[:24])
    return magic, version, struct_size, record_count, write_offset


def decode_string(data: bytes) -> str:
    """解码 C 字符串 (以 null 结尾)"""
    null_pos = data.find(b'\x00')
    if null_pos >= 0:
        data = data[:null_pos]
    return data.decode('utf-8', errors='replace')


def read_orders(filepath: str) -> Iterator[dict]:
    """读取委托数据"""
    with open(filepath, 'rb') as f:
        magic, version, struct_size, record_count, write_offset = read_header(f)

        if magic != MAGIC_ORDER:
            raise ValueError(f"Invalid magic for orders file: 0x{magic:08X}")
        if struct_size != ORDER_SIZE:
            raise ValueError(f"Struct size mismatch: {struct_size} vs {ORDER_SIZE}")

        print(f"  Orders file: {record_count} records")

        for i in range(record_count):
            data = f.read(ORDER_SIZE)
            if len(data) < ORDER_SIZE:
                break

            values = struct.unpack(ORDER_FORMAT, data)
            record = dict(zip(ORDER_FIELDS, values))

            # 解码字符串
            record['htscsecurityid'] = decode_string(record['htscsecurityid'])
            record['securitystatus'] = decode_string(record['securitystatus'])

            yield record


def read_transactions(filepath: str) -> Iterator[dict]:
    """读取成交数据"""
    with open(filepath, 'rb') as f:
        magic, version, struct_size, record_count, write_offset = read_header(f)

        if magic != MAGIC_TRANSACTION:
            raise ValueError(f"Invalid magic for transactions file: 0x{magic:08X}")
        if struct_size != TRANSACTION_SIZE:
            raise ValueError(f"Struct size mismatch: {struct_size} vs {TRANSACTION_SIZE}")

        print(f"  Transactions file: {record_count} records")

        for i in range(record_count):
            data = f.read(TRANSACTION_SIZE)
            if len(data) < TRANSACTION_SIZE:
                break

            values = struct.unpack(TXN_FORMAT, data)
            record = dict(zip(TXN_FIELDS, values))

            # 解码字符串
            record['htscsecurityid'] = decode_string(record['htscsecurityid'])

            yield record


def format_date(mddate: int) -> str:
    """将 YYYYMMDD 格式的整数转换为日期字符串"""
    return f"{mddate // 10000:04d}-{(mddate // 100) % 100:02d}-{mddate % 100:02d}"


def escape_string(s: str) -> str:
    """转义字符串用于 ClickHouse 插入"""
    return s.replace("\\", "\\\\").replace("'", "\\'")


def import_orders(data_dir: str, dry_run: bool = False):
    """导入委托数据到 ClickHouse"""
    orders_file = os.path.join(data_dir, 'orders.bin')
    if not os.path.exists(orders_file):
        print(f"  Orders file not found: {orders_file}")
        return

    # 生成 CSV 临时文件
    csv_file = '/tmp/orders_import.csv'
    count = 0

    with open(csv_file, 'w') as f:
        for record in read_orders(orders_file):
            date_str = format_date(record['mddate'])
            f.write(f"{date_str}\t{record['mdtime']}\t{escape_string(record['htscsecurityid'])}\t"
                    f"{record['securityidsource']}\t{record['securitytype']}\t{record['orderindex']}\t"
                    f"{record['ordertype']}\t{record['orderprice']}\t{record['orderqty']}\t"
                    f"{record['orderbsflag']}\t{record['channelno']}\t{record['orderno']}\t"
                    f"{record['tradedqty']}\t{record['applseqnum']}\t{record['datamultiplepowerof10']}\t"
                    f"{escape_string(record['securitystatus'])}\n")
            count += 1

    print(f"  Written {count} orders to {csv_file}")

    if not dry_run:
        cmd = f"clickhouse-client --database market_data --query \"INSERT INTO orders FORMAT TabSeparated\" < {csv_file}"
        print(f"  Running: {cmd}")
        subprocess.run(cmd, shell=True, check=True)
        print(f"  Imported {count} orders")


def import_transactions(data_dir: str, dry_run: bool = False):
    """导入成交数据到 ClickHouse"""
    txn_file = os.path.join(data_dir, 'transactions.bin')
    if not os.path.exists(txn_file):
        print(f"  Transactions file not found: {txn_file}")
        return

    # 生成 CSV 临时文件
    csv_file = '/tmp/transactions_import.csv'
    count = 0

    with open(csv_file, 'w') as f:
        for record in read_transactions(txn_file):
            date_str = format_date(record['mddate'])
            f.write(f"{date_str}\t{record['mdtime']}\t{escape_string(record['htscsecurityid'])}\t"
                    f"{record['securityidsource']}\t{record['securitytype']}\t{record['tradeindex']}\t"
                    f"{record['tradebuyno']}\t{record['tradesellno']}\t{record['tradetype']}\t"
                    f"{record['tradebsflag']}\t{record['tradeprice']}\t{record['tradeqty']}\t"
                    f"{record['trademoney']}\t{record['applseqnum']}\t{record['channelno']}\t"
                    f"{record['datamultiplepowerof10']}\n")
            count += 1

    print(f"  Written {count} transactions to {csv_file}")

    if not dry_run:
        cmd = f"clickhouse-client --database market_data --query \"INSERT INTO transactions FORMAT TabSeparated\" < {csv_file}"
        print(f"  Running: {cmd}")
        subprocess.run(cmd, shell=True, check=True)
        print(f"  Imported {count} transactions")


def main():
    if len(sys.argv) < 2:
        print("Usage: import_to_clickhouse.py <data_dir> [--dry-run]")
        print("  data_dir: Path to daily data directory (e.g., /data/raw/2026/01/18)")
        print("  --dry-run: Only generate CSV files, don't import")
        sys.exit(1)

    data_dir = sys.argv[1]
    dry_run = '--dry-run' in sys.argv

    if not os.path.isdir(data_dir):
        print(f"Error: Directory not found: {data_dir}")
        sys.exit(1)

    print(f"Importing data from: {data_dir}")
    print(f"Dry run: {dry_run}")
    print()

    print("Importing orders...")
    import_orders(data_dir, dry_run)
    print()

    print("Importing transactions...")
    import_transactions(data_dir, dry_run)
    print()

    # TODO: 添加 ticks 和 snapshots 的导入
    # 由于这两个 struct 较大且包含数组，需要更复杂的处理

    print("Done!")


if __name__ == '__main__':
    main()
