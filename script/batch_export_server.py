#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
批量导出脚本 - 一次遍历 mmap 文件导出多个股票

优势：只遍历一次 mmap 文件，而不是每个股票遍历一次
     115 个股票从 ~10 小时 -> ~15 分钟

用法（在服务器上运行）:
    python batch_export_server.py <data_dir> <symbols_file> <output_dir> [end_time]

示例:
    python batch_export_server.py /home/jiace/project/trading-engine/data/raw/2026/01/22 symbols.txt /tmp/output 094500
"""
from __future__ import print_function
import sys
import struct
import mmap
import os
import time

HEADER_SIZE = 64
SIZE_ORDER = 144
SIZE_TRANSACTION = 128
SIZE_TICK = 2216


def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {'record_count': record_count, 'struct_size': struct_size}


def load_symbols(symbols_file):
    """读取股票代码列表，返回 set（不带后缀）"""
    symbols = set()
    with open(symbols_file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            # 支持带后缀或不带后缀
            symbol = line.split(',')[0].strip()
            if '.' in symbol:
                symbol = symbol.split('.')[0]
            if symbol:
                symbols.add(symbol)
    return symbols


def get_full_symbol(symbol_base):
    """根据代码获取完整 symbol（带交易所后缀）"""
    if symbol_base.startswith('6'):
        return symbol_base + '.SH'
    else:
        return symbol_base + '.SZ'


def export_orders_batch(filepath, symbols, output_dir, end_time=None):
    """批量导出 order 数据 - 一次遍历导出所有股票"""
    # 每个股票一个输出文件
    files = {}
    counts = {}
    for sym in symbols:
        full_sym = get_full_symbol(sym)
        output_path = os.path.join(output_dir, 'MD_ORDER_StockType_{}.csv'.format(full_sym))
        files[sym] = open(output_path, 'w')
        counts[sym] = 0

    # 将 symbols 转为 bytes set 以加速匹配
    symbols_bytes = set(s.encode('utf-8') for s in symbols)

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        total_records = header['record_count']

        print("Processing orders.bin: {:,} records".format(total_records))
        sys.stdout.flush()
        last_progress = -1
        last_time = time.time()
        start_time = last_time

        for i in range(total_records):
            # 进度显示（每1%或每30秒）
            progress = (i * 100) // total_records
            now = time.time()
            if progress > last_progress or now - last_time >= 30:
                elapsed = now - start_time
                speed = i / elapsed if elapsed > 0 else 0
                eta = (total_records - i) / speed if speed > 0 else 0
                matched = sum(counts.values())
                print("  Orders: {}% ({:,}/{:,}) speed={:,.0f}/s ETA={:.0f}s matched={:,}".format(
                    progress, i, total_records, speed, eta, matched))
                sys.stdout.flush()
                last_progress = progress
                last_time = now

            offset = HEADER_SIZE + i * SIZE_ORDER
            mm.seek(offset)
            data = mm.read(SIZE_ORDER)

            sym = data[:40].rstrip(b'\x00')
            # 快速检查是否为目标股票
            matched_sym = None
            for target in symbols_bytes:
                if target in sym:
                    matched_sym = target.decode('utf-8')
                    break
            if not matched_sym:
                continue

            mddate, mdtime = struct.unpack('<ii', data[40:48])
            # 时间过滤
            if end_time and mdtime >= end_time:
                continue

            orderindex = struct.unpack('<q', data[56:64])[0]
            ordertype = struct.unpack('<i', data[64:68])[0]
            orderprice = struct.unpack('<q', data[72:80])[0]
            orderqty = struct.unpack('<q', data[80:88])[0]
            orderbsflag = struct.unpack('<i', data[88:92])[0]
            channelno = struct.unpack('<i', data[92:96])[0]
            orderno = struct.unpack('<q', data[96:104])[0]
            applseqnum = struct.unpack('<q', data[112:120])[0]

            sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
            if matched_sym.startswith('6'):
                exchange = 'XSHG'
            else:
                exchange = 'XSHE'

            line = 'HTSCSecurityID: "{}" MDDate: {} MDTime: {} securityIDSource: {} securityType: StockType '.format(
                sym_str, mddate, mdtime, exchange)
            line += 'OrderIndex: {} '.format(orderindex)
            if orderno > 0:
                line += 'OrderNO: {} '.format(orderno)
            line += 'OrderType: {} OrderPrice: {} OrderQty: {} OrderBSFlag: {} '.format(
                ordertype, orderprice, orderqty, orderbsflag)
            line += 'ChannelNo: {} ApplSeqNum: {} DataMultiplePowerOf10: 4'.format(
                channelno, applseqnum)

            files[matched_sym].write(line + '\n')
            counts[matched_sym] += 1

        mm.close()

    # 关闭所有文件
    for f in files.values():
        f.close()

    print("  Orders: 100%")
    return counts


def export_transactions_batch(filepath, symbols, output_dir, end_time=None):
    """批量导出 transaction 数据 - 一次遍历导出所有股票"""
    files = {}
    counts = {}
    for sym in symbols:
        full_sym = get_full_symbol(sym)
        output_path = os.path.join(output_dir, 'MD_TRANSACTION_StockType_{}.csv'.format(full_sym))
        files[sym] = open(output_path, 'w')
        counts[sym] = 0

    symbols_bytes = set(s.encode('utf-8') for s in symbols)

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        total_records = header['record_count']

        print("Processing transactions.bin: {:,} records".format(total_records))
        sys.stdout.flush()
        last_progress = -1
        last_time = time.time()
        start_time = last_time

        for i in range(total_records):
            progress = (i * 100) // total_records
            now = time.time()
            if progress > last_progress or now - last_time >= 30:
                elapsed = now - start_time
                speed = i / elapsed if elapsed > 0 else 0
                eta = (total_records - i) / speed if speed > 0 else 0
                matched = sum(counts.values())
                print("  Transactions: {}% ({:,}/{:,}) speed={:,.0f}/s ETA={:.0f}s matched={:,}".format(
                    progress, i, total_records, speed, eta, matched))
                sys.stdout.flush()
                last_progress = progress
                last_time = now

            offset = HEADER_SIZE + i * SIZE_TRANSACTION
            mm.seek(offset)
            data = mm.read(SIZE_TRANSACTION)

            sym = data[:40].rstrip(b'\x00')
            matched_sym = None
            for target in symbols_bytes:
                if target in sym:
                    matched_sym = target.decode('utf-8')
                    break
            if not matched_sym:
                continue

            mddate, mdtime = struct.unpack('<ii', data[40:48])
            if end_time and mdtime >= end_time:
                continue

            tradeindex = struct.unpack('<q', data[56:64])[0]
            tradebuyno = struct.unpack('<q', data[64:72])[0]
            tradesellno = struct.unpack('<q', data[72:80])[0]
            tradetype = struct.unpack('<i', data[80:84])[0]
            tradebsflag = struct.unpack('<i', data[84:88])[0]
            tradeprice = struct.unpack('<q', data[88:96])[0]
            tradeqty = struct.unpack('<q', data[96:104])[0]
            trademoney = struct.unpack('<q', data[104:112])[0]
            applseqnum = struct.unpack('<q', data[112:120])[0]
            channelno = struct.unpack('<i', data[120:124])[0]

            sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
            if matched_sym.startswith('6'):
                exchange = 'XSHG'
            else:
                exchange = 'XSHE'

            line = 'HTSCSecurityID: "{}" MDDate: {} MDTime: {} securityIDSource: {} securityType: StockType '.format(
                sym_str, mddate, mdtime, exchange)
            line += 'TradeIndex: {} '.format(tradeindex)
            if tradebuyno > 0:
                line += 'TradeBuyNo: {} '.format(tradebuyno)
            if tradesellno > 0:
                line += 'TradeSellNo: {} '.format(tradesellno)
            line += 'TradeType: {} TradeBSFlag: {} '.format(tradetype, tradebsflag)
            if tradeprice > 0:
                line += 'TradePrice: {} '.format(tradeprice)
            line += 'TradeQty: {} '.format(tradeqty)
            if trademoney > 0:
                line += 'TradeMoney: {} '.format(trademoney)
            line += 'ApplSeqNum: {} ChannelNo: {} DataMultiplePowerOf10: 4'.format(
                applseqnum, channelno)

            files[matched_sym].write(line + '\n')
            counts[matched_sym] += 1

        mm.close()

    for f in files.values():
        f.close()

    print("  Transactions: 100%")
    return counts


def export_ticks_batch(filepath, symbols, output_dir, end_time=None):
    """批量导出 tick 数据 - 一次遍历导出所有股票"""
    files = {}
    counts = {}
    for sym in symbols:
        full_sym = get_full_symbol(sym)
        output_path = os.path.join(output_dir, 'MD_TICK_StockType_{}.csv'.format(full_sym))
        files[sym] = open(output_path, 'w')
        counts[sym] = 0

    symbols_bytes = set(s.encode('utf-8') for s in symbols)

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        total_records = header['record_count']

        print("Processing ticks.bin: {:,} records".format(total_records))
        sys.stdout.flush()
        last_progress = -1
        last_time = time.time()
        start_time = last_time

        for i in range(total_records):
            progress = (i * 100) // total_records
            now = time.time()
            if progress > last_progress or now - last_time >= 30:
                elapsed = now - start_time
                speed = i / elapsed if elapsed > 0 else 0
                eta = (total_records - i) / speed if speed > 0 else 0
                matched = sum(counts.values())
                print("  Ticks: {}% ({:,}/{:,}) speed={:,.0f}/s ETA={:.0f}s matched={:,}".format(
                    progress, i, total_records, speed, eta, matched))
                sys.stdout.flush()
                last_progress = progress
                last_time = now

            offset = HEADER_SIZE + i * SIZE_TICK
            mm.seek(offset)
            data = mm.read(SIZE_TICK)

            sym = data[:40].rstrip(b'\x00')
            matched_sym = None
            for target in symbols_bytes:
                if target in sym:
                    matched_sym = target.decode('utf-8')
                    break
            if not matched_sym:
                continue

            mddate, mdtime = struct.unpack('<ii', data[40:48])
            if end_time and mdtime >= end_time:
                continue

            phase_byte = data[56:57]
            tradingphasecode = phase_byte if isinstance(phase_byte, str) else chr(phase_byte[0]) if phase_byte[0] != 0 else '0'
            maxpx, minpx, preclosepx = struct.unpack('<qqq', data[72:96])
            lastpx = struct.unpack('<q', data[120:128])[0]
            openpx = struct.unpack('<q', data[128:136])[0]
            highpx = struct.unpack('<q', data[144:152])[0]
            lowpx = struct.unpack('<q', data[152:160])[0]
            channelno = struct.unpack('<i', data[264:268])[0]

            buy_prices = struct.unpack('<10q', data[272:352])
            buy_qtys = struct.unpack('<10q', data[352:432])
            sell_prices = struct.unpack('<10q', data[432:512])
            sell_qtys = struct.unpack('<10q', data[512:592])

            sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
            if matched_sym.startswith('6'):
                exchange = 'XSHG'
            else:
                exchange = 'XSHE'

            line = 'HTSCSecurityID: "{}" MDDate: {} MDTime: {} TradingPhaseCode: "{}" '.format(
                sym_str, mddate, mdtime, tradingphasecode)
            line += 'securityIDSource: {} securityType: StockType '.format(exchange)
            line += 'MaxPx: {} MinPx: {} PreClosePx: {} '.format(maxpx, minpx, preclosepx)
            if lastpx > 0:
                line += 'LastPx: {} '.format(lastpx)
            if openpx > 0:
                line += 'OpenPx: {} '.format(openpx)
            if highpx > 0:
                line += 'HighPx: {} '.format(highpx)
            if lowpx > 0:
                line += 'LowPx: {} '.format(lowpx)
            line += 'ChannelNo: {} '.format(channelno)

            for p in buy_prices:
                line += 'BuyPriceQueue: {} '.format(p)
            for q in buy_qtys:
                line += 'BuyOrderQtyQueue: {} '.format(q)
            for p in sell_prices:
                line += 'SellPriceQueue: {} '.format(p)
            for q in sell_qtys:
                line += 'SellOrderQtyQueue: {} '.format(q)
            for _ in range(10):
                line += 'BuyNumOrdersQueue: 0 '
            for _ in range(10):
                line += 'SellNumOrdersQueue: 0 '

            line += 'DataMultiplePowerOf10: 4'
            files[matched_sym].write(line + '\n')
            counts[matched_sym] += 1

        mm.close()

    for f in files.values():
        f.close()

    print("  Ticks: 100%")
    return counts


def main():
    if len(sys.argv) < 4:
        print("Usage: python batch_export_server.py <data_dir> <symbols_file> <output_dir> [end_time]")
        print("")
        print("Arguments:")
        print("  data_dir     - Directory containing orders.bin, transactions.bin, ticks.bin")
        print("  symbols_file - File with stock symbols (one per line)")
        print("  output_dir   - Output directory for exported CSV files")
        print("  end_time     - Optional: end time filter in HHMMSS format (e.g., 094500)")
        sys.exit(1)

    data_dir = sys.argv[1]
    symbols_file = sys.argv[2]
    output_dir = sys.argv[3]
    end_time = int(sys.argv[4]) * 1000 if len(sys.argv) > 4 else None  # HHMMSS -> HHMMSSmmm

    # 加载股票列表
    symbols = load_symbols(symbols_file)
    print("Loaded {} symbols from {}".format(len(symbols), symbols_file))
    print("Output directory: {}".format(output_dir))
    if end_time:
        print("End time filter: {}".format(sys.argv[4]))
    print("")

    # 创建输出目录
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    start_time = time.time()

    # 导出 orders
    orders_file = os.path.join(data_dir, 'orders.bin')
    order_counts = {}
    if os.path.exists(orders_file):
        order_counts = export_orders_batch(orders_file, symbols, output_dir, end_time)
    else:
        print("Warning: orders.bin not found")

    # 导出 transactions
    txn_file = os.path.join(data_dir, 'transactions.bin')
    txn_counts = {}
    if os.path.exists(txn_file):
        txn_counts = export_transactions_batch(txn_file, symbols, output_dir, end_time)
    else:
        print("Warning: transactions.bin not found")

    # 导出 ticks
    ticks_file = os.path.join(data_dir, 'ticks.bin')
    tick_counts = {}
    if os.path.exists(ticks_file):
        tick_counts = export_ticks_batch(ticks_file, symbols, output_dir, end_time)
    else:
        print("Warning: ticks.bin not found")

    elapsed = time.time() - start_time

    # 打印统计
    print("")
    print("=" * 60)
    print("Export completed in {:.1f} seconds".format(elapsed))
    print("")
    print("Per-symbol statistics:")
    for sym in sorted(symbols):
        full_sym = get_full_symbol(sym)
        orders = order_counts.get(sym, 0)
        txns = txn_counts.get(sym, 0)
        ticks = tick_counts.get(sym, 0)
        print("  {}: orders={}, transactions={}, ticks={}".format(full_sym, orders, txns, ticks))
    print("=" * 60)


if __name__ == '__main__':
    main()
