#!/usr/bin/env python3
"""
比较两个头文件中的结构体字段是否一致
用法: python script/compare_structs.py
"""

import re
from dataclasses import dataclass
from typing import Dict, List, Tuple
from pathlib import Path


@dataclass
class Field:
    """结构体字段"""
    type: str
    name: str
    array_size: str = ""  # 如果是数组，存储大小如 "[10]"

    def __str__(self):
        return f"{self.type} {self.name}{self.array_size}"

    def signature(self) -> str:
        """返回字段签名（类型+名称+数组大小）"""
        return f"{self.type}|{self.name}|{self.array_size}"


@dataclass
class Struct:
    """结构体定义"""
    name: str
    fields: List[Field]

    def field_names(self) -> set:
        return {f.name for f in self.fields}

    def field_map(self) -> Dict[str, Field]:
        return {f.name: f for f in self.fields}


def parse_header(filepath: str) -> Dict[str, Struct]:
    """解析头文件，提取结构体定义"""
    with open(filepath, 'r') as f:
        content = f.read()

    structs = {}

    # 匹配 struct XXX { ... };
    struct_pattern = r'struct\s+(\w+)\s*\{([^}]+(?:\{[^}]*\}[^}]*)*)\};'

    for match in re.finditer(struct_pattern, content, re.DOTALL):
        struct_name = match.group(1)
        struct_body = match.group(2)

        fields = []

        # 匹配字段定义，支持：
        # - 基本类型: int32_t name;
        # - 数组: int64_t name[10];
        # - 嵌套结构体数组: MDEntryDetailStruct entries[10];
        field_pattern = r'^\s*((?:const\s+)?(?:unsigned\s+)?\w+)\s+(\w+)(\[\d+\])?;'

        for line in struct_body.split('\n'):
            # 跳过注释行
            line = re.sub(r'//.*$', '', line).strip()
            if not line:
                continue

            field_match = re.match(field_pattern, line)
            if field_match:
                field_type = field_match.group(1)
                field_name = field_match.group(2)
                array_size = field_match.group(3) or ""

                # 跳过 padding 字段
                if field_name.startswith('_pad') or field_name.startswith('_reserved'):
                    continue

                fields.append(Field(field_type, field_name, array_size))

        structs[struct_name] = Struct(struct_name, fields)

    return structs


def compare_structs(file1: str, file2: str) -> List[str]:
    """比较两个头文件中的结构体"""
    structs1 = parse_header(file1)
    structs2 = parse_header(file2)

    differences = []

    # 检查结构体是否存在
    all_struct_names = set(structs1.keys()) | set(structs2.keys())

    for struct_name in sorted(all_struct_names):
        if struct_name not in structs1:
            differences.append(f"❌ 结构体 {struct_name} 只存在于 {Path(file2).name}")
            continue
        if struct_name not in structs2:
            differences.append(f"❌ 结构体 {struct_name} 只存在于 {Path(file1).name}")
            continue

        s1 = structs1[struct_name]
        s2 = structs2[struct_name]

        # 比较字段
        names1 = s1.field_names()
        names2 = s2.field_names()
        map1 = s1.field_map()
        map2 = s2.field_map()

        only_in_1 = names1 - names2
        only_in_2 = names2 - names1
        common = names1 & names2

        struct_diffs = []

        # 只在文件1中的字段
        for name in sorted(only_in_1):
            struct_diffs.append(f"  ➖ 字段 '{map1[name]}' 只在 {Path(file1).name}")

        # 只在文件2中的字段
        for name in sorted(only_in_2):
            struct_diffs.append(f"  ➕ 字段 '{map2[name]}' 只在 {Path(file2).name}")

        # 检查共有字段的类型和数组大小是否一致
        for name in sorted(common):
            f1 = map1[name]
            f2 = map2[name]

            if f1.type != f2.type:
                struct_diffs.append(f"  ⚠️  字段 '{name}' 类型不同: {f1.type} vs {f2.type}")

            if f1.array_size != f2.array_size:
                struct_diffs.append(f"  ⚠️  字段 '{name}' 数组大小不同: {f1.array_size or '(非数组)'} vs {f2.array_size or '(非数组)'}")

        # 检查字段顺序
        order1 = [f.name for f in s1.fields]
        order2 = [f.name for f in s2.fields]

        # 只比较共有字段的相对顺序
        common_order1 = [n for n in order1 if n in common]
        common_order2 = [n for n in order2 if n in common]

        if common_order1 != common_order2:
            struct_diffs.append(f"  🔀 字段顺序不同")
            struct_diffs.append(f"      {Path(file1).name}: {', '.join(common_order1[:5])}...")
            struct_diffs.append(f"      {Path(file2).name}: {', '.join(common_order2[:5])}...")

        if struct_diffs:
            differences.append(f"\n📦 结构体 {struct_name}:")
            differences.extend(struct_diffs)
        else:
            differences.append(f"\n✅ 结构体 {struct_name}: 字段完全一致")

    return differences


def print_struct_summary(filepath: str):
    """打印结构体摘要"""
    structs = parse_header(filepath)
    print(f"\n📄 {Path(filepath).name} 结构体摘要:")
    print("-" * 50)
    for name, struct in structs.items():
        print(f"  {name}: {len(struct.fields)} 个字段")


def main():
    base_dir = Path(__file__).parent.parent
    file1 = base_dir / "include" / "market_data_structs.h"
    file2 = base_dir / "include" / "market_data_structs_aligned.h"

    print("=" * 60)
    print("结构体字段比较工具")
    print("=" * 60)
    print(f"\n比较文件:")
    print(f"  1. {file1}")
    print(f"  2. {file2}")

    # 打印结构体摘要
    print_struct_summary(str(file1))
    print_struct_summary(str(file2))

    # 比较结构体
    print("\n" + "=" * 60)
    print("比较结果")
    print("=" * 60)

    differences = compare_structs(str(file1), str(file2))

    for diff in differences:
        print(diff)

    # 统计
    print("\n" + "=" * 60)
    print("总结")
    print("=" * 60)

    has_issues = any("❌" in d or "⚠️" in d or "➖" in d or "➕" in d for d in differences)
    has_order_diff = any("🔀" in d for d in differences)

    if has_issues:
        print("⚠️  发现字段差异，请检查上述详情")
    elif has_order_diff:
        print("ℹ️  字段内容一致，但顺序不同（aligned 版本已优化内存布局）")
    else:
        print("✅ 所有结构体字段完全一致")


if __name__ == "__main__":
    main()
