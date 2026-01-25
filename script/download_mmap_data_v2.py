#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从服务器下载 mmap 市场数据 (V2 格式支持)

支持 V1 和 V2 格式的自动检测和导出。

V2 格式特点:
- 使用对齐结构体 (8字节对齐)
- 新增 local_recv_timestamp 字段 (纳秒)
- 使用新 Magic 区分: ORD2, TXN2, TIK2, OBK2

用法:
    python script/download_mmap_data_v2.py <symbol> [date]

示例:
    python script/download_mmap_data_v2.py 600759.SH          # 今天
    python script/download_mmap_data_v2.py 600759.SH 20260120 # 指定日期
"""

import os
import sys
import subprocess
import tempfile
from datetime import datetime

# SSH 配置
SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine.backup.20260123_111520/data/raw"
LOCAL_TEST_DATA = "test_data"

# ============================================================================
# 远程导出脚本 - 支持 V1 和 V2 格式
# ============================================================================
REMOTE_EXPORT_SCRIPT = r'''#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
临时导出脚本 - 从 mmap 文件导出为 protobuf text 格式
支持 V1 和 V2 格式自动检测
"""
from __future__ import print_function
import sys
import struct
import mmap
import os

HEADER_SIZE = 64

# Magic 定义
MAGIC_ORDER_V1 = 0x4D444F52       # "MDOR"
MAGIC_TRANSACTION_V1 = 0x4D445458 # "MDTX"
MAGIC_TICK_V1 = 0x4D44544B        # "MDTK"

MAGIC_ORDER_V2 = 0x4F524432       # "ORD2"
MAGIC_TRANSACTION_V2 = 0x54584E32 # "TXN2"
MAGIC_TICK_V2 = 0x54494B32        # "TIK2"

# 结构体大小
SIZE_ORDER_V1 = 144
SIZE_ORDER_V2 = 144
SIZE_TRANSACTION_V1 = 128
SIZE_TRANSACTION_V2 = 136
SIZE_TICK_V1 = 2216
SIZE_TICK_V2 = 2216


def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {
        'magic': magic,
        'record_count': record_count,
        'struct_size': struct_size
    }


def detect_version(magic):
    """检测文件版本"""
    if magic in (MAGIC_ORDER_V1, MAGIC_TRANSACTION_V1, MAGIC_TICK_V1):
        return 1
    elif magic in (MAGIC_ORDER_V2, MAGIC_TRANSACTION_V2, MAGIC_TICK_V2):
        return 2
    return 0


def get_phase_code(phase_byte):
    """兼容 Python 2/3 获取交易阶段代码"""
    b = phase_byte[0]
    # Python 3: b 是 int; Python 2: b 是 str
    if isinstance(b, int):
        return chr(b) if b != 0 else '0'
    else:
        return b if b != '\x00' else '0'


def export_orders(filepath, symbol, output_path):
    """导出 order 数据为 protobuf text 格式"""
    count = 0
    target = symbol.encode('utf-8') if isinstance(symbol, str) else symbol

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        version = detect_version(header['magic'])

        if version == 0:
            print("Unknown order file format (magic: 0x%08X)" % header['magic'])
            mm.close()
            return 0

        print("Order file format: V%d (magic: 0x%08X)" % (version, header['magic']))

        record_size = SIZE_ORDER_V2 if version == 2 else SIZE_ORDER_V1

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * record_size
                mm.seek(offset)
                data = mm.read(record_size)

                if version == 2:
                    # V2: local_recv_timestamp 在 offset 0, symbol 在 offset 88
                    local_recv_ts = struct.unpack('<q', data[0:8])[0]
                    orderindex = struct.unpack('<q', data[8:16])[0]
                    orderprice = struct.unpack('<q', data[16:24])[0]
                    orderqty = struct.unpack('<q', data[24:32])[0]
                    orderno = struct.unpack('<q', data[32:40])[0]
                    applseqnum = struct.unpack('<q', data[48:56])[0]
                    mddate = struct.unpack('<i', data[56:60])[0]
                    mdtime = struct.unpack('<i', data[60:64])[0]
                    ordertype = struct.unpack('<i', data[72:76])[0]
                    orderbsflag = struct.unpack('<i', data[76:80])[0]
                    channelno = struct.unpack('<i', data[80:84])[0]
                    sym = data[88:128].split(b'\x00')[0]
                else:
                    # V1: symbol 在 offset 0
                    local_recv_ts = 0
                    sym = data[:40].split(b'\x00')[0]
                    mddate, mdtime = struct.unpack('<ii', data[40:48])
                    orderindex = struct.unpack('<q', data[56:64])[0]
                    ordertype = struct.unpack('<i', data[64:68])[0]
                    orderprice = struct.unpack('<q', data[72:80])[0]
                    orderqty = struct.unpack('<q', data[80:88])[0]
                    orderbsflag = struct.unpack('<i', data[88:92])[0]
                    channelno = struct.unpack('<i', data[92:96])[0]
                    orderno = struct.unpack('<q', data[96:104])[0]
                    applseqnum = struct.unpack('<q', data[112:120])[0]

                if target not in sym:
                    continue

                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                exchange = 'XSHG' if sym_str.startswith('6') else 'XSHE'

                line = 'HTSCSecurityID: "{}" MDDate: {} MDTime: {} securityIDSource: {} securityType: StockType '.format(
                    sym_str, mddate, mdtime, exchange)
                line += 'OrderIndex: {} '.format(orderindex)
                if orderno > 0:
                    line += 'OrderNO: {} '.format(orderno)
                line += 'OrderType: {} OrderPrice: {} OrderQty: {} OrderBSFlag: {} '.format(
                    ordertype, orderprice, orderqty, orderbsflag)
                line += 'ChannelNo: {} ApplSeqNum: {} DataMultiplePowerOf10: 4'.format(
                    channelno, applseqnum)
                if local_recv_ts > 0:
                    line += ' LocalRecvTimestamp: {}'.format(local_recv_ts)

                out.write(line + '\n')
                count += 1

        mm.close()
    return count


