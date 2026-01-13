#!/usr/bin/env python3
"""
验证脚本：通过拆分后的两个文件反向还原源文件，验证映射的正确性
"""

import pandas as pd
import numpy as np
from pathlib import Path


def restore_from_order(order_file: str) -> pd.DataFrame:
    """
    从ORDER文件反向还原逐笔委托数据

    反向映射:
    - HTSCSecurityID -> 6Symbol证券代码 (去掉.SH后缀)
    - MDTime -> 7TickTime逐笔时间
    - OrderIndex -> 4RecID逐笔序号
    - OrderType -> 12TickType类型 (2=A, 10=D)
    - OrderPrice -> 8TickPrice逐笔价格 (除以10000)
    - OrderQty -> 9TickVolume逐笔数量
    - OrderBSFlag -> 11TickBSFlag逐笔标识 (1=B, 2=S)
    - ChannelNo -> 5Channel逐笔通道
    - OrderNO -> 13BuyOrderNO买方订单 或 14SellOrderNO卖方订单
    - TradedQty -> 10TradeAmount已成交的委托数量/成交金额
    """
    df = pd.read_excel(order_file)

    result = pd.DataFrame()

    # 6Symbol证券代码: 去掉.SH后缀
    result['6Symbol证券代码'] = df['HTSCSecurityID'].str.replace('.SH', '', regex=False).astype(int)

    result['7TickTime逐笔时间'] = df['MDTime']
    result['4RecID逐笔序号'] = df['OrderIndex']

    # 12TickType类型: 2=A, 10=D
    result['12TickType类型'] = df['OrderType'].map({2: 'A', 10: 'D'})

    # 8TickPrice逐笔价格: 除以10000
    result['8TickPrice逐笔价格'] = df['OrderPrice'] / 10000

    result['9TickVolume逐笔数量'] = df['OrderQty']

    # 11TickBSFlag逐笔标识: 1=B, 2=S
    result['11TickBSFlag逐笔标识'] = df['OrderBSFlag'].map({1: 'B', 2: 'S'})

    result['5Channel逐笔通道'] = df['ChannelNo']

    # 13BuyOrderNO买方订单 和 14SellOrderNO卖方订单
    # B: BuyOrderNO=OrderNO, SellOrderNO=0
    # S: BuyOrderNO=0, SellOrderNO=OrderNO
    bs_flag = df['OrderBSFlag'].values
    order_no = df['OrderNO'].values
    result['13BuyOrderNO买方订单'] = [order_no[i] if bs_flag[i] == 1 else 0 for i in range(len(df))]
    result['14SellOrderNO卖方订单'] = [order_no[i] if bs_flag[i] == 2 else 0 for i in range(len(df))]

    # 10TradeAmount: NaN -> 0
    result['10TradeAmount已成交的委托数量/成交金额'] = df['TradedQty'].fillna(0)

    return result


def restore_from_transaction(trans_file: str) -> pd.DataFrame:
    """
    从TRANSACTION文件反向还原逐笔成交数据

    反向映射:
    - HTSCSecurityID -> 6Symbol证券代码 (去掉.SH后缀)
    - MDTime -> 7TickTime逐笔时间
    - TradeIndex -> 4RecID逐笔序号
    - 固定值 -> 12TickType类型 = 'T'
    - TradePrice -> 8TickPrice逐笔价格 (除以10000)
    - TradeQty -> 9TickVolume逐笔数量
    - TradeBSFlag -> 11TickBSFlag逐笔标识 (1=B, 2=S, 0=N)
    - ChannelNo -> 5Channel逐笔通道
    - TradeBuyNo -> 13BuyOrderNO买方订单
    - TradeSellNo -> 14SellOrderNO卖方订单
    - TradeMoney -> 10TradeAmount已成交的委托数量/成交金额 (除以10000)
    """
    df = pd.read_excel(trans_file)

    result = pd.DataFrame()

    # 6Symbol证券代码: 去掉.SH后缀
    result['6Symbol证券代码'] = df['HTSCSecurityID'].str.replace('.SH', '', regex=False).astype(int)

    result['7TickTime逐笔时间'] = df['MDTime']
    result['4RecID逐笔序号'] = df['TradeIndex']

    # 12TickType类型: 固定为T
    result['12TickType类型'] = 'T'

    # 8TickPrice逐笔价格: 除以10000
    result['8TickPrice逐笔价格'] = df['TradePrice'] / 10000

    result['9TickVolume逐笔数量'] = df['TradeQty']

    # 11TickBSFlag逐笔标识: 1=B, 2=S, 0=N
    result['11TickBSFlag逐笔标识'] = df['TradeBSFlag'].map({1: 'B', 2: 'S', 0: 'N'})

    result['5Channel逐笔通道'] = df['ChannelNo']

    result['13BuyOrderNO买方订单'] = df['TradeBuyNo']
    result['14SellOrderNO卖方订单'] = df['TradeSellNo']

    # 10TradeAmount: 除以10000
    result['10TradeAmount已成交的委托数量/成交金额'] = df['TradeMoney'] / 10000

    return result


