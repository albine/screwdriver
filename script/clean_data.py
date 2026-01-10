#!/usr/bin/env python3
"""
清洗test_data目录中的数据文件，输出为xlsx格式
"""

import os
import re
from pathlib import Path

try:
    import pandas as pd
except ImportError:
    print("请先安装pandas: pip install pandas openpyxl")
    exit(1)

try:
    import openpyxl
except ImportError:
    print("请先安装openpyxl: pip install openpyxl")
    exit(1)


def parse_line(line: str) -> dict:
    """解析一行数据，返回key-value字典"""
    result = {}
    # 匹配 key: value 格式，value可能是带引号的字符串或数字
    pattern = r'(\w+):\s*("([^"]+)"|(\S+))'

    for match in re.finditer(pattern, line):
        key = match.group(1)
        # 跳过MDDate
        if key == 'MDDate':
            continue
        # 如果是带引号的值，取引号内的内容；否则取整个值
        if match.group(3):
            value = match.group(3)
        else:
            value = match.group(4)
            # 尝试转换为数字
            try:
                if '.' in value:
                    value = float(value)
                else:
                    value = int(value)
            except ValueError:
                pass
        result[key] = value

    return result


def process_file(input_path: str, output_path: str):
    """处理单个文件"""
    print(f"处理文件: {input_path}")

    rows = []
    with open(input_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line:
                row = parse_line(line)
                if row:
                    rows.append(row)

    if not rows:
        print(f"  警告: 文件为空或无法解析")
        return

    df = pd.DataFrame(rows)
    df.to_excel(output_path, index=False)
    print(f"  输出: {output_path} ({len(rows)} 行)")


def main():
    test_data_dir = Path(__file__).parent / 'test_data'
    output_dir = Path(__file__).parent / 'test_data_cleaned'

    # 创建输出目录
    output_dir.mkdir(exist_ok=True)

    # 处理所有csv文件
    for file_path in test_data_dir.glob('*.csv'):
        output_path = output_dir / (file_path.stem + '.xlsx')
        process_file(str(file_path), str(output_path))

    print(f"\n完成！清洗后的文件保存在: {output_dir}")


if __name__ == '__main__':
    main()