def export_transactions(filepath, symbol, output_path):
    """导出 transaction 数据为 protobuf text 格式"""
    count = 0
    target = symbol.encode('utf-8') if isinstance(symbol, str) else symbol

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        version = detect_version(header['magic'])

        if version == 0:
            print("Unknown transaction file format (magic: 0x%08X)" % header['magic'])
            mm.close()
            return 0

        print("Transaction file format: V%d (magic: 0x%08X)" % (version, header['magic']))

        record_size = SIZE_TRANSACTION_V2 if version == 2 else SIZE_TRANSACTION_V1

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * record_size
                mm.seek(offset)
                data = mm.read(record_size)

                if version == 2:
                    # V2: local_recv_timestamp 在 offset 0, symbol 在 offset 96
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
                    channelno = struct.unpack('<i', data[88:92])[0]
                    sym = data[96:136].split(b'\x00')[0]
                else:
                    # V1: symbol 在 offset 0
                    local_recv_ts = 0
                    sym = data[:40].split(b'\x00')[0]
                    mddate, mdtime = struct.unpack('<ii', data[40:48])
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

                if target not in sym:
                    continue

                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                exchange = 'XSHG' if sym_str.startswith('6') else 'XSHE'

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
                if local_recv_ts > 0:
                    line += ' LocalRecvTimestamp: {}'.format(local_recv_ts)

                out.write(line + '\n')
                count += 1

        mm.close()
    return count


def export_ticks(filepath, symbol, output_path):
    """导出 tick 数据为 protobuf text 格式"""
    count = 0
    target = symbol.encode('utf-8') if isinstance(symbol, str) else symbol

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)
        version = detect_version(header['magic'])

        if version == 0:
            print("Unknown tick file format (magic: 0x%08X)" % header['magic'])
            mm.close()
            return 0

        print("Tick file format: V%d (magic: 0x%08X)" % (version, header['magic']))

        record_size = SIZE_TICK_V2 if version == 2 else SIZE_TICK_V1

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * record_size
                mm.seek(offset)
                data = mm.read(record_size)

                if version == 2:
                    # V2: local_recv_timestamp 在 offset 0, symbol 在 offset 2168
                    local_recv_ts = struct.unpack('<q', data[0:8])[0]
                    datatimestamp = struct.unpack('<q', data[8:16])[0]
                    maxpx = struct.unpack('<q', data[16:24])[0]
                    minpx = struct.unpack('<q', data[24:32])[0]
                    preclosepx = struct.unpack('<q', data[32:40])[0]
                    lastpx = struct.unpack('<q', data[64:72])[0]
                    openpx = struct.unpack('<q', data[72:80])[0]
                    highpx = struct.unpack('<q', data[88:96])[0]
                    lowpx = struct.unpack('<q', data[96:104])[0]
                    mddate = struct.unpack('<i', data[2120:2124])[0]
                    mdtime = struct.unpack('<i', data[2124:2128])[0]
                    channelno = struct.unpack('<i', data[2144:2148])[0]
                    sym = data[2168:2208].split(b'\x00')[0]
                    phase_byte = data[2208:2209]
                    tradingphasecode = get_phase_code(phase_byte)

                    # 价格队列 (V2 offset: 200)
                    buy_prices = struct.unpack('<10q', data[200:280])
                    buy_qtys = struct.unpack('<10q', data[280:360])
                    sell_prices = struct.unpack('<10q', data[360:440])
                    sell_qtys = struct.unpack('<10q', data[440:520])
                else:
                    # V1: symbol 在 offset 0
                    local_recv_ts = 0
                    sym = data[:40].split(b'\x00')[0]
                    mddate, mdtime = struct.unpack('<ii', data[40:48])
                    phase_byte = data[56:57]
                    tradingphasecode = get_phase_code(phase_byte)
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

                if target not in sym:
                    continue

                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                exchange = 'XSHG' if sym_str.startswith('6') else 'XSHE'

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
                if local_recv_ts > 0:
                    line += ' LocalRecvTimestamp: {}'.format(local_recv_ts)

                out.write(line + '\n')
                count += 1

        mm.close()
    return count


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python export_mmap_v2.py <data_dir> <symbol> <output_dir>")
        sys.exit(1)

    data_dir = sys.argv[1]
    symbol = sys.argv[2]
    output_dir = sys.argv[3]

    symbol_base = symbol.split('.')[0]

    # 导出 orders
    orders_file = os.path.join(data_dir, 'orders.bin')
    if os.path.exists(orders_file):
        output_path = os.path.join(output_dir, 'MD_ORDER_StockType_{}.csv'.format(symbol))
        count = export_orders(orders_file, symbol_base, output_path)
        print("Exported {} order records to {}".format(count, output_path))

    # 导出 transactions
    txn_file = os.path.join(data_dir, 'transactions.bin')
    if os.path.exists(txn_file):
        output_path = os.path.join(output_dir, 'MD_TRANSACTION_StockType_{}.csv'.format(symbol))
        count = export_transactions(txn_file, symbol_base, output_path)
        print("Exported {} transaction records to {}".format(count, output_path))

    # 导出 ticks
    ticks_file = os.path.join(data_dir, 'ticks.bin')
    if os.path.exists(ticks_file):
        output_path = os.path.join(output_dir, 'MD_TICK_StockType_{}.csv'.format(symbol))
        count = export_ticks(ticks_file, symbol_base, output_path)
        print("Exported {} tick records to {}".format(count, output_path))
