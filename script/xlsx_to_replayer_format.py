#!/usr/bin/env python3
"""
将xlsx格式的ORDER和TRANSACTION数据转换为HistoryDataReplayer能读取的文本格式
"""

import pandas as pd
import re
import sys
from pathlib import Path


def extract_date_from_filename(filename: str) -> int:
    """从文件名中提取日期，如 MD_ORDER_StockType_603122.SH.xlsx -> 需要原始数据日期"""
    # 这里假设日期需要从外部传入，因为清洗后的文件名中没有日期
    # 默认返回20251031
    return 20251031


def convert_order_xlsx_to_txt(xlsx_file: str, txt_file: str, md_date: int):
    """
    将ORDER xlsx文件转换为txt格式

    格式: HTSCSecurityID: "xxx" MDDate: xxx MDTime: xxx ...
    """
    print(f"读取ORDER文件: {xlsx_file}")
    df = pd.read_excel(xlsx_file)
    print(f"记录数: {len(df)}")

    with open(txt_file, 'w') as f:
        for _, row in df.iterrows():
            # TradedQty: NaN转0
            traded_qty = 0 if pd.isna(row['TradedQty']) else int(row['TradedQty'])

            line = (
                f'HTSCSecurityID: "{row["HTSCSecurityID"]}" '
                f'MDDate: {md_date} '
                f'MDTime: {int(row["MDTime"])} '
                f'OrderIndex: {int(row["OrderIndex"])} '
                f'OrderType: {int(row["OrderType"])} '
                f'OrderPrice: {int(row["OrderPrice"])} '
                f'OrderQty: {int(row["OrderQty"])} '
                f'OrderBSFlag: {int(row["OrderBSFlag"])} '
                f'ChannelNo: {int(row["ChannelNo"])} '
                f'OrderNO: {int(row["OrderNO"])} '
                f'ApplSeqNum: {int(row["ApplSeqNum"])} '
                f'DataMultiplePowerOf10: {int(row["DataMultiplePowerOf10"])} '
                f'TradedQty: {traded_qty}'
            )
            f.write(line + '\n')

    print(f"输出文件: {txt_file}")


def convert_transaction_xlsx_to_txt(xlsx_file: str, txt_file: str, md_date: int):
    """
    将TRANSACTION xlsx文件转换为txt格式

    格式: HTSCSecurityID: "xxx" MDDate: xxx MDTime: xxx ...
    """
    print(f"读取TRANSACTION文件: {xlsx_file}")
    df = pd.read_excel(xlsx_file)
    print(f"记录数: {len(df)}")

    with open(txt_file, 'w') as f:
        for _, row in df.iterrows():
            line = (
                f'HTSCSecurityID: "{row["HTSCSecurityID"]}" '
                f'MDDate: {md_date} '
                f'MDTime: {int(row["MDTime"])} '
                f'TradeIndex: {int(row["TradeIndex"])} '
                f'TradeBuyNo: {int(row["TradeBuyNo"])} '
                f'TradeSellNo: {int(row["TradeSellNo"])} '
                f'TradeBSFlag: {int(row["TradeBSFlag"])} '
                f'TradePrice: {int(row["TradePrice"])} '
                f'TradeQty: {int(row["TradeQty"])} '
                f'TradeMoney: {int(row["TradeMoney"])} '
                f'ApplSeqNum: {int(row["ApplSeqNum"])} '
                f'ChannelNo: {int(row["ChannelNo"])} '
                f'DataMultiplePowerOf10: {int(row["DataMultiplePowerOf10"])}'
            )
            f.write(line + '\n')

    print(f"输出文件: {txt_file}")


def main():
    if len(sys.argv) < 2:
        # 默认处理603122
        security_id = "603122.SH"
        md_date = 20251031
    else:
        security_id = sys.argv[1]
        md_date = int(sys.argv[2]) if len(sys.argv) > 2 else 20251031

    input_dir = "test_data_cleaned"
    output_dir = "test_data_cleaned"

    order_xlsx = f"{input_dir}/MD_ORDER_StockType_{security_id}.xlsx"
    order_txt = f"{output_dir}/MD_ORDER_StockType_{security_id}.txt"

    trans_xlsx = f"{input_dir}/MD_TRANSACTION_StockType_{security_id}.xlsx"
    trans_txt = f"{output_dir}/MD_TRANSACTION_StockType_{security_id}.txt"

    print(f"证券代码: {security_id}")
    print(f"日期: {md_date}")
    print()

    convert_order_xlsx_to_txt(order_xlsx, order_txt, md_date)
    print()
    convert_transaction_xlsx_to_txt(trans_xlsx, trans_txt, md_date)

    print("\n转换完成!")


if __name__ == '__main__':
    main()
