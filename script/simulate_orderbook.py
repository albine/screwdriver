#!/usr/bin/env python3
"""
OrderBook Python模拟器

模拟FastOrderBook的处理逻辑，用于验证C++实现的正确性。
从xlsx文件读取ORDER和TRANSACTION数据，模拟订单簿的状态变化。

使用方法:
    python3 script/simulate_orderbook.py
    python3 script/simulate_orderbook.py test_data_cleaned/MD_ORDER_StockType_603122.SH.xlsx test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.xlsx
"""
import pandas as pd
import sys
from collections import defaultdict
from pathlib import Path


def load_data(order_file: str, txn_file: str):
    """加载ORDER和TRANSACTION数据"""
    order_df = pd.read_excel(order_file)
    txn_df = pd.read_excel(txn_file)
    return order_df, txn_df


def create_event_stream(order_df: pd.DataFrame, txn_df: pd.DataFrame) -> list:
    """创建按时间和ApplSeqNum排序的事件流"""
    events = []

    for _, row in order_df.iterrows():
        events.append({
            'type': 'ORDER',
            'applseqnum': row['ApplSeqNum'],
            'mdtime': row['MDTime'],
            'orderno': row['OrderNO'],
            'ordertype': row['OrderType'],
            'price': row['OrderPrice'],
            'qty': row['OrderQty'],
            'bsflag': row['OrderBSFlag']
        })

    for _, row in txn_df.iterrows():
        events.append({
            'type': 'TXN',
            'applseqnum': row['ApplSeqNum'],
            'mdtime': row['MDTime'],
            'tradebuyno': row['TradeBuyNo'],
            'tradesellno': row['TradeSellNo'],
            'tradebsflag': row['TradeBSFlag'],
            'qty': row['TradeQty'],
            'price': row['TradePrice']
        })

    # 按MDTime和ApplSeqNum排序
    events.sort(key=lambda x: (x['mdtime'], x['applseqnum']))
    return events


def simulate_orderbook(events: list) -> dict:
    """
    模拟OrderBook处理逻辑

    返回:
        orders: 最终的活跃订单 {orderno: {price, qty, side}}
    """
    orders = {}  # orderno -> {price, qty, side}

    for event in events:
        if event['type'] == 'ORDER':
            orderno = event['orderno']
            ordertype = event['ordertype']

            if ordertype == 2:  # 新增订单
                orders[orderno] = {
                    'price': event['price'],
                    'qty': event['qty'],
                    'side': 'buy' if event['bsflag'] == 1 else 'sell'
                }
            elif ordertype == 10:  # 撤单
                if orderno in orders:
                    del orders[orderno]

        else:  # TXN
            buyno = event['tradebuyno']
            sellno = event['tradesellno']
            bsflag = event['tradebsflag']
            qty = event['qty']

            # TradeBSFlag处理逻辑（与FastOrderBook一致）
            # 0: 不明，更新双方
            # 1: 买方主动，只更新卖方
            # 2: 卖方主动，只更新买方

            if bsflag == 0:  # 更新双方
                for orderno in [buyno, sellno]:
                    if orderno in orders:
                        orders[orderno]['qty'] -= qty
                        if orders[orderno]['qty'] <= 0:
                            del orders[orderno]
            elif bsflag == 1:  # 只更新卖方
                if sellno in orders:
                    orders[sellno]['qty'] -= qty
                    if orders[sellno]['qty'] <= 0:
                        del orders[sellno]
            elif bsflag == 2:  # 只更新买方
                if buyno in orders:
                    orders[buyno]['qty'] -= qty
                    if orders[buyno]['qty'] <= 0:
                        del orders[buyno]

    return orders


