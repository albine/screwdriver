#!/usr/bin/env python3
"""
自动化涨停开板策略测试:
1. SSH 连接 market-m 的 ClickHouse，筛选指定日期有涨停开板的股票
2. 下载 tick/order/transaction 数据到 test_data/
3. 对每只股票的每个封板段运行 LimitUpBreakSellStrategy 模拟
4. 输出衰减模式汇总

用法:
  python test/test_limit_up_break_auto.py 20260309
  python test/test_limit_up_break_auto.py 20260309 20260310
  python test/test_limit_up_break_auto.py 20260309 --min-seal 120   # 最短封板120秒
  python test/test_limit_up_break_auto.py 20260309 --max-stocks 10  # 最多10只
"""

import argparse
import csv
import os
import subprocess
import sys
from collections import deque
from typing import Optional

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(PROJECT_DIR, "test_data")

# ==========================================
# SSH / ClickHouse
# ==========================================
def find_ssh_agent():
    """查找可用的 SSH agent socket"""
    import glob
    sock = os.environ.get("SSH_AUTH_SOCK")
    if sock and os.path.exists(sock):
        return sock
    for path in sorted(glob.glob("/tmp/ssh-*/agent.*"), reverse=True):
        result = subprocess.run(
            ["ssh-add", "-l"],
            env={**os.environ, "SSH_AUTH_SOCK": path},
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and "SHA256:" in result.stdout:
            return path
    return None


def run_ch_query(query: str, ssh_sock: str) -> str:
    """在 market-m 上执行 ClickHouse 查询，返回原始输出 (去除 NUL)"""
    env = {**os.environ, "SSH_AUTH_SOCK": ssh_sock}
    cmd = [
        "ssh", "-F", os.path.expanduser("~/.ssh/config"), "market-m",
        f'clickhouse-client --password=$CLICKHOUSE_PASSWORD -d default -q "{query}"'
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=120, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"ClickHouse query failed: {result.stderr.decode().strip()}")
    # ClickHouse TabSeparated escapes NUL as literal \0 (backslash+zero)
    return result.stdout.decode().replace('\\0', '').replace('\x00', '')


def download_table(table: str, symbol: str, date: str, ssh_sock: str, out_path: str):
    """从 ClickHouse 下载一张表的 CSV 数据，去除 NUL 字符"""
    symbol = clean_symbol(symbol)
    if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
        return  # 已存在且非空，跳过

    env = {**os.environ, "SSH_AUTH_SOCK": ssh_sock}
    query = f"SELECT * FROM {table} WHERE mddate={date} AND htscsecurityid='{symbol}' FORMAT CSVWithNames"
    cmd = [
        "ssh", "-F", os.path.expanduser("~/.ssh/config"), "market-m",
        f'clickhouse-client --password=$CLICKHOUSE_PASSWORD -d default -q "{query}"'
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=300, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"Download failed for {table}/{symbol}: {result.stderr.decode().strip()}")

    # 去除 FixedString 的 NUL 填充
    data = result.stdout.replace(b'\x00', b'')
    with open(out_path, 'wb') as f:
        f.write(data)


# ==========================================
# Step 1: 从 ClickHouse 筛选涨停开板股票
# ==========================================
def find_limit_up_break_stocks(date: str, ssh_sock: str, min_seal_sec: int = 60) -> list[dict]:
    """
    查询 ClickHouse 找出指定日期有涨停后开板的股票。
    返回 [{symbol, max_px, first_limit_time, break_time}, ...]
    """
    query = (
        "SELECT "
        "  trimBoth(htscsecurityid) as sid, "
        "  max(maxpx) as max_px, "
        "  minIf(mdtime, lastpx >= maxpx AND mdtime >= 93000000) as first_limit_time, "
        "  minIf(mdtime, lastpx < maxpx AND mdtime > 93100000) as break_time "
        "FROM MDStockStruct "
        f"WHERE mddate = {date} "
        "  AND maxpx > 0 AND lastpx > 0 "
        "GROUP BY htscsecurityid "
        "HAVING break_time > 93100000 AND break_time < 150000000 "
        "  AND first_limit_time > 0 AND first_limit_time < break_time "
        f"  AND (break_time - first_limit_time) > {min_seal_sec * 1000} "
        "ORDER BY break_time "
        "FORMAT TabSeparatedWithNames"
    )
    output = run_ch_query(query, ssh_sock)
    lines = output.strip().split('\n')
    if len(lines) < 2:
        return []

    header = lines[0].split('\t')
    results = []
    for line in lines[1:]:
        fields = line.split('\t')
        row = dict(zip(header, fields))
        sid = clean_symbol(row['sid'])
        results.append({
            'symbol': sid,
            'max_px': int(row['max_px']),
            'first_limit_time': int(row['first_limit_time']),
            'break_time': int(row['break_time']),
        })
    return results


def verify_data_completeness(symbol: str, date: str, ssh_sock: str) -> bool:
    """检查 order 和 transaction 数据是否覆盖全天"""
    symbol = clean_symbol(symbol)
    for table in ["MDOrderStruct", "MDTransactionStruct"]:
        query = (
            f"SELECT count() as cnt, max(mdtime) as max_t "
            f"FROM {table} "
            f"WHERE mddate={date} AND htscsecurityid='{symbol}' "
            "FORMAT TabSeparatedWithNames"
        )
        try:
            output = run_ch_query(query, ssh_sock)
            lines = output.strip().split('\n')
            if len(lines) < 2:
                return False
            fields = lines[1].split('\t')
            cnt = int(fields[0])
            max_t = int(fields[1])
            if cnt < 100 or max_t < 145000000:
                return False
        except Exception:
            return False
    return True


# ==========================================
# Step 2: 下载数据
# ==========================================
def download_stock_data(symbol: str, date: str, ssh_sock: str):
    """下载某只股票某天的 tick/order/transaction 到 test_data/"""
    symbol = clean_symbol(symbol)
    os.makedirs(DATA_DIR, exist_ok=True)

    table_prefix_map = {
        "MDStockStruct": "MD_TICK_StockType",
        "MDOrderStruct": "MD_ORDER_StockType",
        "MDTransactionStruct": "MD_TRANSACTION_StockType",
    }
    for table, prefix in table_prefix_map.items():
        out_path = os.path.join(DATA_DIR, f"{prefix}_{symbol}_{date}.csv")
        download_table(table, symbol, date, ssh_sock, out_path)


# ==========================================
# 工具函数
# ==========================================
def format_mdtime(mdtime: int) -> str:
    h = mdtime // 10000000
    m = (mdtime // 100000) % 100
    s = (mdtime // 1000) % 100
    ms = mdtime % 1000
    return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"

def mdtime_to_ms(mdtime: int) -> int:
    h = mdtime // 10000000
    m = (mdtime // 100000) % 100
    s = (mdtime // 1000) % 100
    ms = mdtime % 1000
    return h * 3600000 + m * 60000 + s * 1000 + ms

def price_str(price: int) -> str:
    return f"{price/10000:.2f}"

def is_sh(symbol: str) -> bool:
    return symbol.endswith('.SH')


# ==========================================
# LimitUpBidBook
# ==========================================
class LimitUpBidBook:
    def __init__(self, limit_up_price: int, sh_market: bool = False):
        self.limit_up_price = limit_up_price
        self.sh_market = sh_market
        self.orders: dict[int, int] = {}
        self.total_bid_volume = 0

    def on_order(self, row: dict):
        orderprice = int(row['orderprice'])
        orderbsflag = int(row['orderbsflag'])
        orderno = int(row['orderno'])
        orderindex = int(row['orderindex'])
        orderqty = int(row['orderqty'])
        ordertype = int(row['ordertype'])

        if orderbsflag != 1 or orderprice != self.limit_up_price:
            return

        order_id = orderno if orderno != 0 else orderindex

        if self.sh_market and ordertype == 10:
            if order_id in self.orders:
                self.total_bid_volume -= self.orders[order_id]
                del self.orders[order_id]
        else:
            self.orders[order_id] = orderqty
            self.total_bid_volume += orderqty

    def on_transaction(self, row: dict):
        tradetype = int(row['tradetype'])
        tradebuyno = int(row['tradebuyno'])
        tradeqty = int(row['tradeqty'])
        tradeprice = int(row['tradeprice'])

        if tradetype == 0:
            if tradeprice != self.limit_up_price:
                return
            if tradebuyno not in self.orders:
                return
            old = self.orders[tradebuyno]
            consumed = min(tradeqty, old)
            self.total_bid_volume -= consumed
            if consumed >= old:
                del self.orders[tradebuyno]
            else:
                self.orders[tradebuyno] -= consumed
        else:
            if self.sh_market:
                return
            if tradebuyno not in self.orders:
                return
            old = self.orders[tradebuyno]
            consumed = min(tradeqty, old)
            self.total_bid_volume -= consumed
            if consumed >= old:
                del self.orders[tradebuyno]
            else:
                self.orders[tradebuyno] -= consumed


# ==========================================
# LimitUpBreakSellStrategy
# ==========================================
class LimitUpBreakSellStrategy:
    INACTIVE = "INACTIVE"
    MONITORING = "MONITORING"
    TRIGGERED = "TRIGGERED"

    def __init__(self, limit_up_price: int, sh_market: bool = False):
        self.state = self.INACTIVE
        self.limit_up_price = limit_up_price
        self.sh_market = sh_market
        self.limit_up_bid_orders: dict[int, int] = {}
        self.flow_window: deque = deque()
        self.flow_event_count = 0
        self.trigger_time: Optional[int] = None
        self.trigger_reason: str = ""

    def enter_monitoring(self, mdtime: int):
        if self.state == self.INACTIVE:
            self.state = self.MONITORING

    def on_order(self, row: dict, book: LimitUpBidBook):
        if self.state != self.MONITORING:
            return
        orderprice = int(row['orderprice'])
        orderbsflag = int(row['orderbsflag'])
        orderno = int(row['orderno'])
        orderindex = int(row['orderindex'])
        orderqty = int(row['orderqty'])
        ordertype = int(row['ordertype'])
        mdtime = int(row['mdtime'])

        if orderbsflag != 1 or orderprice != self.limit_up_price:
            return
        order_id = orderno if orderno != 0 else orderindex

        if self.sh_market and ordertype == 10:
            if order_id in self.limit_up_bid_orders:
                cancel_vol = self.limit_up_bid_orders[order_id]
                self.flow_window.append((mdtime, cancel_vol))
                self.flow_event_count += 1
                del self.limit_up_bid_orders[order_id]
                self._check_flow(mdtime, book)
        else:
            self.limit_up_bid_orders[order_id] = orderqty

    def on_transaction(self, row: dict, book: LimitUpBidBook):
        if self.state != self.MONITORING:
            return
        tradetype = int(row['tradetype'])
        tradebuyno = int(row['tradebuyno'])
        tradeqty = int(row['tradeqty'])
        tradeprice = int(row['tradeprice'])
        mdtime = int(row['mdtime'])

        if tradetype == 0:
            if tradeprice == self.limit_up_price:
                self.flow_window.append((mdtime, tradeqty))
                self.flow_event_count += 1
                if tradebuyno in self.limit_up_bid_orders:
                    old = self.limit_up_bid_orders[tradebuyno]
                    if tradeqty >= old:
                        del self.limit_up_bid_orders[tradebuyno]
                    else:
                        self.limit_up_bid_orders[tradebuyno] -= tradeqty
                self._check_flow(mdtime, book)
        else:
            if self.sh_market:
                return
            if tradebuyno in self.limit_up_bid_orders:
                cancel_vol = tradeqty
                self.flow_window.append((mdtime, cancel_vol))
                self.flow_event_count += 1
                old = self.limit_up_bid_orders[tradebuyno]
                if cancel_vol >= old:
                    del self.limit_up_bid_orders[tradebuyno]
                else:
                    self.limit_up_bid_orders[tradebuyno] -= cancel_vol
                self._check_flow(mdtime, book)

    def _check_flow(self, current_time: int, book: LimitUpBidBook):
        if self.state != self.MONITORING:
            return
        current_ms = mdtime_to_ms(current_time)
        while self.flow_window:
            front_ms = mdtime_to_ms(self.flow_window[0][0])
            if current_ms - front_ms <= 300:
                break
            self.flow_window.popleft()

        flow_sum = sum(vol for _, vol in self.flow_window)
        current_bid = book.total_bid_volume

        if current_bid == 0 or flow_sum * 10 > current_bid * 4:
            ratio = (flow_sum * 100.0 / current_bid) if current_bid > 0 else float('inf')
            self._emit_signal(current_time,
                f"flow_sum={flow_sum}, bid_vol={current_bid}, ratio={ratio:.1f}%, window={len(self.flow_window)}")

    def _emit_signal(self, mdtime: int, reason: str):
        if self.state == self.TRIGGERED:
            return
        self.state = self.TRIGGERED
        self.trigger_time = mdtime
        self.trigger_reason = reason


# ==========================================
# Step 3: 分析
# ==========================================
def clean_symbol(s: str) -> str:
    """去除 FixedString NUL 填充和空白 (ClickHouse 输出 literal \\0)"""
    return s.replace('\\0', '').replace('\x00', '').strip()


def load_csv(filepath: str) -> list[dict]:
    rows = []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows

def merge_events_sz(order_rows, txn_rows):
    events = []
    for r in order_rows:
        r['_type'] = 'order'
        r['_sort_key'] = int(r['applseqnum'])
        events.append(r)
    for r in txn_rows:
        r['_type'] = 'txn'
        r['_sort_key'] = int(r['applseqnum'])
        events.append(r)
    events.sort(key=lambda x: x['_sort_key'])
    return events

def merge_events_sh(order_rows, txn_rows):
    events = []
    for r in order_rows:
        r['_type'] = 'order'
        r['_sort_key'] = (int(r['mdtime']), 0, int(r['applseqnum']))
        events.append(r)
    for r in txn_rows:
        r['_type'] = 'txn'
        r['_sort_key'] = (int(r['mdtime']), 1, int(r['applseqnum']))
        events.append(r)
    events.sort(key=lambda x: x['_sort_key'])
    return events

def find_all_seal_breaks(ticks, limit_up_price, min_seal_seconds=60):
    seal_start = None
    results = []
    for t in ticks:
        mdtime = int(t['mdtime'])
        if mdtime < 93000000:
            continue
        bp = t['buypricequeue'].strip('"').strip('[]')
        bid1 = int(bp.split(',')[0]) if bp else 0
        if bid1 == limit_up_price:
            if seal_start is None:
                seal_start = mdtime
        else:
            if seal_start is not None:
                duration = mdtime_to_ms(mdtime) - mdtime_to_ms(seal_start)
                if duration >= min_seal_seconds * 1000:
                    results.append((seal_start, mdtime, duration))
                seal_start = None
    return results


def analyze_stock(symbol: str, date: str, min_seal_sec: int = 60) -> list[dict]:
    """分析单只股票的涨停开板模式，返回每个封板段的分析结果"""
    sh = is_sh(symbol)

    tick_file = os.path.join(DATA_DIR, f"MD_TICK_StockType_{symbol}_{date}.csv")
    order_file = os.path.join(DATA_DIR, f"MD_ORDER_StockType_{symbol}_{date}.csv")
    txn_file = os.path.join(DATA_DIR, f"MD_TRANSACTION_StockType_{symbol}_{date}.csv")

    for f in [tick_file, order_file, txn_file]:
        if not os.path.exists(f) or os.path.getsize(f) == 0:
            print(f"  [SKIP] 文件缺失或为空: {os.path.basename(f)}")
            return []

    print(f"\n{'='*70}")
    print(f"  {symbol} | {date} | {'上海' if sh else '深圳'}")
    print(f"{'='*70}")

    ticks = load_csv(tick_file)

    limit_up_price = 0
    for t in ticks:
        mp = int(t['maxpx'])
        if mp > 0:
            limit_up_price = mp
            break
    if limit_up_price == 0:
        print("  [SKIP] 无涨停价")
        return []
    print(f"  涨停价: {price_str(limit_up_price)}")

    seal_breaks = find_all_seal_breaks(ticks, limit_up_price, min_seal_seconds=min_seal_sec)
    if not seal_breaks:
        print(f"  [SKIP] 无持续≥{min_seal_sec}s 的封板段")
        return []
    print(f"  发现 {len(seal_breaks)} 个封板段:")
    for i, (ss, bt, dur) in enumerate(seal_breaks):
        print(f"    [{i}] {format_mdtime(ss)} ~ {format_mdtime(bt)} ({dur/1000:.0f}s)")

    order_rows = load_csv(order_file)
    txn_rows = load_csv(txn_file)

    if sh:
        all_events = merge_events_sh(order_rows, txn_rows)
    else:
        all_events = merge_events_sz(order_rows, txn_rows)
    print(f"  事件总数: {len(all_events)} (orders={len(order_rows)}, txns={len(txn_rows)})")

    results = []

    for seg_idx, (seal_start, real_break_time, seal_duration) in enumerate(seal_breaks):
        print(f"\n  --- 封板段 [{seg_idx}]: {format_mdtime(seal_start)} ~ "
              f"{format_mdtime(real_break_time)} ({seal_duration/1000:.0f}s) ---")

        book = LimitUpBidBook(limit_up_price, sh_market=sh)
        strategy = LimitUpBreakSellStrategy(limit_up_price, sh_market=sh)

        monitoring_entered = False
        for row in all_events:
            mdtime = int(row['mdtime'])

            if row['_type'] == 'order':
                book.on_order(row)
            else:
                book.on_transaction(row)

            if mdtime < seal_start:
                continue

            if not monitoring_entered and book.total_bid_volume > 0:
                monitoring_entered = True
                strategy.enter_monitoring(mdtime)

            if row['_type'] == 'order':
                strategy.on_order(row, book)
            else:
                strategy.on_transaction(row, book)

            if strategy.state == strategy.TRIGGERED:
                break

        if not strategy.trigger_time:
            print("    [WARN] 策略未触发")
            continue

        trigger_ms = mdtime_to_ms(strategy.trigger_time)
        break_ms = mdtime_to_ms(real_break_time)
        diff_ms = break_ms - trigger_ms
        print(f"    触发: {format_mdtime(strategy.trigger_time)} | "
              f"开板: {format_mdtime(real_break_time)} | 提前: {diff_ms}ms")
        print(f"    原因: {strategy.trigger_reason}")

        # 10ms 粒度封单量衰减
        book2 = LimitUpBidBook(limit_up_price, sh_market=sh)
        window_end_ms = trigger_ms + 1000
        next_print_ms = trigger_ms
        bid_vol_series = []

        for row in all_events:
            mdtime_val = int(row['mdtime'])
            cur_ms = mdtime_to_ms(mdtime_val)
            while cur_ms >= next_print_ms and next_print_ms <= window_end_ms:
                offset = next_print_ms - trigger_ms
                bid_vol_series.append((offset, book2.total_bid_volume))
                next_print_ms += 10
            if row['_type'] == 'order':
                book2.on_order(row)
            else:
                book2.on_transaction(row)
            if cur_ms > window_end_ms + 1000:
                break
        while next_print_ms <= window_end_ms:
            offset = next_print_ms - trigger_ms
            bid_vol_series.append((offset, book2.total_bid_volume))
            next_print_ms += 10

        # 衰减速度
        trigger_vol = None
        prev_vol = None
        decay_parts = []
        for offset, vol in bid_vol_series:
            if offset == 0:
                trigger_vol = vol
            if offset >= 0 and offset % 100 == 0:
                if prev_vol is not None and trigger_vol and trigger_vol > 0:
                    drop_pct = (prev_vol - vol) * 100.0 / trigger_vol
                    decay_parts.append(f"T{offset-100:+d}~{offset:+d}: {drop_pct:+.1f}%")
                prev_vol = vol
        print(f"    衰减: {' | '.join(decay_parts)}")

        # 关键时间点占比
        pct_points = []
        for target in [0, 100, 200, 300, 500, 800, 1000]:
            for off, vol in bid_vol_series:
                if off == target:
                    if trigger_vol and trigger_vol > 0:
                        pct_points.append(f"T{target:+d}={vol*100.0/trigger_vol:.0f}%")
                    break
        print(f"    占比: {' | '.join(pct_points)}")

        results.append({
            'symbol': symbol,
            'date': date,
            'market': 'SH' if sh else 'SZ',
            'seg': seg_idx,
            'seal_start': seal_start,
            'seal_duration_s': seal_duration / 1000,
            'limit_up_price': limit_up_price,
            'trigger_time': strategy.trigger_time,
            'trigger_vol': trigger_vol,
            'real_break_time': real_break_time,
            'early_ms': diff_ms,
            'bid_vol_series': bid_vol_series,
        })

    return results


# ==========================================
# 汇总输出
# ==========================================
def print_summary(all_results: list[dict]):
    if not all_results:
        print("\n  没有可分析的封板段。")
        return

    print(f"\n{'='*90}")
    print(f"  汇总: {len(all_results)} 个封板段")
    print(f"{'='*90}")
    print(f"  {'股票':>12} | {'日期':>10} | {'封板':>7} | {'触发时间':>15} | {'开板时间':>15} | {'提前ms':>8} | {'T+100':>6} | {'T+300':>6} | {'T+1s':>6}")
    print(f"  {'-'*12}-+-{'-'*10}-+-{'-'*7}-+-{'-'*15}-+-{'-'*15}-+-{'-'*8}-+-{'-'*6}-+-{'-'*6}-+-{'-'*6}")

    for r in all_results:
        # 计算关键占比
        trigger_vol = r.get('trigger_vol', 0)
        pcts = {}
        if trigger_vol and trigger_vol > 0:
            for off, vol in r['bid_vol_series']:
                if off in (100, 300, 1000):
                    pcts[off] = vol * 100.0 / trigger_vol

        p100 = f"{pcts.get(100, 0):.0f}%" if 100 in pcts else "N/A"
        p300 = f"{pcts.get(300, 0):.0f}%" if 300 in pcts else "N/A"
        p1000 = f"{pcts.get(1000, 0):.0f}%" if 1000 in pcts else "N/A"

        print(f"  {r['symbol']:>12} | {r['date']:>10} | "
              f"{r['seal_duration_s']:>5.0f}s | "
              f"{format_mdtime(r['trigger_time']):>15} | "
              f"{format_mdtime(r['real_break_time']):>15} | "
              f"{r['early_ms']:>8} | "
              f"{p100:>6} | {p300:>6} | {p1000:>6}")


# ==========================================
# main
# ==========================================
def main():
    parser = argparse.ArgumentParser(description="涨停开板策略自动化测试")
    parser.add_argument("dates", nargs="+", help="交易日期 (YYYYMMDD)")
    parser.add_argument("--min-seal", type=int, default=60, help="最短封板时间(秒), 默认60")
    parser.add_argument("--max-stocks", type=int, default=20, help="每个日期最多分析股票数, 默认20")
    parser.add_argument("--skip-download", action="store_true", help="跳过下载，只用已有数据")
    args = parser.parse_args()

    # 查找 SSH agent
    print("=== Step 0: 查找 SSH agent ===")
    ssh_sock = find_ssh_agent()
    if not ssh_sock:
        print("  [ERROR] 找不到可用的 SSH agent，无法连接 market-m")
        sys.exit(1)
    print(f"  SSH agent: {ssh_sock}")

    all_results = []

    for date in args.dates:
        print(f"\n{'#'*70}")
        print(f"# 日期: {date}")
        print(f"{'#'*70}")

        # Step 1: 筛选股票
        print(f"\n=== Step 1: 从 ClickHouse 筛选涨停开板股票 (封板≥{args.min_seal}s) ===")
        try:
            candidates = find_limit_up_break_stocks(date, ssh_sock, min_seal_sec=args.min_seal)
        except Exception as e:
            print(f"  [ERROR] 查询失败: {e}")
            continue

        if not candidates:
            print(f"  {date} 无符合条件的股票")
            continue

        print(f"  找到 {len(candidates)} 只候选股票:")
        for c in candidates:
            print(f"    {c['symbol']} | maxpx={price_str(c['max_px'])} | "
                  f"first_limit={format_mdtime(c['first_limit_time'])} | "
                  f"break={format_mdtime(c['break_time'])}")

        # 验证数据完整性并限制数量
        print(f"\n=== Step 2: 验证数据完整性并下载 ===")
        valid_stocks = []
        for c in candidates:
            if len(valid_stocks) >= args.max_stocks:
                break
            symbol = c['symbol']
            print(f"  {symbol}: ", end="", flush=True)
            try:
                if verify_data_completeness(symbol, date, ssh_sock):
                    valid_stocks.append(symbol)
                    print("OK", end="")
                else:
                    print("数据不完整, 跳过")
                    continue
            except Exception as e:
                print(f"检查失败({e}), 跳过")
                continue

            # 下载
            if not args.skip_download:
                print(" -> 下载...", end="", flush=True)
                try:
                    download_stock_data(symbol, date, ssh_sock)
                    print(" 完成")
                except Exception as e:
                    print(f" 失败({e})")
                    valid_stocks.pop()
                    continue
            else:
                print()

        if not valid_stocks:
            print(f"  {date} 无可用股票")
            continue
        print(f"  共 {len(valid_stocks)} 只股票可分析")

        # Step 3: 分析
        print(f"\n=== Step 3: 策略模拟分析 ===")
        for symbol in valid_stocks:
            try:
                results = analyze_stock(symbol, date, min_seal_sec=args.min_seal)
                if results:
                    all_results.extend(results)
            except Exception as e:
                print(f"  [ERROR] {symbol} 分析失败: {e}")

    # 汇总
    print_summary(all_results)


if __name__ == "__main__":
    main()
