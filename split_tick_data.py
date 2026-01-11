#!/usr/bin/env python3
"""
将SSEL2 Tick历史数据文件拆分为逐笔委托和逐笔成交两个文件
"""

import pandas as pd
import sys
from pathlib import Path


def split_tick_data(input_file: str, output_dir: str = "test_data_cleaned"):
    """
    拆分Tick数据文件

    Args:
        input_file: 输入的xlsx文件路径
        output_dir: 输出目录
    """
    print(f"读取文件: {input_file}")
    df = pd.read_excel(input_file)

    # 从文件名提取证券代码
    filename = Path(input_file).stem
    symbol = filename.split('-')[0]  # 603122-SSEL2_Tick_20251031 -> 603122
    security_id = f"{symbol}.SH"

    print(f"证券代码: {security_id}")
    print(f"总记录数: {len(df)}")

    # 按TickType分类
    tick_type_col = '12TickType类型'
    order_df = df[df[tick_type_col].isin(['A', 'D'])].copy()
    trade_df = df[df[tick_type_col] == 'T'].copy()

    print(f"逐笔委托记录数: {len(order_df)} (A: {len(df[df[tick_type_col] == 'A'])}, D: {len(df[df[tick_type_col] == 'D'])})")
    print(f"逐笔成交记录数: {len(trade_df)}")

    # 处理逐笔委托 (ORDER)
    order_result = process_order(order_df, security_id)

    # 处理逐笔成交 (TRANSACTION)
    trade_result = process_transaction(trade_df, security_id)

    # 确保输出目录存在
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)

    # 保存文件
    order_file = output_path / f"MD_ORDER_StockType_{security_id}.xlsx"
    trade_file = output_path / f"MD_TRANSACTION_StockType_{security_id}.xlsx"

    order_result.to_excel(order_file, index=False)
    trade_result.to_excel(trade_file, index=False)

    print(f"\n输出文件:")
    print(f"  逐笔委托: {order_file}")
    print(f"  逐笔成交: {trade_file}")


def process_order(df: pd.DataFrame, security_id: str) -> pd.DataFrame:
    """
    处理逐笔委托数据

    字段映射:
    - HTSCSecurityID: 证券代码 (加.SH后缀)
    - MDTime: 7TickTime逐笔时间
    - securityIDSource: XSHG (固定值)
    - securityType: StockType (固定值)
    - OrderIndex: 4RecID逐笔序号
    - OrderType: 12TickType类型 (A=2, D=10)
    - OrderPrice: 8TickPrice逐笔价格 * 10000
    - OrderQty: 9TickVolume逐笔数量
    - OrderBSFlag: 11TickBSFlag逐笔标识 (B=1, S=2)
    - ChannelNo: 5Channel逐笔通道
    - OrderNO: B取BuyOrderNO, S取SellOrderNO
    - ApplSeqNum: 4RecID逐笔序号
    - DataMultiplePowerOf10: 4 (固定值)
    - TradedQty: 10TradeAmount已成交的委托数量/成交金额
    """
    result = pd.DataFrame()

    result['HTSCSecurityID'] = [security_id] * len(df)
    result['MDTime'] = df['7TickTime逐笔时间'].values
    result['securityIDSource'] = 'XSHG'
    result['securityType'] = 'StockType'
    result['OrderIndex'] = df['4RecID逐笔序号'].values

    # OrderType: A=2, D=10
    result['OrderType'] = df['12TickType类型'].map({'A': 2, 'D': 10}).values

    # OrderPrice: 乘以10000并转为整数
    result['OrderPrice'] = (df['8TickPrice逐笔价格'] * 10000).round().astype(int).values

    result['OrderQty'] = df['9TickVolume逐笔数量'].values

    # OrderBSFlag: B=1, S=2
    result['OrderBSFlag'] = df['11TickBSFlag逐笔标识'].map({'B': 1, 'S': 2}).values

    result['ChannelNo'] = df['5Channel逐笔通道'].values

    # OrderNO: B取BuyOrderNO, S取SellOrderNO
    bs_flag = df['11TickBSFlag逐笔标识'].values
    buy_order_no = df['13BuyOrderNO买方订单'].values
    sell_order_no = df['14SellOrderNO卖方订单'].values
    result['OrderNO'] = [buy_order_no[i] if bs_flag[i] == 'B' else sell_order_no[i] for i in range(len(df))]

    result['ApplSeqNum'] = df['4RecID逐笔序号'].values
    result['DataMultiplePowerOf10'] = 4

    # TradedQty: 已成交数量 (可能为NaN)
    traded_qty = df['10TradeAmount已成交的委托数量/成交金额'].values
    result['TradedQty'] = [None if v == 0 else v for v in traded_qty]

    return result


def process_transaction(df: pd.DataFrame, security_id: str) -> pd.DataFrame:
    """
    处理逐笔成交数据

    字段映射:
    - HTSCSecurityID: 证券代码 (加.SH后缀)
    - MDTime: 7TickTime逐笔时间
    - securityIDSource: XSHG (固定值)
    - securityType: StockType (固定值)
    - TradeIndex: 4RecID逐笔序号
    - TradeBuyNo: 13BuyOrderNO买方订单
    - TradeSellNo: 14SellOrderNO卖方订单
    - TradeBSFlag: 11TickBSFlag逐笔标识 (B=1, S=2, N=0)
    - TradePrice: 8TickPrice逐笔价格 * 10000
    - TradeQty: 9TickVolume逐笔数量
    - TradeMoney: 10TradeAmount已成交的委托数量/成交金额 * 10000
    - ApplSeqNum: 4RecID逐笔序号
    - ChannelNo: 5Channel逐笔通道
    - DataMultiplePowerOf10: 4 (固定值)
    """
    result = pd.DataFrame()

    result['HTSCSecurityID'] = [security_id] * len(df)
    result['MDTime'] = df['7TickTime逐笔时间'].values
    result['securityIDSource'] = 'XSHG'
    result['securityType'] = 'StockType'
    result['TradeIndex'] = df['4RecID逐笔序号'].values
    result['TradeBuyNo'] = df['13BuyOrderNO买方订单'].values
    result['TradeSellNo'] = df['14SellOrderNO卖方订单'].values

    # TradeBSFlag: B=1, S=2, N=0
    result['TradeBSFlag'] = df['11TickBSFlag逐笔标识'].map({'B': 1, 'S': 2, 'N': 0}).values

    # TradePrice: 乘以10000并转为整数
    result['TradePrice'] = (df['8TickPrice逐笔价格'] * 10000).round().astype(int).values

    result['TradeQty'] = df['9TickVolume逐笔数量'].values

    # TradeMoney: 乘以10000并转为整数
    result['TradeMoney'] = (df['10TradeAmount已成交的委托数量/成交金额'] * 10000).round().astype(int).values

    result['ApplSeqNum'] = df['4RecID逐笔序号'].values
    result['ChannelNo'] = df['5Channel逐笔通道'].values
    result['DataMultiplePowerOf10'] = 4

    return result


if __name__ == '__main__':
    if len(sys.argv) < 2:
        # 默认处理603122的数据
        input_file = "test_data/603122-SSEL2_Tick_20251031.xlsx"
    else:
        input_file = sys.argv[1]

    output_dir = sys.argv[2] if len(sys.argv) > 2 else "test_data_cleaned"

    split_tick_data(input_file, output_dir)
