---
name: backtest
description: 运行策略回测。当用户要求回测某个股票的策略时使用此技能。支持下载数据、配置策略、运行引擎并显示结果。
allowed-tools: Bash(python:*), Bash(python3:*), Read, Edit
---

# 策略回测技能

当用户要求回测策略时，使用 `script/run_strategy_backtest.py` 脚本执行回测。

## 用法

```bash
python script/run_strategy_backtest.py <股票代码> <策略名称> [日期]
```

## 参数说明

- **股票代码**: 支持带后缀(600759.SH)或不带后缀(600759)格式
- **策略名称**: 以下策略之一
  - `OpeningRangeBreakoutStrategy` - 开盘区间突破策略
  - `PercentageGainBreakoutStrategy` - 追4/6新高策略
  - `HenggouStrategy` - 横沟策略（90秒涨速突破）
  - `BreakoutPriceVolumeStrategy` - 突破价格成交量策略
- **日期**: 可选，YYYYMMDD格式，默认今天
- **--no-download**: 跳过数据下载，使用本地已有数据
- **-v**: 显示详细输出

## 示例

回测横沟策略（自动下载今日数据）:
```bash
python script/run_strategy_backtest.py 688028 HenggouStrategy
```

回测追4/6策略:
```bash
python script/run_strategy_backtest.py 600266.SH PercentageGainBreakoutStrategy
```

回测开盘区间突破策略:
```bash
python script/run_strategy_backtest.py 600759 OpeningRangeBreakoutStrategy
```

使用本地已有数据（跳过下载）:
```bash
python script/run_strategy_backtest.py 688556.SH HenggouStrategy --no-download
```

## 执行流程

脚本会自动:
1. 下载市场数据（上海股票从服务器 mmap 下载）
2. 更新回测配置文件
3. 运行回测引擎
4. 解析并格式化输出结果

## 注意事项

- 深圳股票（非6开头）暂不支持从服务器下载，需要本地已有数据或使用 `--no-download`
- 如果回测失败，检查引擎是否已编译: `./build.sh engine`
- 详细输出使用 `-v` 参数查看 Phase 转换信息
