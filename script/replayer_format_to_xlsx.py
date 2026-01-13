#!/usr/bin/env python3
"""
高性能市场数据格式转换工具
将 debug 字符串格式的 csv 文件转换为结构化的 csv/xlsx 格式

用法:
    python replayer_format_to_xlsx.py                     # 转换所有股票
    python replayer_format_to_xlsx.py 600759.SH          # 转换指定股票
    python replayer_format_to_xlsx.py 600759.SH --xlsx   # 强制输出 xlsx
"""

import argparse
import re
import sys
import time
from pathlib import Path
from typing import List, Optional

try:
    import pandas as pd
except ImportError:
    print("请先安装pandas: pip install pandas")
    sys.exit(1)

# 预编译正则表达式（性能优化）
PATTERN = re.compile(r'(\w+):\s*("([^"]+)"|(\S+))')


def parse_line_fast(line: str) -> dict:
    """快速解析一行数据，返回 key-value 字典"""
    result = {}
    for match in PATTERN.finditer(line):
        key = match.group(1)
        if key == 'MDDate':  # 跳过 MDDate
            continue
        # 带引号的值取引号内容，否则取整个值
        value = match.group(3) if match.group(3) else match.group(4)
        # 尝试转换为数字
        if value and not match.group(3):  # 非字符串才尝试转换
            try:
                value = int(value) if '.' not in value else float(value)
            except ValueError:
                pass
        result[key] = value
    return result


def process_file(input_path: Path, output_path: Path, use_xlsx: bool = False) -> bool:
    """处理单个文件"""
    print(f"处理: {input_path.name}")
    start_time = time.time()

    # 读取并解析
    rows = []
    line_count = 0

    with open(input_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line:
                row = parse_line_fast(line)
                if row:
                    rows.append(row)
                line_count += 1
                # 进度显示
                if line_count % 50000 == 0:
                    print(f"  已处理 {line_count:,} 行...")

    if not rows:
        print(f"  警告: 文件为空或无法解析")
        return False

    # 构建 DataFrame
    df = pd.DataFrame(rows)

    # 输出文件
    if use_xlsx:
        # xlsx 输出（使用 xlsxwriter 引擎更快）
        try:
            df.to_excel(output_path, index=False, engine='xlsxwriter')
        except ImportError:
            print("  xlsxwriter 不可用，使用 openpyxl")
            df.to_excel(output_path, index=False)
    else:
        # csv 输出（快 10-50 倍）
        df.to_csv(output_path, index=False)

    elapsed = time.time() - start_time
    print(f"  完成: {len(rows):,} 行, {elapsed:.2f} 秒 -> {output_path.name}")
    return True


def find_files(test_data_dir: Path, security_ids: Optional[List[str]] = None) -> List[Path]:
    """查找要处理的文件"""
    all_files = list(test_data_dir.glob('*.csv'))

    if not security_ids:
        return all_files

    # 根据股票代码过滤
    filtered = []
    for sec_id in security_ids:
        for f in all_files:
            if sec_id in f.name:
                filtered.append(f)
    return filtered


def main():
    parser = argparse.ArgumentParser(
        description='高性能市场数据格式转换工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
  %(prog)s                        # 转换所有股票 (输出 csv)
  %(prog)s 600759.SH              # 转换指定股票
  %(prog)s 600759.SH 002603.SZ    # 转换多个股票
  %(prog)s 600759.SH --xlsx       # 强制输出 xlsx 格式
'''
    )
    parser.add_argument('securities', nargs='*', help='股票代码列表 (如 600759.SH)')
    parser.add_argument('--xlsx', action='store_true', help='输出 xlsx 格式 (默认 csv，更快)')
    parser.add_argument('--input', '-i', type=str, help='输入目录 (默认: test_data)')
    parser.add_argument('--output', '-o', type=str, help='输出目录 (默认: test_data_cleaned)')

    args = parser.parse_args()

    # 目录设置
    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    test_data_dir = Path(args.input) if args.input else project_dir / 'test_data'
    output_dir = Path(args.output) if args.output else project_dir / 'test_data_cleaned'

    if not test_data_dir.exists():
        print(f"错误: 输入目录不存在: {test_data_dir}")
        sys.exit(1)

    output_dir.mkdir(exist_ok=True)

    # 查找文件
    files = find_files(test_data_dir, args.securities if args.securities else None)

    if not files:
        if args.securities:
            print(f"错误: 未找到匹配的文件: {args.securities}")
        else:
            print(f"错误: 目录中没有 csv 文件: {test_data_dir}")
        sys.exit(1)

    print(f"找到 {len(files)} 个文件待处理")
    print(f"输出格式: {'xlsx' if args.xlsx else 'csv'}")
    print()

    # 处理文件
    total_start = time.time()
    success_count = 0

    for file_path in sorted(files):
        ext = '.xlsx' if args.xlsx else '.csv'
        output_path = output_dir / (file_path.stem + ext)
        if process_file(file_path, output_path, args.xlsx):
            success_count += 1
        print()

    total_elapsed = time.time() - total_start
    print(f"完成！处理 {success_count}/{len(files)} 个文件，总耗时 {total_elapsed:.2f} 秒")
    print(f"输出目录: {output_dir}")


if __name__ == '__main__':
    main()
