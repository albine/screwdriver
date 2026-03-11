#!/usr/bin/env python3
"""
多股票批量测试 LimitUpBreakSellStrategy 的开板检测。
验证封单量衰减模式：是否普遍呈现先快后慢的特征。

支持 SZ (深圳) 和 SH (上海) 两个市场:
- SZ: applseqnum 共享序列，撤单在 transaction (tradetype!=0)，order_id=orderindex
- SH: mdtime 排序，撤单在 order (ordertype=10)，order_id=orderno
"""

import csv
import os
import sys
from collections import deque
from typing import Optional


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
# LimitUpBidBook: 追踪涨停价买方挂单量
# ==========================================
class LimitUpBidBook:
    def __init__(self, limit_up_price: int, sh_market: bool = False):
        self.limit_up_price = limit_up_price
        self.sh_market = sh_market
        self.orders: dict[int, int] = {}  # order_id -> remaining_qty
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
            # SH 撤单: 移除该订单
            if order_id in self.orders:
                self.total_bid_volume -= self.orders[order_id]
                del self.orders[order_id]
        else:
            # 新限价买单
            self.orders[order_id] = orderqty
            self.total_bid_volume += orderqty

    def on_transaction(self, row: dict):
        tradetype = int(row['tradetype'])
        tradebuyno = int(row['tradebuyno'])
        tradeqty = int(row['tradeqty'])
        tradeprice = int(row['tradeprice'])

        if tradetype == 0:
            # 成交
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
            # SZ 撤单 (SH 撤单已在 on_order 处理)
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
            # SH 撤单: 加入 flow_window
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
            # SZ 撤单
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
# 加载与回放
# ==========================================
def load_csv(filepath: str) -> list[dict]:
    rows = []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows

def merge_events_sz(order_rows, txn_rows):
    """SZ: 按 applseqnum 合并排序"""
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
    """SH: 按 mdtime 排序，同时间 order 在前"""
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
    """
    找到所有连续封板段→开板的转换。
    只返回封板时长 >= min_seal_seconds 的段。
    返回 [(seal_start_time, break_time, duration_ms), ...]
    """
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


def analyze_stock(symbol: str, date: str, data_dir: str):
    """分析单只股票的涨停开板模式"""
    sh = is_sh(symbol)
    suffix = f"_{date}" if date != "20260310" or symbol != "000533.SZ" else ""

    tick_file = os.path.join(data_dir, f"MD_TICK_StockType_{symbol}{suffix}.csv")
    order_file = os.path.join(data_dir, f"MD_ORDER_StockType_{symbol}{suffix}.csv")
    txn_file = os.path.join(data_dir, f"MD_TRANSACTION_StockType_{symbol}{suffix}.csv")

    for f in [tick_file, order_file, txn_file]:
        if not os.path.exists(f):
            print(f"  [SKIP] 文件不存在: {f}")
            return None

    print(f"\n{'='*70}")
    print(f"  {symbol} | {date} | {'上海' if sh else '深圳'}")
    print(f"{'='*70}")

    # 加载 tick
    ticks = load_csv(tick_file)

    # 获取涨停价
    limit_up_price = 0
    for t in ticks:
        mp = int(t['maxpx'])
        if mp > 0:
            limit_up_price = mp
            break
    if limit_up_price == 0:
        print("  [SKIP] 无涨停价")
        return None
    print(f"  涨停价: {price_str(limit_up_price)}")

    # 找所有封板段
    seal_breaks = find_all_seal_breaks(ticks, limit_up_price, min_seal_seconds=60)

    if not seal_breaks:
        print("  [SKIP] 无持续≥60s的封板段")
        return None
    print(f"  发现 {len(seal_breaks)} 个封板段:")
    for i, (ss, bt, dur) in enumerate(seal_breaks):
        print(f"    [{i}] {format_mdtime(ss)} ~ {format_mdtime(bt)} ({dur/1000:.0f}s)")

    # 加载 order + transaction
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

        # 创建 book 和 strategy
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

        # 10ms 封单量衰减 (只打印 T+0 ~ T+1000)
        book2 = LimitUpBidBook(limit_up_price, sh_market=sh)
        window_start_ms = trigger_ms
        window_end_ms = trigger_ms + 1000
        next_print_ms = window_start_ms
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

        # 衰减速度 (每100ms)
        trigger_vol = None
        prev_vol = None
        decay_str_parts = []
        for offset, vol in bid_vol_series:
            if offset == 0:
                trigger_vol = vol
            if offset >= 0 and offset % 100 == 0:
                if prev_vol is not None and trigger_vol and trigger_vol > 0:
                    drop = prev_vol - vol
                    drop_pct = drop * 100.0 / trigger_vol
                    decay_str_parts.append(f"T{offset-100:+d}~{offset:+d}: {drop_pct:+.1f}%")
                prev_vol = vol
        print(f"    衰减: {' | '.join(decay_str_parts)}")

        # 触发时/100ms/300ms/500ms/1000ms 的封单量占比
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
            'limit_up_price': limit_up_price,
            'trigger_time': strategy.trigger_time,
            'real_break_time': real_break_time,
            'early_ms': diff_ms,
            'bid_vol_series': bid_vol_series,
        })

    return results


def main():
    data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test_data")

    # 测试股票列表
    stocks = [
        ("000533.SZ", "20260310"),
        ("002082.SZ", "20260309"),
        ("600989.SH", "20260309"),
        ("600227.SH", "20260309"),
        ("600545.SH", "20260306"),
    ]

    all_results = []
    for symbol, date in stocks:
        stock_results = analyze_stock(symbol, date, data_dir)
        if stock_results:
            all_results.extend(stock_results)

    # ==========================================
    # 汇总
    # ==========================================
    print(f"\n{'='*70}")
    print(f"  汇总: {len(all_results)} 个封板段完成分析")
    print(f"{'='*70}")
    print(f"  {'股票':>12} | {'封板起始':>15} | {'触发时间':>15} | {'开板时间':>15} | {'提前ms':>8}")
    print(f"  {'-'*12}-+-{'-'*15}-+-{'-'*15}-+-{'-'*15}-+-{'-'*8}")
    for r in all_results:
        print(f"  {r['symbol']:>12} | {format_mdtime(r['seal_start']):>15} | "
              f"{format_mdtime(r['trigger_time']):>15} | {format_mdtime(r['real_break_time']):>15} | "
              f"{r['early_ms']:>8}")

    # 衰减模式对比
    print(f"\n  --- 衰减模式对比 (封单量占触发时的百分比) ---")
    check_offsets = [0, 100, 200, 300, 500, 800, 1000]
    header = f"  {'偏移':>10}"
    for r in all_results:
        label = f"{r['symbol']}[{r['seg']}]"
        header += f" | {label:>14}"
    print(header)

    for target_offset in check_offsets:
        line = f"  T{target_offset:+5d}ms   "
        for r in all_results:
            trigger_vol = None
            target_vol = None
            for off, vol in r['bid_vol_series']:
                if off == 0:
                    trigger_vol = vol
                if off == target_offset:
                    target_vol = vol
                    break
            if trigger_vol and trigger_vol > 0 and target_vol is not None:
                pct = target_vol * 100.0 / trigger_vol
                line += f" | {pct:>12.1f}%"
            else:
                line += f" | {'N/A':>13}"
        print(line)


if __name__ == "__main__":
    main()