'''


def get_date_path(date_str=None):
    """获取日期路径 YYYY/MM/DD"""
    if date_str:
        d = datetime.strptime(date_str, '%Y%m%d')
    else:
        d = datetime.now()
    return d.strftime('%Y/%m/%d')


def run_cmd(cmd, check=True):
    """运行命令并返回输出"""
    print(f"  > {cmd}")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and result.returncode != 0:
        print(f"Error: {result.stderr}")
        sys.exit(1)
    return result


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    symbol = sys.argv[1]
    date_str = sys.argv[2] if len(sys.argv) > 2 else None

    # 确保 symbol 有后缀
    if '.' not in symbol:
        if symbol.startswith('6'):
            symbol = f"{symbol}.SH"
        else:
            symbol = f"{symbol}.SZ"

    date_path = get_date_path(date_str)
    remote_data_dir = f"{REMOTE_DATA_BASE}/{date_path}"

    print(f"下载 {symbol} 的市场数据 (支持 V1/V2 格式)")
    print(f"  服务器: {SSH_HOST}")
    print(f"  数据目录: {remote_data_dir}")
    print()

    # 1. 检查远程数据目录是否存在
    print("1. 检查远程数据目录...")
    result = run_cmd(f'ssh {SSH_HOST} "ls -la {remote_data_dir}/*.bin 2>/dev/null || echo NO_DATA"', check=False)
    if 'NO_DATA' in result.stdout or result.returncode != 0:
        print(f"错误: 远程目录 {remote_data_dir} 不存在或没有数据文件")
        sys.exit(1)
    print(result.stdout)

    # 2. 上传导出脚本到服务器
    print("2. 上传导出脚本到服务器...")
    with tempfile.NamedTemporaryFile(mode='w', suffix='.py', delete=False) as f:
        f.write(REMOTE_EXPORT_SCRIPT)
        local_script = f.name

    run_cmd(f'scp {local_script} {SSH_HOST}:/tmp/export_mmap_v2.py')
    os.unlink(local_script)

    # 3. 在服务器上运行导出脚本
    print(f"3. 在服务器上导出 {symbol} 数据...")
    remote_output_dir = f"/tmp/mmap_export_{symbol.replace('.', '_')}"
    run_cmd(f'ssh {SSH_HOST} "mkdir -p {remote_output_dir} && python /tmp/export_mmap_v2.py {remote_data_dir} {symbol} {remote_output_dir}"')

    # 4. 下载导出的文件
    print("4. 下载数据文件...")
    os.makedirs(LOCAL_TEST_DATA, exist_ok=True)

    for dtype in ['ORDER', 'TRANSACTION', 'TICK']:
        remote_file = f"{remote_output_dir}/MD_{dtype}_StockType_{symbol}.csv"
        local_file = f"{LOCAL_TEST_DATA}/MD_{dtype}_StockType_{symbol}.csv"
        result = run_cmd(f'scp {SSH_HOST}:{remote_file} {local_file}', check=False)
        if result.returncode == 0:
            # 统计行数
            wc_result = subprocess.run(f'wc -l {local_file}', shell=True, capture_output=True, text=True)
            lines = wc_result.stdout.split()[0] if wc_result.returncode == 0 else '?'
            print(f"  {dtype}: {lines} 条记录")
        else:
            print(f"  {dtype}: 无数据")

    # 5. 清理服务器临时文件
    print("5. 清理临时文件...")
    run_cmd(f'ssh {SSH_HOST} "rm -rf {remote_output_dir} /tmp/export_mmap_v2.py"', check=False)

    print()
    print(f"完成! 数据已保存到 {LOCAL_TEST_DATA}/")


if __name__ == '__main__':
    main()
