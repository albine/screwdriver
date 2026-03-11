#!/usr/bin/env python3
"""
测试 LimitUpBreakSellStrategy 的开板检测逻辑。

使用 000533.SZ 在 20260310 的真实逐笔数据。
逻辑完全复现 C++ 策略：
- FastOrderBook 由 on_order/on_transaction 驱动
- 策略用 book.get_bid_volume_at_price(limit_up_price) 获取封单量
- 200ms 滑动窗口追踪 撤单+成交 的流出量

这里用 Python 跑 **不依赖 FastOrderBook** 的测试：
- 用逐笔 order 数据自行追踪涨停价买单
- 按 applseqnum 顺序回放（SZ 市场 order+txn 共享序列）
- 比较策略触发时间 vs tick 中 bid1 从涨停价变化的时间
"""

import csv
import os
from collections import deque
from dataclasses import dataclass
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


# ==========================================
# 简化 FastOrderBook: 只追踪涨停价买委托量
# 用 SZ 逐笔 order/transaction 驱动（与 C++ FastOrderBook 相同的逻辑）
# ==========================================
class LimitUpBidBook:
    """
    追踪涨停价的买方总挂单量。
    等价于 FastOrderBook.get_bid_volume_at_price(limit_up_price)。

    SZ 市场逻辑 (securityidsource=102):
    - on_order: orderbsflag=1(买) 且 orderprice==limit_up_price => 记录新单
    - on_transaction tradetype=0(成交): 扣减 tradebuyno 对应的买单量
    - on_transaction tradetype!=0(撤单): 扣减 tradebuyno 对应的买单量
    """
    def __init__(self, limit_up_price: int):
        self.limit_up_price = limit_up_price
        self.orders: dict[int, int] = {}  # orderno -> remaining_qty
        self.total_bid_volume = 0

    def on_order(self, row: dict):
        orderprice = int(row['orderprice'])
        orderbsflag = int(row['orderbsflag'])
        orderno = int(row['orderno'])
        orderindex = int(row['orderindex'])
        orderqty = int(row['orderqty'])
        ordertype = int(row['ordertype'])

        # 只关心涨停价的买单
        if orderbsflag != 1 or orderprice != self.limit_up_price:
            return

        # SZ 用 orderindex，SH 用 orderno（与 FastOrderBook 一致）
        order_id = orderno if orderno != 0 else orderindex

        # 限价单 (ordertype=2 for SZ)
        self.orders[order_id] = orderqty
        self.total_bid_volume += orderqty

    def on_transaction(self, row: dict):
        tradetype = int(row['tradetype'])
        tradebuyno = int(row['tradebuyno'])
        tradeqty = int(row['tradeqty'])
        tradeprice = int(row['tradeprice'])

        if tradetype == 0:
            # 成交: 只有涨停价成交才影响涨停价买单
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
            # 撤单: tradebuyno 指向被撤的买单
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
# LimitUpBreakSellStrategy Python 复现
# ==========================================
class LimitUpBreakSellStrategy:
    INACTIVE = "INACTIVE"
    MONITORING = "MONITORING"
    TRIGGERED = "TRIGGERED"

    def __init__(self, limit_up_price: int, start_monitor_time: int = 93000000):
        self.state = self.INACTIVE
        self.limit_up_price = limit_up_price
        self.start_monitor_time = start_monitor_time

        # 涨停价买单追踪（策略层面，与 book 独立）
        self.limit_up_bid_orders: dict[int, int] = {}

        # 200ms 滑动窗口
        self.flow_window: deque = deque()
        self.flow_event_count = 0

        # 结果
        self.trigger_time: Optional[int] = None
        self.trigger_reason: str = ""

    def enter_monitoring(self, mdtime: int):
        """手动进入 MONITORING 状态（在 tick 确认涨停后调用）"""
        if self.state == self.INACTIVE:
            self.state = self.MONITORING
            print(f"  -> MONITORING at {format_mdtime(mdtime)}")

    def on_order(self, row: dict, book: LimitUpBidBook):
        if self.state != self.MONITORING:
            return

        orderprice = int(row['orderprice'])
        orderbsflag = int(row['orderbsflag'])
        orderno = int(row['orderno'])
        orderindex = int(row['orderindex'])
        orderqty = int(row['orderqty'])
        mdtime = int(row['mdtime'])

        if orderbsflag != 1 or orderprice != self.limit_up_price:
            return

        order_id = orderno if orderno != 0 else orderindex

        # 记录到策略的买单追踪
        self.limit_up_bid_orders[order_id] = orderqty
        # SZ 没有 ordertype=10 撤单，所以 on_order 不产生 flow event

    def on_transaction(self, row: dict, book: LimitUpBidBook):
        if self.state != self.MONITORING:
            return

        tradetype = int(row['tradetype'])
        tradebuyno = int(row['tradebuyno'])
        tradeqty = int(row['tradeqty'])
        tradeprice = int(row['tradeprice'])
        mdtime = int(row['mdtime'])

        if tradetype == 0:
            # 成交 at 涨停价
            if tradeprice == self.limit_up_price:
                self.flow_window.append((mdtime, tradeqty))
                self.flow_event_count += 1

                # 更新买单剩余量
                if tradebuyno in self.limit_up_bid_orders:
                    old = self.limit_up_bid_orders[tradebuyno]
                    if tradeqty >= old:
                        del self.limit_up_bid_orders[tradebuyno]
                    else:
                        self.limit_up_bid_orders[tradebuyno] -= tradeqty

                self._check_flow(mdtime, book)
        else:
            # SZ 撤单
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

        # 清理 > 300ms 的条目
        while self.flow_window:
            front_ms = mdtime_to_ms(self.flow_window[0][0])
            if current_ms - front_ms <= 300:
                break
            self.flow_window.popleft()

        flow_sum = sum(vol for _, vol in self.flow_window)
        current_bid = book.total_bid_volume

        # 触发条件: current_bid == 0 OR flow_sum * 10 > current_bid * 4
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
# 主测试
# ==========================================
def main():
    data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "test_data")
    symbol = "000533.SZ"

    tick_file = os.path.join(data_dir, f"MD_TICK_StockType_{symbol}.csv")
    order_file = os.path.join(data_dir, f"MD_ORDER_StockType_{symbol}.csv")
    txn_file = os.path.join(data_dir, f"MD_TRANSACTION_StockType_{symbol}.csv")

    print(f"=== LimitUpBreakSellStrategy Test ===")
    print(f"Symbol: {symbol}")
    print(f"Date: 20260310")
    print()

    # ==========================================
    # 加载 tick 数据
    # ==========================================
    print("Loading tick data...")
    ticks = []
    with open(tick_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            ticks.append(row)
    print(f"  Ticks: {len(ticks)}")

    # 获取涨停价
    limit_up_price = 0
    for t in ticks:
        mp = int(t['maxpx'])
        if mp > 0:
            limit_up_price = mp
            break
    print(f"  Limit-up price: {limit_up_price} ({price_str(limit_up_price)})")
    print()

    # ==========================================
    # Step 1: 真实开板时间 (from tick bid1)
    # ==========================================
    print("=== Step 1: 真实开板时间 (tick bid1 从涨停价变为非涨停价) ===")
    was_at_limit = False
    real_break_time = None
    for t in ticks:
        mdtime = int(t['mdtime'])
        if mdtime < 93000000:
            continue
        bp = t['buypricequeue'].strip('"').strip('[]')
        bid1 = int(bp.split(',')[0]) if bp else 0
        if bid1 == limit_up_price:
            was_at_limit = True
        elif was_at_limit and bid1 > 0 and bid1 < limit_up_price:
            real_break_time = mdtime
            break

    if real_break_time:
        print(f"  真实开板时间: {format_mdtime(real_break_time)}")
    else:
        print("  tick 中未检测到开板!")
    print()

    # 打印开板前后封单量变化
    print("=== 开板前后 tick 封单量变化 ===")
    if real_break_time:
        break_ms = mdtime_to_ms(real_break_time)
        for t in ticks:
            mdtime = int(t['mdtime'])
            ms = mdtime_to_ms(mdtime)
            if break_ms - 30000 <= ms <= break_ms + 10000:
                bp = t['buypricequeue'].strip('"').strip('[]')
                bq = t['buyorderqtyqueue'].strip('"').strip('[]')
                bid1 = int(bp.split(',')[0]) if bp else 0
                bid1_qty = int(bq.split(',')[0]) if bq else 0
                lastpx = int(t['lastpx'])
                tag = "<<< LIMIT UP" if bid1 == limit_up_price else "*** BREAK ***"
                print(f"  {format_mdtime(mdtime)} | bid1={bid1:>8}({price_str(bid1):>8}) | "
                      f"qty={bid1_qty:>12} | lastpx={lastpx:>8} | {tag}")
    print()

    # ==========================================
    # Step 2: 加载 order + transaction, 按 applseqnum 排序回放
    # ==========================================
    print("=== Step 2: 策略模拟 (order+transaction 按 applseqnum 排序) ===")

    # 加载 order
    print("Loading order data...")
    order_rows = []
    with open(order_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            row['_type'] = 'order'
            row['_seq'] = int(row['applseqnum'])
            order_rows.append(row)
    print(f"  Orders: {len(order_rows)}")

    # 加载 transaction
    print("Loading transaction data...")
    txn_rows = []
    with open(txn_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            row['_type'] = 'txn'
            row['_seq'] = int(row['applseqnum'])
            txn_rows.append(row)
    print(f"  Transactions: {len(txn_rows)}")

    # 合并并按 applseqnum 排序
    all_events = order_rows + txn_rows
    all_events.sort(key=lambda x: x['_seq'])
    print(f"  Total events: {len(all_events)}")
    print()

    # 创建 book 和 strategy
    book = LimitUpBidBook(limit_up_price)
    strategy = LimitUpBreakSellStrategy(limit_up_price)

    # 回放
    last_report_time = 0
    monitoring_entered = False

    for row in all_events:
        mdtime = int(row['mdtime'])

        # 集合竞价阶段: 只更新 book，不触发策略
        if mdtime < 93000000:
            if row['_type'] == 'order':
                book.on_order(row)
            else:
                book.on_transaction(row)
            continue

        # 开盘后第一个事件：如果 bid_vol > 0 说明确实涨停了，进入 MONITORING
        if not monitoring_entered and book.total_bid_volume > 0:
            monitoring_entered = True
            strategy.enter_monitoring(mdtime)

        # 处理事件
        if row['_type'] == 'order':
            book.on_order(row)
            strategy.on_order(row, book)
        else:
            book.on_transaction(row)
            strategy.on_transaction(row, book)

        # 定期报告
        if mdtime // 10000000 != last_report_time // 10000000 or \
           (strategy.state == strategy.TRIGGERED):
            last_report_time = mdtime
            if strategy.state != strategy.TRIGGERED:
                print(f"  {format_mdtime(mdtime)} | bid_vol={book.total_bid_volume:>12} | "
                      f"orders={len(book.orders):>6} | flows={strategy.flow_event_count:>6}")

        if strategy.state == strategy.TRIGGERED:
            break

    # ==========================================
    # Step 2.5: 从 09:45:54.000 到 09:45:55.000 每 10ms 打印买一量
    # 重新回放，不跑策略，只追踪 book
    # ==========================================
    print("=== Step 2.5: 09:45:54.000 ~ 09:45:55.000 每 10ms 买一量 ===")
    book2 = LimitUpBidBook(limit_up_price)
    # 先全量回放到 09:45:54 之前
    next_print_ms = mdtime_to_ms(94554000)  # 09:45:54.000
    end_ms = mdtime_to_ms(94555000)         # 09:45:55.000
    for row in all_events:
        mdtime = int(row['mdtime'])
        cur_ms = mdtime_to_ms(mdtime)

        # 到达打印区间后，先打印再处理
        while cur_ms >= next_print_ms and next_print_ms <= end_ms:
            print(f"  {format_mdtime(mdtime):>15} (seq={int(row['applseqnum']):>8}) | "
                  f"bid_vol={book2.total_bid_volume:>12} | orders={len(book2.orders):>6}")
            next_print_ms += 10  # +10ms

        if row['_type'] == 'order':
            book2.on_order(row)
        else:
            book2.on_transaction(row)

        if cur_ms > end_ms + 1000:
            break

    print()

    # ==========================================
    # Step 3: 比较结果
    # ==========================================
    print("=" * 60)
    print("=== 测试结果 ===")
    print("=" * 60)

    if strategy.trigger_time:
        print(f"  策略触发时间:   {format_mdtime(strategy.trigger_time)}")
        print(f"  触发原因:       {strategy.trigger_reason}")
    else:
        print(f"  策略未触发! state={strategy.state}")

    if real_break_time:
        print(f"  真实开板时间:   {format_mdtime(real_break_time)} (tick bid1 != maxpx)")

    if strategy.trigger_time and real_break_time:
        trigger_ms = mdtime_to_ms(strategy.trigger_time)
        break_ms = mdtime_to_ms(real_break_time)
        diff_ms = break_ms - trigger_ms

        print()
        if diff_ms > 0:
            print(f"  >>> 策略提前 {diff_ms}ms 发出卖出信号 <<<")
        elif diff_ms == 0:
            print(f"  >>> 策略与开板同时触发 <<<")
        else:
            print(f"  >>> 策略滞后 {-diff_ms}ms <<<")

    print()

    # 打印触发时刻的 flow_window 详情
    print("=== 触发时刻 flow_window 详情 ===")
    for t, vol in strategy.flow_window:
        print(f"  {format_mdtime(t)} | volume={vol}")
    print(f"  Total flow events: {strategy.flow_event_count}")
    print(f"  Final bid_volume: {book.total_bid_volume}")
    print(f"  Remaining orders: {len(book.orders)}")


if __name__ == "__main__":
    main()
