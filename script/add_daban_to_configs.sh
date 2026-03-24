#!/bin/bash
# 给当前目录下所有 strategy_live*.conf 中的股票添加 DabanStrategy 配置
# 用法: 在 config 目录下运行 ./add_daban_to_configs.sh

for conf in strategy_live*.conf; do
    [ -f "$conf" ] || continue

    echo "Processing: $conf"

    # 提取所有唯一股票代码（去重）
    symbols=$(awk -F',' '{print $1}' "$conf" | sort -u)

    # 检查哪些股票还没有 DabanStrategy
    added=0
    for sym in $symbols; do
        if ! grep -q "^${sym},DabanStrategy$" "$conf"; then
            echo "${sym},DabanStrategy" >> "$conf"
            ((added++))
        fi
    done

    echo "  Added DabanStrategy to $added symbols"
done

echo "Done."