def verify_split(source_file: str, order_file: str, trans_file: str):
    """
    验证拆分结果的正确性
    """
    print(f"读取源文件: {source_file}")
    source_df = pd.read_excel(source_file)

    # 排除TickType=S的行
    source_df = source_df[source_df['12TickType类型'] != 'S'].copy()
    print(f"源文件记录数 (排除S): {len(source_df)}")

    print(f"\n读取ORDER文件: {order_file}")
    order_restored = restore_from_order(order_file)
    print(f"ORDER还原记录数: {len(order_restored)}")

    print(f"\n读取TRANSACTION文件: {trans_file}")
    trans_restored = restore_from_transaction(trans_file)
    print(f"TRANSACTION还原记录数: {len(trans_restored)}")

    # 合并还原的数据
    restored_df = pd.concat([order_restored, trans_restored], ignore_index=True)
    print(f"\n合并后记录数: {len(restored_df)}")

    # 按RecID排序以便比较
    source_df = source_df.sort_values('4RecID逐笔序号').reset_index(drop=True)
    restored_df = restored_df.sort_values('4RecID逐笔序号').reset_index(drop=True)

    # 比较关键字段
    compare_cols = [
        '6Symbol证券代码',
        '7TickTime逐笔时间',
        '4RecID逐笔序号',
        '12TickType类型',
        '8TickPrice逐笔价格',
        '9TickVolume逐笔数量',
        '11TickBSFlag逐笔标识',
        '5Channel逐笔通道',
        '13BuyOrderNO买方订单',
        '14SellOrderNO卖方订单',
        '10TradeAmount已成交的委托数量/成交金额',
    ]

    print("\n" + "=" * 60)
    print("验证结果:")
    print("=" * 60)

    all_match = True
    for col in compare_cols:
        source_col = source_df[col].values
        restored_col = restored_df[col].values

        # 对于浮点数，使用近似比较
        if source_col.dtype in [np.float64, np.float32]:
            match = np.allclose(source_col, restored_col, rtol=1e-5, atol=1e-8, equal_nan=True)
        else:
            match = np.array_equal(source_col, restored_col)

        status = "✓" if match else "✗"
        print(f"  {status} {col}")

        if not match:
            all_match = False
            # 找出不匹配的行
            if source_col.dtype in [np.float64, np.float32]:
                diff_mask = ~np.isclose(source_col, restored_col, rtol=1e-5, atol=1e-8, equal_nan=True)
            else:
                diff_mask = source_col != restored_col

            diff_indices = np.where(diff_mask)[0][:5]  # 只显示前5个不匹配
            print(f"    不匹配样例 (前5个):")
            for idx in diff_indices:
                print(f"      行{idx}: 源={source_col[idx]}, 还原={restored_col[idx]}")

    print("=" * 60)
    if all_match:
        print("验证通过! 所有字段完全匹配。")
    else:
        print("验证失败! 存在不匹配的字段。")
    print("=" * 60)

    return all_match


if __name__ == '__main__':
    import sys

    if len(sys.argv) >= 4:
        source_file = sys.argv[1]
        order_file = sys.argv[2]
        trans_file = sys.argv[3]
    else:
        # 默认文件路径
        source_file = "test_data/603122-SSEL2_Tick_20251031.xlsx"
        order_file = "test_data_cleaned/MD_ORDER_StockType_603122.SH.xlsx"
        trans_file = "test_data_cleaned/MD_TRANSACTION_StockType_603122.SH.xlsx"

    verify_split(source_file, order_file, trans_file)
