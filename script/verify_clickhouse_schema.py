#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
验证 ClickHouse 表结构与 C++ struct 字段的对应关系

用法:
    python script/verify_clickhouse_schema.py

功能:
1. 解析 include/market_data_structs_aligned.h 中的 C++ struct 定义
2. 解析 script/create_clickhouse_tables_v2.sql 中的表定义
3. 对比两者的字段，报告缺失或多余的字段
"""
from __future__ import print_function
import re
import os
import sys

# 项目根目录
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

# 文件路径
HEADER_FILE = os.path.join(PROJECT_ROOT, 'include', 'market_data_structs_aligned.h')
SQL_FILE = os.path.join(PROJECT_ROOT, 'script', 'create_clickhouse_tables_v2.sql')

# 结构体与表的映射 (支持多种表名格式)
STRUCT_TABLE_MAP = {
    'MDOrderStruct': ['orders', 'MDOrderStruct'],
    'MDTransactionStruct': ['transactions', 'MDTransactionStruct'],
    'MDStockStruct': ['ticks', 'MDStockStruct'],
}

# 忽略的字段（padding 等）
IGNORED_FIELDS = {'_pad'}


def parse_cpp_struct(header_content, struct_name):
    """解析 C++ struct 定义，提取字段名列表"""
    # 匹配 struct 定义块
    pattern = r'struct\s+{}\s*\{{([^}}]+)\}}'.format(struct_name)
    match = re.search(pattern, header_content, re.DOTALL)
    if not match:
        print("ERROR: Cannot find struct {}".format(struct_name))
        return []

    struct_body = match.group(1)
    fields = []

    # 匹配字段定义: type fieldname; 或 type fieldname[size];
    field_pattern = r'(?:int64_t|int32_t|uint32_t|char)\s+(\w+)(?:\[\d+\])?;'

    for line in struct_body.split('\n'):
        line = line.strip()
        # 跳过注释和空行
        if not line or line.startswith('//'):
            continue

        match = re.search(field_pattern, line)
        if match:
            field_name = match.group(1).lower()
            if field_name not in IGNORED_FIELDS:
                fields.append(field_name)

    return fields


def parse_sql_table(sql_content, table_names):
    """解析 SQL CREATE TABLE 定义，提取列名列表

    Args:
        sql_content: SQL 文件内容
        table_names: 表名列表，尝试匹配任意一个

    Returns:
        (columns, materialized_cols, alias_cols, matched_name)
    """
    if isinstance(table_names, str):
        table_names = [table_names]

    table_body = None
    matched_name = None

    for table_name in table_names:
        # 尝试多种格式: market_data.xxx 或直接 xxx
        patterns = [
            r'CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?market_data\.{}\s*\(([^;]+)\)\s*ENGINE'.format(table_name),
            r'CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?{}\s*\(([^;]+)\)\s*ENGINE'.format(table_name),
        ]

        for pattern in patterns:
            match = re.search(pattern, sql_content, re.DOTALL | re.IGNORECASE)
            if match:
                table_body = match.group(1)
                matched_name = table_name
                break

        if table_body:
            break

    if not table_body:
        print("ERROR: Cannot find table (tried: {})".format(', '.join(table_names)))
        return [], [], [], None

    columns = []
    materialized_cols = []
    alias_cols = []

    for line in table_body.split('\n'):
        line = line.strip()
        # 跳过注释和空行
        if not line or line.startswith('--'):
            continue

        # 检测 MATERIALIZED 列
        if 'MATERIALIZED' in line:
            col_match = re.match(r'^(\w+)\s+', line)
            if col_match:
                materialized_cols.append(col_match.group(1).lower())
            continue

        # 检测 ALIAS 列
        if 'ALIAS' in line:
            col_match = re.match(r'^(\w+)\s+', line)
            if col_match:
                alias_cols.append(col_match.group(1).lower())
            continue

        # 匹配普通列定义: column_name TYPE (支持更多类型和 CODEC)
        col_match = re.match(r'^(\w+)\s+(?:Int64|Int32|UInt32|String|Array|FixedString)', line)
        if col_match:
            col_name = col_match.group(1).lower()
            columns.append(col_name)

    return columns, materialized_cols, alias_cols, matched_name


def compare_fields(struct_name, table_name, cpp_fields, sql_columns, materialized_cols, alias_cols):
    """对比 C++ 字段和 SQL 列"""
    cpp_set = set(cpp_fields)
    sql_set = set(sql_columns)

    # 特殊处理：数组字段在 SQL 中可能有不同的名称
    # 例如 buypricequeue[10] 在 SQL 中是 buypricequeue Array(Int64)

    missing_in_sql = cpp_set - sql_set
    extra_in_sql = sql_set - cpp_set
    common = cpp_set & sql_set

    print("\n" + "=" * 60)
    print("{} -> {}".format(struct_name, table_name))
    print("=" * 60)
    print("C++ struct fields: {}".format(len(cpp_fields)))
    print("SQL real columns:  {}".format(len(sql_columns)))
    print("Common fields:     {}".format(len(common)))

    all_ok = True

    if missing_in_sql:
        all_ok = False
        print("\n[ERROR] Missing in SQL (present in C++ struct):")
        for f in sorted(missing_in_sql):
            print("  - {}".format(f))

    if extra_in_sql:
        # extra_in_sql 可能是合法的（如 _pad），只做提示
        print("\n[INFO] Extra real columns in SQL (not in C++ struct):")
        for f in sorted(extra_in_sql):
            print("  + {}".format(f))

    # 显示 MATERIALIZED 列
    if materialized_cols:
        print("\n[INFO] MATERIALIZED columns (auto-computed, stored):")
        for f in materialized_cols:
            print("  * {}".format(f))

    # 显示 ALIAS 列
    if alias_cols:
        print("\n[INFO] ALIAS columns (computed on query, not stored):")
        for f in alias_cols:
            print("  ~ {}".format(f))

    if all_ok and not missing_in_sql:
        print("\n[OK] All C++ fields are present in SQL table")

    return len(missing_in_sql) == 0


def main():
    # 支持命令行参数指定 SQL 文件
    if len(sys.argv) > 1:
        sql_file = sys.argv[1]
        if not os.path.isabs(sql_file):
            sql_file = os.path.join(PROJECT_ROOT, sql_file)
    else:
        sql_file = SQL_FILE

    print("Verifying ClickHouse schema against C++ structs...")
    print("Header file: {}".format(HEADER_FILE))
    print("SQL file: {}".format(sql_file))

    # 读取文件
    if not os.path.exists(HEADER_FILE):
        print("ERROR: Header file not found: {}".format(HEADER_FILE))
        sys.exit(1)

    if not os.path.exists(sql_file):
        print("ERROR: SQL file not found: {}".format(sql_file))
        sys.exit(1)

    with open(HEADER_FILE, 'r') as f:
        header_content = f.read()

    with open(sql_file, 'r') as f:
        sql_content = f.read()

    # 验证每个结构体
    all_ok = True
    for struct_name, table_names in STRUCT_TABLE_MAP.items():
        cpp_fields = parse_cpp_struct(header_content, struct_name)
        sql_columns, materialized_cols, alias_cols, matched_name = parse_sql_table(sql_content, table_names)

        if not cpp_fields:
            print("WARNING: No fields parsed from {}".format(struct_name))
            all_ok = False
            continue

        if not sql_columns and not matched_name:
            print("WARNING: No columns parsed from table (tried: {})".format(', '.join(table_names)))
            all_ok = False
            continue

        if not compare_fields(struct_name, matched_name, cpp_fields, sql_columns, materialized_cols, alias_cols):
            all_ok = False

    print("\n" + "=" * 60)
    if all_ok:
        print("VERIFICATION PASSED: All fields are mapped correctly")
        sys.exit(0)
    else:
        print("VERIFICATION FAILED: Some fields are missing")
        sys.exit(1)


if __name__ == '__main__':
    main()
