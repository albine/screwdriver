#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
从 mmap 二进制文件导入数据到 ClickHouse (Python 2.7 兼容)

支持 V2 格式的 mmap 文件：
- orders.bin -> MDOrderStruct
- transactions.bin -> MDTransactionStruct
- ticks.bin -> MDStockStruct

用法:
    python import_mmap_to_clickhouse.py /path/to/data/raw/2026/01/23 --password <pwd>
    python import_mmap_to_clickhouse.py /path/to/data/raw/2026/01/23 --password <pwd> --dry-run

注意:
- 此脚本在服务器上运行（Python 2.7.5）
- 表必须先用 create_clickhouse_tables_user.sql 创建
"""
from __future__ import print_function
import sys
import os
import struct
import mmap
import argparse
import subprocess
import tempfile

# ============================================================================
# 常量定义
# ============================================================================
HEADER_SIZE = 64

# Magic 定义 (V2)
MAGIC_ORDER_V2 = 0x4F524432       # "ORD2"
MAGIC_TRANSACTION_V2 = 0x54584E32 # "TXN2"
MAGIC_TICK_V2 = 0x54494B32        # "TIK2"

# 结构体大小
SIZE_ORDER_V2 = 144
SIZE_TRANSACTION_V2 = 136
SIZE_TICK_V2 = 2216

# ClickHouse 配置
CLICKHOUSE_HOST = 'localhost'
CLICKHOUSE_DATABASE = 'default'

# ============================================================================
# 辅助函数
# ============================================================================

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
        'write_offset': write_offset
    }


def get_char_field(data, length):
    """从 bytes 中提取 char 数组，去除 null 终止符"""
    # Python 2/3 兼容
    result = data[:length].split(b'\x00')[0]
    if isinstance(result, bytes):
        return result.decode('utf-8', errors='ignore')
    return result


def escape_tsv(value):
    """转义 TSV 字段值"""
    if value is None:
        return '\\N'
    if isinstance(value, bytes):
        value = value.decode('utf-8', errors='ignore')
    if isinstance(value, str):
        # 转义特殊字符
        return value.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')
    return str(value)


def format_array(arr):
    """格式化数组为 ClickHouse 格式: [v1,v2,v3,...]"""
    return '[' + ','.join(str(x) for x in arr) + ']'


# ============================================================================
# 解析函数
# ============================================================================

def parse_order_v2(data):
    """
    解析 MDOrderStruct V2 (144 bytes)

    内存布局:
    - offset 0-55: 7 个 int64_t (local_recv_timestamp, orderindex, orderprice, orderqty, orderno, tradedqty, applseqnum)
    - offset 56-87: 8 个 int32_t (mddate, mdtime, securityidsource, securitytype, ordertype, orderbsflag, channelno, datamultiplepowerof10)
    - offset 88-127: char htscsecurityid[40]
    - offset 128-143: char securitystatus[16]

    TSV 列顺序 (与 SQL 建表顺序一致):
    local_recv_timestamp, orderindex, orderprice, orderqty, orderno, tradedqty, applseqnum,
    mddate, mdtime, securityidsource, securitytype, ordertype, orderbsflag, channelno, datamultiplepowerof10,
    htscsecurityid, securitystatus
    """
    # 解析 int64_t 字段 (7 个)
    int64_fields = struct.unpack('<7q', data[0:56])
    local_recv_timestamp = int64_fields[0]
    orderindex = int64_fields[1]
    orderprice = int64_fields[2]
    orderqty = int64_fields[3]
    orderno = int64_fields[4]
    tradedqty = int64_fields[5]
    applseqnum = int64_fields[6]

    # 解析 int32_t 字段 (8 个)
    int32_fields = struct.unpack('<8i', data[56:88])
    mddate = int32_fields[0]
    mdtime = int32_fields[1]
    securityidsource = int32_fields[2]
    securitytype = int32_fields[3]
    ordertype = int32_fields[4]
    orderbsflag = int32_fields[5]
    channelno = int32_fields[6]
    datamultiplepowerof10 = int32_fields[7]

    # 解析 char 数组
    htscsecurityid = get_char_field(data[88:128], 40)
    securitystatus = get_char_field(data[128:144], 16)

    # 按 SQL 列顺序返回 TSV 行
    return '\t'.join([
        escape_tsv(local_recv_timestamp),
        escape_tsv(orderindex),
        escape_tsv(orderprice),
        escape_tsv(orderqty),
        escape_tsv(orderno),
        escape_tsv(tradedqty),
        escape_tsv(applseqnum),
        escape_tsv(mddate),
        escape_tsv(mdtime),
        escape_tsv(securityidsource),
        escape_tsv(securitytype),
        escape_tsv(ordertype),
        escape_tsv(orderbsflag),
        escape_tsv(channelno),
        escape_tsv(datamultiplepowerof10),
        escape_tsv(htscsecurityid),
        escape_tsv(securitystatus)
    ])


def parse_transaction_v2(data):
    """
    解析 MDTransactionStruct V2 (136 bytes)

    内存布局:
    - offset 0-63: 8 个 int64_t (local_recv_timestamp, tradeindex, tradebuyno, tradesellno, tradeprice, tradeqty, trademoney, applseqnum)
    - offset 64-95: 8 个 int32_t (mddate, mdtime, securityidsource, securitytype, tradetype, tradebsflag, channelno, datamultiplepowerof10)
    - offset 96-135: char htscsecurityid[40]

    TSV 列顺序 (与 SQL 建表顺序一致):
    local_recv_timestamp, tradeindex, tradebuyno, tradesellno, tradeprice, tradeqty, trademoney, applseqnum,
    mddate, mdtime, securityidsource, securitytype, tradetype, tradebsflag, channelno, datamultiplepowerof10,
    htscsecurityid
    """
    # 解析 int64_t 字段 (8 个)
    int64_fields = struct.unpack('<8q', data[0:64])
    local_recv_timestamp = int64_fields[0]
    tradeindex = int64_fields[1]
    tradebuyno = int64_fields[2]
    tradesellno = int64_fields[3]
    tradeprice = int64_fields[4]
    tradeqty = int64_fields[5]
    trademoney = int64_fields[6]
    applseqnum = int64_fields[7]

    # 解析 int32_t 字段 (8 个)
    int32_fields = struct.unpack('<8i', data[64:96])
    mddate = int32_fields[0]
    mdtime = int32_fields[1]
    securityidsource = int32_fields[2]
    securitytype = int32_fields[3]
    tradetype = int32_fields[4]
    tradebsflag = int32_fields[5]
    channelno = int32_fields[6]
    datamultiplepowerof10 = int32_fields[7]

    # 解析 char 数组
    htscsecurityid = get_char_field(data[96:136], 40)

    # 按 SQL 列顺序返回 TSV 行
    return '\t'.join([
        escape_tsv(local_recv_timestamp),
        escape_tsv(tradeindex),
        escape_tsv(tradebuyno),
        escape_tsv(tradesellno),
        escape_tsv(tradeprice),
        escape_tsv(tradeqty),
        escape_tsv(trademoney),
        escape_tsv(applseqnum),
        escape_tsv(mddate),
        escape_tsv(mdtime),
        escape_tsv(securityidsource),
        escape_tsv(securitytype),
        escape_tsv(tradetype),
        escape_tsv(tradebsflag),
        escape_tsv(channelno),
        escape_tsv(datamultiplepowerof10),
        escape_tsv(htscsecurityid)
    ])


def parse_tick_v2(data):
    """
    解析 MDStockStruct V2 (2216 bytes)

    内存布局:
    - offset 0-199: 25 个 int64_t 标量
    - offset 200-519: 40 个 int64_t 价格队列 (4 x 10)
    - offset 520-2119: 200 个 int64_t 订单队列 (4 x 50)
    - offset 2120-2167: 12 个 int32_t
    - offset 2168-2207: char htscsecurityid[40]
    - offset 2208: char tradingphasecode[1]
    - offset 2209-2215: char _pad[7]

    TSV 列顺序 (与 SQL 建表顺序一致):
    local_recv_timestamp, datatimestamp, maxpx, minpx, preclosepx, numtrades, totalvolumetrade, totalvaluetrade,
    lastpx, openpx, closepx, highpx, lowpx, totalbuyqty, totalsellqty, weightedavgbuypx, weightedavgsellpx,
    withdrawbuynumber, withdrawbuyamount, withdrawbuymoney, withdrawsellnumber, withdrawsellamount, withdrawsellmoney,
    totalbuynumber, totalsellnumber,
    buypricequeue, buyorderqtyqueue, sellpricequeue, sellorderqtyqueue,
    buyorderqueue, sellorderqueue, buynumordersqueue, sellnumordersqueue,
    mddate, mdtime, securityidsource, securitytype, numbuyorders, numsellorders, channelno, datamultiplepowerof10,
    buyorderqueue_count, sellorderqueue_count, buynumordersqueue_count, sellnumordersqueue_count,
    htscsecurityid, tradingphasecode
    """
    # 解析 25 个 int64_t 标量 (offset 0-199)
    int64_scalars = struct.unpack('<25q', data[0:200])
    local_recv_timestamp = int64_scalars[0]
    datatimestamp = int64_scalars[1]
    maxpx = int64_scalars[2]
    minpx = int64_scalars[3]
    preclosepx = int64_scalars[4]
    numtrades = int64_scalars[5]
    totalvolumetrade = int64_scalars[6]
    totalvaluetrade = int64_scalars[7]
    lastpx = int64_scalars[8]
    openpx = int64_scalars[9]
    closepx = int64_scalars[10]
    highpx = int64_scalars[11]
    lowpx = int64_scalars[12]
    totalbuyqty = int64_scalars[13]
    totalsellqty = int64_scalars[14]
    weightedavgbuypx = int64_scalars[15]
    weightedavgsellpx = int64_scalars[16]
    withdrawbuynumber = int64_scalars[17]
    withdrawbuyamount = int64_scalars[18]
    withdrawbuymoney = int64_scalars[19]
    withdrawsellnumber = int64_scalars[20]
    withdrawsellamount = int64_scalars[21]
    withdrawsellmoney = int64_scalars[22]
    totalbuynumber = int64_scalars[23]
    totalsellnumber = int64_scalars[24]

    # 解析价格队列 (offset 200-519)
    buypricequeue = list(struct.unpack('<10q', data[200:280]))
    buyorderqtyqueue = list(struct.unpack('<10q', data[280:360]))
    sellpricequeue = list(struct.unpack('<10q', data[360:440]))
    sellorderqtyqueue = list(struct.unpack('<10q', data[440:520]))

    # 解析订单队列 (offset 520-2119)
    buyorderqueue = list(struct.unpack('<50q', data[520:920]))
    sellorderqueue = list(struct.unpack('<50q', data[920:1320]))
    buynumordersqueue = list(struct.unpack('<50q', data[1320:1720]))
    sellnumordersqueue = list(struct.unpack('<50q', data[1720:2120]))

    # 解析 int32_t 字段 (offset 2120-2167, 12 个)
    int32_fields = struct.unpack('<12i', data[2120:2168])
    mddate = int32_fields[0]
    mdtime = int32_fields[1]
    securityidsource = int32_fields[2]
    securitytype = int32_fields[3]
    numbuyorders = int32_fields[4]
    numsellorders = int32_fields[5]
    channelno = int32_fields[6]
    datamultiplepowerof10 = int32_fields[7]
    buyorderqueue_count = int32_fields[8]
    sellorderqueue_count = int32_fields[9]
    buynumordersqueue_count = int32_fields[10]
    sellnumordersqueue_count = int32_fields[11]

    # 解析 char 字段 (offset 2168-2208)
    htscsecurityid = get_char_field(data[2168:2208], 40)

    # 解析 tradingphasecode (offset 2208, 1 byte)
    phase_byte = data[2208:2209]
    if isinstance(phase_byte[0], int):
        tradingphasecode = chr(phase_byte[0]) if phase_byte[0] != 0 else ''
    else:
        tradingphasecode = phase_byte[0] if phase_byte[0] != '\x00' else ''

    # 按 SQL 列顺序返回 TSV 行
    return '\t'.join([
        # int64 标量
        escape_tsv(local_recv_timestamp),
        escape_tsv(datatimestamp),
        escape_tsv(maxpx),
        escape_tsv(minpx),
        escape_tsv(preclosepx),
        escape_tsv(numtrades),
        escape_tsv(totalvolumetrade),
        escape_tsv(totalvaluetrade),
        escape_tsv(lastpx),
        escape_tsv(openpx),
        escape_tsv(closepx),
        escape_tsv(highpx),
        escape_tsv(lowpx),
        escape_tsv(totalbuyqty),
        escape_tsv(totalsellqty),
        escape_tsv(weightedavgbuypx),
        escape_tsv(weightedavgsellpx),
        escape_tsv(withdrawbuynumber),
        escape_tsv(withdrawbuyamount),
        escape_tsv(withdrawbuymoney),
        escape_tsv(withdrawsellnumber),
        escape_tsv(withdrawsellamount),
        escape_tsv(withdrawsellmoney),
        escape_tsv(totalbuynumber),
        escape_tsv(totalsellnumber),
        # int64 数组 (价格队列)
        format_array(buypricequeue),
        format_array(buyorderqtyqueue),
        format_array(sellpricequeue),
        format_array(sellorderqtyqueue),
        # int64 数组 (订单队列)
        format_array(buyorderqueue),
        format_array(sellorderqueue),
        format_array(buynumordersqueue),
        format_array(sellnumordersqueue),
        # int32 标量
        escape_tsv(mddate),
        escape_tsv(mdtime),
        escape_tsv(securityidsource),
        escape_tsv(securitytype),
        escape_tsv(numbuyorders),
        escape_tsv(numsellorders),
        escape_tsv(channelno),
        escape_tsv(datamultiplepowerof10),
        escape_tsv(buyorderqueue_count),
        escape_tsv(sellorderqueue_count),
        escape_tsv(buynumordersqueue_count),
        escape_tsv(sellnumordersqueue_count),
        # char 字段
        escape_tsv(htscsecurityid),
        escape_tsv(tradingphasecode)
    ])


# ============================================================================
# 导出和导入函数
# ============================================================================

def export_to_tsv(filepath, record_size, parse_func, output_path, magic_expected, limit=0):
    """从 mmap 文件导出为 TSV"""
    count = 0

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        if header['magic'] != magic_expected:
            print("ERROR: Magic mismatch. Expected 0x{:08X}, got 0x{:08X}".format(
                magic_expected, header['magic']))
            mm.close()
            return 0

        total_records = header['record_count']
        records_to_process = min(total_records, limit) if limit > 0 else total_records

        print("  File format: V2 (magic: 0x{:08X})".format(header['magic']))
        print("  Total records: {:,}".format(total_records))
        print("  Processing: {:,}{}".format(records_to_process, " (limited)" if limit > 0 else ""))
        print("  Struct size: {}".format(header['struct_size']))

        with open(output_path, 'w') as out:
            for i in range(records_to_process):
                offset = HEADER_SIZE + i * record_size
                mm.seek(offset)
                data = mm.read(record_size)

                try:
                    line = parse_func(data)
                    out.write(line + '\n')
                    count += 1
                except Exception as e:
                    print("  WARNING: Failed to parse record {}: {}".format(i, e))

        mm.close()

    return count


def import_tsv_to_clickhouse(tsv_path, table_name, password, host, database, dry_run=False):
    """使用 clickhouse-client 导入 TSV 文件"""
    if dry_run:
        print("  [DRY-RUN] Would import {} to {}".format(tsv_path, table_name))
        return True

    cmd = [
        'clickhouse-client',
        '--host', host,
        '--database', database,
        '--password', password,
        '--query', 'INSERT INTO {} FORMAT TabSeparated'.format(table_name)
    ]

    try:
        with open(tsv_path, 'r') as f:
            proc = subprocess.Popen(
                cmd,
                stdin=f,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            stdout, stderr = proc.communicate()

            if proc.returncode != 0:
                print("  ERROR: Import failed")
                print("  stderr: {}".format(stderr.decode('utf-8', errors='ignore')))
                return False

            return True
    except Exception as e:
        print("  ERROR: {}".format(e))
        return False


def process_file(data_dir, filename, table_name, record_size, parse_func, magic, password, host, database, dry_run, limit=0):
    """处理单个 mmap 文件"""
    filepath = os.path.join(data_dir, filename)

    if not os.path.exists(filepath):
        print("  SKIP: {} not found".format(filename))
        return 0

    print("\nProcessing {} -> {}".format(filename, table_name))

    # 创建临时 TSV 文件
    fd, tsv_path = tempfile.mkstemp(suffix='.tsv', prefix='mmap_import_')
    os.close(fd)

    try:
        # 导出为 TSV
        count = export_to_tsv(filepath, record_size, parse_func, tsv_path, magic, limit)

        if count == 0:
            print("  No records exported")
            return 0

        print("  Exported {} records to TSV".format(count))

        # 导入到 ClickHouse
        if import_tsv_to_clickhouse(tsv_path, table_name, password, host, database, dry_run):
            print("  {} Imported {} records to {}".format(
                '[DRY-RUN]' if dry_run else '[OK]',
                count, table_name))
            return count
        else:
            return 0
    finally:
        # 清理临时文件
        if os.path.exists(tsv_path):
            os.unlink(tsv_path)


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='Import mmap binary files to ClickHouse',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python import_mmap_to_clickhouse.py /path/to/data/raw/2026/01/23 --password mypassword
    python import_mmap_to_clickhouse.py /path/to/data/raw/2026/01/23 --password mypassword --dry-run

Files:
    orders.bin       -> MDOrderStruct
    transactions.bin -> MDTransactionStruct
    ticks.bin        -> MDStockStruct
'''
    )

    parser.add_argument('data_dir', help='Directory containing mmap files (orders.bin, transactions.bin, ticks.bin)')
    parser.add_argument('--password', required=True, help='ClickHouse password')
    parser.add_argument('--host', default=CLICKHOUSE_HOST, help='ClickHouse host (default: localhost)')
    parser.add_argument('--database', default=CLICKHOUSE_DATABASE, help='ClickHouse database (default: default)')
    parser.add_argument('--dry-run', action='store_true', help='Parse files but do not import')
    parser.add_argument('--only', choices=['orders', 'transactions', 'ticks'], help='Import only specified type')
    parser.add_argument('--limit', type=int, default=0, help='Limit number of records per file (0=unlimited)')

    args = parser.parse_args()

    # 使用命令行参数
    ch_host = args.host
    ch_database = args.database

    # 验证数据目录
    if not os.path.isdir(args.data_dir):
        print("ERROR: Data directory not found: {}".format(args.data_dir))
        sys.exit(1)

    print("=" * 60)
    print("mmap to ClickHouse Import")
    print("=" * 60)
    print("Data directory: {}".format(args.data_dir))
    print("ClickHouse: {}:{}".format(ch_host, ch_database))
    print("Dry run: {}".format(args.dry_run))

    # 文件映射
    file_configs = [
        ('orders.bin', 'MDOrderStruct', SIZE_ORDER_V2, parse_order_v2, MAGIC_ORDER_V2),
        ('transactions.bin', 'MDTransactionStruct', SIZE_TRANSACTION_V2, parse_transaction_v2, MAGIC_TRANSACTION_V2),
        ('ticks.bin', 'MDStockStruct', SIZE_TICK_V2, parse_tick_v2, MAGIC_TICK_V2),
    ]

    # 过滤
    if args.only:
        filter_map = {'orders': 'orders.bin', 'transactions': 'transactions.bin', 'ticks': 'ticks.bin'}
        file_configs = [c for c in file_configs if c[0] == filter_map[args.only]]

    total_records = 0

    for filename, table_name, record_size, parse_func, magic in file_configs:
        count = process_file(
            args.data_dir,
            filename,
            table_name,
            record_size,
            parse_func,
            magic,
            args.password,
            ch_host,
            ch_database,
            args.dry_run,
            args.limit
        )
        total_records += count

    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    print("Total records imported: {}".format(total_records))

    if args.dry_run:
        print("\n[DRY-RUN] No data was actually imported.")


if __name__ == '__main__':
    main()
