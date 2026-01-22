#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从服务器下载 mmap 市场数据

用法:
    python script/download_mmap_data.py <symbol> [date]

示例:
    python script/download_mmap_data.py 600759.SH          # 今天
    python script/download_mmap_data.py 600759.SH 20260120 # 指定日期
"""

import os
import sys
import subprocess
import tempfile
from datetime import datetime

# SSH 配置
SSH_HOST = "market-m"
REMOTE_DATA_BASE = "/home/jiace/project/trading-engine/data/raw"
LOCAL_TEST_DATA = "test_data"

# 远程导出脚本（会在服务器上临时创建）
REMOTE_EXPORT_SCRIPT = '''#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""临时导出脚本 - 从 mmap 文件导出为 protobuf text 格式"""
from __future__ import print_function
import sys
import struct
import mmap
import os

HEADER_SIZE = 64
SIZE_ORDER = 144
SIZE_TRANSACTION = 128
SIZE_TICK = 2216

def read_header(mm):
    mm.seek(0)
    data = mm.read(HEADER_SIZE)
    magic, version, struct_size, record_count, write_offset = struct.unpack('<IHHQQ', data[:24])
    return {'record_count': record_count, 'struct_size': struct_size}

def export_orders(filepath, symbol, output_path):
    """导出 order 数据为 protobuf text 格式"""
    count = 0
    target = symbol.encode('utf-8') if isinstance(symbol, str) else symbol

    with open(filepath, 'rb') as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        header = read_header(mm)

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * SIZE_ORDER
                mm.seek(offset)
                data = mm.read(SIZE_ORDER)

                sym = data[:40].rstrip(b'\\x00')
                if target not in sym:
                    continue

                mddate, mdtime = struct.unpack('<ii', data[40:48])
                # securityidsource at 48-52, securitytype at 52-56
                orderindex = struct.unpack('<q', data[56:64])[0]
                ordertype = struct.unpack('<i', data[64:68])[0]
                # padding at 68-72
                orderprice = struct.unpack('<q', data[72:80])[0]
                orderqty = struct.unpack('<q', data[80:88])[0]
                orderbsflag = struct.unpack('<i', data[88:92])[0]
                channelno = struct.unpack('<i', data[92:96])[0]
                orderno = struct.unpack('<q', data[96:104])[0]
                # tradedqty at 104-112
                applseqnum = struct.unpack('<q', data[112:120])[0]

                # 确定交易所 (使用 errors='ignore' 处理非 UTF-8 字节)
                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                if sym_str.startswith('6'):
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

                out.write(line + '\\n')
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

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * SIZE_TRANSACTION
                mm.seek(offset)
                data = mm.read(SIZE_TRANSACTION)

                sym = data[:40].rstrip(b'\\x00')
                if target not in sym:
                    continue

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

                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                if sym_str.startswith('6'):
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

                out.write(line + '\\n')
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

        with open(output_path, 'w') as out:
            for i in range(header['record_count']):
                offset = HEADER_SIZE + i * SIZE_TICK
                mm.seek(offset)
                data = mm.read(SIZE_TICK)

                sym = data[:40].rstrip(b'\\x00')
                if target not in sym:
                    continue

                mddate, mdtime = struct.unpack('<ii', data[40:48])
                # tradingphasecode at 56 (Python 2/3 compatible)
                phase_byte = data[56:57]
                tradingphasecode = phase_byte if isinstance(phase_byte, str) else chr(phase_byte[0]) if phase_byte[0] != 0 else '0'
                # maxpx at 72, minpx at 80, preclosepx at 88
                maxpx, minpx, preclosepx = struct.unpack('<qqq', data[72:96])
                # lastpx at 120, openpx at 128, closepx at 136, highpx at 144, lowpx at 152
                lastpx = struct.unpack('<q', data[120:128])[0]
                openpx = struct.unpack('<q', data[128:136])[0]
                highpx = struct.unpack('<q', data[144:152])[0]
                lowpx = struct.unpack('<q', data[152:160])[0]
                # channelno at 264
                channelno = struct.unpack('<i', data[264:268])[0]

                # Buy/Sell price queues start at offset 272
                # Each queue has 10 int64 values = 80 bytes
                buy_prices = struct.unpack('<10q', data[272:352])
                buy_qtys = struct.unpack('<10q', data[352:432])
                sell_prices = struct.unpack('<10q', data[432:512])
                sell_qtys = struct.unpack('<10q', data[512:592])

                sym_str = sym.decode('utf-8', errors='ignore') if isinstance(sym, bytes) else sym
                if sym_str.startswith('6'):
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

                # 价格队列 (10 levels)
                for p in buy_prices:
                    line += 'BuyPriceQueue: {} '.format(p)
                for q in buy_qtys:
                    line += 'BuyOrderQtyQueue: {} '.format(q)
                for p in sell_prices:
                    line += 'SellPriceQueue: {} '.format(p)
                for q in sell_qtys:
                    line += 'SellOrderQtyQueue: {} '.format(q)
                # BuyNumOrdersQueue/SellNumOrdersQueue 暂不导出
                for _ in range(10):
                    line += 'BuyNumOrdersQueue: 0 '
                for _ in range(10):
                    line += 'SellNumOrdersQueue: 0 '

                line += 'DataMultiplePowerOf10: 4'
                out.write(line + '\\n')
                count += 1

        mm.close()
    return count

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python export_mmap.py <data_dir> <symbol> <output_dir>")
        sys.exit(1)

    data_dir = sys.argv[1]
    symbol = sys.argv[2]
    output_dir = sys.argv[3]

    # 获取不带后缀的代码用于文件匹配
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

    print(f"下载 {symbol} 的市场数据")
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

    run_cmd(f'scp {local_script} {SSH_HOST}:/tmp/export_mmap.py')
    os.unlink(local_script)

    # 3. 在服务器上运行导出脚本
    print(f"3. 在服务器上导出 {symbol} 数据...")
    remote_output_dir = f"/tmp/mmap_export_{symbol.replace('.', '_')}"
    run_cmd(f'ssh {SSH_HOST} "mkdir -p {remote_output_dir} && python /tmp/export_mmap.py {remote_data_dir} {symbol} {remote_output_dir}"')

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
            print(f"  ✓ {dtype}: {lines} 条记录")
        else:
            print(f"  ✗ {dtype}: 无数据")

    # 5. 清理服务器临时文件
    print("5. 清理临时文件...")
    run_cmd(f'ssh {SSH_HOST} "rm -rf {remote_output_dir} /tmp/export_mmap.py"', check=False)

    print()
    print(f"完成! 数据已保存到 {LOCAL_TEST_DATA}/")


if __name__ == '__main__':
    main()