def analyze_orderbook(orders: dict):
    """分析订单簿状态"""
    buy_orders = {k: v for k, v in orders.items() if v['side'] == 'buy'}
    sell_orders = {k: v for k, v in orders.items() if v['side'] == 'sell'}

    print("=" * 60)
    print("订单簿模拟结果")
    print("=" * 60)

    print(f"\n活跃买单数: {len(buy_orders)}")
    print(f"活跃卖单数: {len(sell_orders)}")
    print(f"活跃订单总数: {len(orders)}")

    # 买盘分析
    if buy_orders:
        # 按价格聚合
        buy_by_price = defaultdict(lambda: {'count': 0, 'total_qty': 0})
        for order in buy_orders.values():
            price = order['price']
            buy_by_price[price]['count'] += 1
            buy_by_price[price]['total_qty'] += order['qty']

        # 最高买价（买一）
        best_bid = max(buy_by_price.keys())
        best_bid_info = buy_by_price[best_bid]

        print(f"\n=== 买盘 ===")
        print(f"买一价: {best_bid} ({best_bid/10000:.2f}元)")
        print(f"买一量: {best_bid_info['total_qty']}")
        print(f"买一订单数: {best_bid_info['count']}")

        # 买盘价格分布（前10档）
        print(f"\n买盘价格分布 (前10档):")
        for price in sorted(buy_by_price.keys(), reverse=True)[:10]:
            info = buy_by_price[price]
            print(f"  {price} ({price/10000:.2f}元): {info['count']}单, 总量{info['total_qty']}")

        # 买盘总量
        total_buy_qty = sum(order['qty'] for order in buy_orders.values())
        print(f"\n买盘总量: {total_buy_qty}")
    else:
        print("\n买盘: 空")

    # 卖盘分析
    if sell_orders:
        # 按价格聚合
        sell_by_price = defaultdict(lambda: {'count': 0, 'total_qty': 0})
        for order in sell_orders.values():
            price = order['price']
            sell_by_price[price]['count'] += 1
            sell_by_price[price]['total_qty'] += order['qty']

        # 最低卖价（卖一）
        best_ask = min(sell_by_price.keys())
        best_ask_info = sell_by_price[best_ask]

        print(f"\n=== 卖盘 ===")
        print(f"卖一价: {best_ask} ({best_ask/10000:.2f}元)")
        print(f"卖一量: {best_ask_info['total_qty']}")
        print(f"卖一订单数: {best_ask_info['count']}")

        # 卖盘价格分布（前10档）
        print(f"\n卖盘价格分布 (前10档):")
        for price in sorted(sell_by_price.keys())[:10]:
            info = sell_by_price[price]
            print(f"  {price} ({price/10000:.2f}元): {info['count']}单, 总量{info['total_qty']}")

        # 卖盘总量
        total_sell_qty = sum(order['qty'] for order in sell_orders.values())
        print(f"\n卖盘总量: {total_sell_qty}")
    else:
        print("\n卖盘: 空")

    print("\n" + "=" * 60)


def main():
    # 默认文件路径
    if len(sys.argv) >= 3:
        order_file = sys.argv[1]
        txn_file = sys.argv[2]
    else:
        order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.xlsx"
        txn_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.xlsx"

    print(f"ORDER文件: {order_file}")
    print(f"TRANSACTION文件: {txn_file}")

    # 检查文件是否存在
    if not Path(order_file).exists():
        print(f"错误: ORDER文件不存在: {order_file}")
        sys.exit(1)
    if not Path(txn_file).exists():
        print(f"错误: TRANSACTION文件不存在: {txn_file}")
        sys.exit(1)

    # 加载数据
    print("\n加载数据...")
    order_df, txn_df = load_data(order_file, txn_file)
    print(f"ORDER记录数: {len(order_df)}")
    print(f"TRANSACTION记录数: {len(txn_df)}")

    # 创建事件流
    print("\n创建事件流...")
    events = create_event_stream(order_df, txn_df)
    print(f"事件总数: {len(events)}")

    # 模拟处理
    print("\n模拟OrderBook处理...")
    orders = simulate_orderbook(events)

    # 分析结果
    analyze_orderbook(orders)


if __name__ == "__main__":
    main()
