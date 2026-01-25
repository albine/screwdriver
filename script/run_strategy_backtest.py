#!/usr/bin/env python3
"""
策略回测自动化脚本

用法:
    python script/run_strategy_backtest.py <股票代码> <策略名称> [日期]

示例:
    python script/run_strategy_backtest.py 600266.SH PercentageGainBreakoutStrategy
    python script/run_strategy_backtest.py 688258 PercentageGainBreakoutStrategy 20260120
    python script/run_strategy_backtest.py 600759 OpeningRangeBreakoutStrategy

支持的策略:
    - OpeningRangeBreakoutStrategy: 开盘区间突破策略
    - PercentageGainBreakoutStrategy: 追4/6新高策略
    - BreakoutPriceVolumeStrategy: 突破价格成交量策略

股票代码格式:
    - 带后缀: 600759.SH, 002603.SZ
    - 不带后缀: 600759, 002603 (自动识别交易所)
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from datetime import datetime

# 项目根目录
PROJECT_ROOT = Path(__file__).parent.parent.absolute()

# 可用策略列表
AVAILABLE_STRATEGIES = [
    "OpeningRangeBreakoutStrategy",
    "PercentageGainBreakoutStrategy",
    "BreakoutPriceVolumeStrategy",
    "BreakoutPriceVolumeStrategy_v2",
    "HenggouStrategy",
    "TestOrderbookStrategy",
]


def normalize_symbol(symbol: str) -> str:
    """规范化股票代码，添加交易所后缀"""
    symbol = symbol.strip().upper()

    # 已经有后缀
    if symbol.endswith('.SH') or symbol.endswith('.SZ'):
        return symbol

    # 提取数字部分
    digits = ''.join(c for c in symbol if c.isdigit())
    if len(digits) != 6:
        raise ValueError(f"无效的股票代码: {symbol}")

    # 判断交易所
    # 6开头 -> 上海, 其他 -> 深圳
    if digits.startswith('6'):
        return f"{digits}.SH"
    else:
        return f"{digits}.SZ"


def check_data_exists(symbol: str) -> bool:
    """检查本地是否已有数据文件"""
    test_data_dir = PROJECT_ROOT / "test_data"
    order_file = test_data_dir / f"MD_ORDER_StockType_{symbol}.csv"
    txn_file = test_data_dir / f"MD_TRANSACTION_StockType_{symbol}.csv"
    return order_file.exists() and txn_file.exists()


def download_data(symbol: str, date: str = None) -> bool:
    """下载市场数据"""
    print(f"\n[1/3] 下载 {symbol} 的市场数据...")

    download_script = PROJECT_ROOT / "script" / "download_mmap_data_v2.py"
    if not download_script.exists():
        print(f"错误: 下载脚本不存在: {download_script}")
        return False

    cmd = ["python", str(download_script), symbol]
    if date:
        cmd.append(date)

    try:
        result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            print(f"下载失败: {result.stderr}")
            return False
        print(result.stdout)
        return True
    except subprocess.TimeoutExpired:
        print("错误: 下载超时")
        return False
    except Exception as e:
        print(f"错误: {e}")
        return False


def update_config(symbol: str, strategy: str) -> bool:
    """更新回测配置文件"""
    print(f"\n[2/3] 更新配置文件...")

    config_file = PROJECT_ROOT / "config" / "strategy_backtest.conf"

    try:
        with open(config_file, 'r') as f:
            lines = f.readlines()

        # 找到配置行（非注释、非空行）
        new_lines = []
        found_config = False
        for line in lines:
            stripped = line.strip()
            if stripped and not stripped.startswith('#'):
                # 替换配置行
                new_lines.append(f"{symbol},{strategy}\n")
                found_config = True
            else:
                new_lines.append(line)

        # 如果没有找到配置行，添加一行
        if not found_config:
            new_lines.append(f"{symbol},{strategy}\n")

        with open(config_file, 'w') as f:
            f.writelines(new_lines)

        print(f"  配置: {symbol},{strategy}")
        return True
    except Exception as e:
        print(f"错误: 更新配置失败 - {e}")
        return False


def run_backtest() -> tuple[bool, str]:
    """运行回测引擎"""
    print(f"\n[3/3] 运行回测...")

    engine_path = PROJECT_ROOT / "build" / "engine"
    if not engine_path.exists():
        print(f"错误: 引擎不存在，请先运行 ./build.sh engine")
        return False, ""

    # 设置库路径
    env = os.environ.copy()
    lib_path = str(PROJECT_ROOT / "fastfish" / "libs")
    env['LD_LIBRARY_PATH'] = f"{lib_path}:{env.get('LD_LIBRARY_PATH', '')}"

    try:
        result = subprocess.run(
            [str(engine_path), "backtest"],
            cwd=PROJECT_ROOT,
            env=env,
            capture_output=True,
            timeout=300
        )
        # 合并 stdout 和 stderr，处理编码
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        return True, output
    except subprocess.TimeoutExpired:
        print("错误: 回测超时")
        return False, ""
    except Exception as e:
        print(f"错误: {e}")
        return False, ""


def parse_backtest_output(output: str) -> dict:
    """解析回测输出，提取关键信息"""
    result = {
        'signal': None,
        'open_info': None,
        'phases': [],
        'error': None,
    }

    lines = output.split('\n')
    for line in lines:
        # 解析开盘信息
        if '开盘:' in line or 'prev_close=' in line:
            result['open_info'] = line

        # 解析 Phase 完成信息
        if 'Phase1完成' in line or 'Phase2完成' in line or 'Phase1' in line or 'Phase2' in line:
            result['phases'].append(line)

        # 解析信号 (兼容两种格式: [SIGNAL] 和 [STRA])
        if '[SIGNAL]' in line or ('[STRA]' in line and 'BUY' in line):
            result['signal'] = line

        # 解析错误
        if '[ERR!]' in line or 'Unknown strategy' in line:
            result['error'] = line

    return result


def format_signal(signal_line: str) -> str:
    """格式化信号输出"""
    if not signal_line:
        return "无信号触发"

    # 提取关键信息
    parts = {}

    # 新格式: [STRA] 600158.SH | BUY | MARKET_TIME=09:30:00.050 | Price=98750(9.88元) | ...
    match = re.search(r'\[STRA\]\s+(\S+)\s+\|\s+(\w+)\s+\|', signal_line)
    if match:
        parts['symbol'] = match.group(1)
        parts['action'] = match.group(2)

    # 旧格式: [SIGNAL] BUY | 600403.SH | Time=09:33:14.460 | Price=70884(7.09元) | ...
    if not parts:
        match = re.search(r'\[SIGNAL\]\s+(\w+)\s+\|\s+(\S+)\s+\|', signal_line)
        if match:
            parts['action'] = match.group(1)
            parts['symbol'] = match.group(2)

    # 提取时间 (兼容 Time= 和 MARKET_TIME=)
    match = re.search(r'(?:Time|MARKET_TIME)=([^\s|]+)', signal_line)
    if match:
        parts['time'] = match.group(1)

    # 价格格式: Price=70884(7.09元)
    match = re.search(r'Price=(\d+)\(([^)]+)\)', signal_line)
    if match:
        parts['price_int'] = match.group(1)
        parts['price'] = match.group(2)

    match = re.search(r'Open=([^\s|]+)', signal_line)
    if match:
        parts['open'] = match.group(1)

    match = re.search(r'PrevClose=([^\s|]+)', signal_line)
    if match:
        parts['prev_close'] = match.group(1)

    # n 和 delta_n 需要精确匹配（避免匹配到其他数字）
    match = re.search(r'\|\s*n=(\d+)\s*\|', signal_line)
    if match:
        parts['n'] = match.group(1)

    match = re.search(r'\|\s*delta_n=(\d+)\s*\|', signal_line)
    if match:
        parts['delta_n'] = match.group(1)

    # Reason 在最后
    match = re.search(r'Reason=(.+)$', signal_line)
    if match:
        parts['reason'] = match.group(1)

    # 计算价格（元）
    price_yuan = '?'
    if 'price_int' in parts:
        try:
            price_yuan = f"{int(parts['price_int']) / 10000:.4f}"
        except:
            price_yuan = parts.get('price', '?')

    # 格式化输出
    output = []
    output.append(f"[SIGNAL] {parts.get('action', '?')} | {parts.get('symbol', '?')}")
    output.append(f"  Time      = {parts.get('time', '?')}")
    output.append(f"  Price     = {price_yuan} 元")
    output.append(f"  Open      = {parts.get('open', '?')}")
    output.append(f"  PrevClose = {parts.get('prev_close', '?')}")
    if 'n' in parts:
        output.append(f"  n         = {parts['n']} (平均成交量)")
    if 'delta_n' in parts:
        output.append(f"  delta_n   = {parts['delta_n']} (买盘成交量)")
    # Reason 可能包含中文，简化显示
    if 'reason' in parts:
        reason = parts['reason']
        # 简单判断触发类型
        if '动力学' in reason or 'delta' in reason.lower():
            output.append(f"  Reason    = 动力学触发")
        elif '直接' in reason:
            output.append(f"  Reason    = 直接突破")
        else:
            output.append(f"  Reason    = 突破触发")

    return '\n'.join(output)


def main():
    parser = argparse.ArgumentParser(
        description='策略回测自动化脚本',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
    python script/run_strategy_backtest.py 600266.SH PercentageGainBreakoutStrategy
    python script/run_strategy_backtest.py 688258 PercentageGainBreakoutStrategy 20260120
    python script/run_strategy_backtest.py 600759 OpeningRangeBreakoutStrategy

支持的策略:
    - OpeningRangeBreakoutStrategy: 开盘区间突破策略
    - PercentageGainBreakoutStrategy: 追4/6新高策略
    - BreakoutPriceVolumeStrategy: 突破价格成交量策略
        '''
    )
    parser.add_argument('symbol', help='股票代码 (如 600266.SH 或 600266)')
    parser.add_argument('strategy', help='策略名称')
    parser.add_argument('date', nargs='?', help='日期 (YYYYMMDD格式，默认今天)')
    parser.add_argument('--no-download', action='store_true', help='跳过数据下载，使用本地已有数据')
    parser.add_argument('--verbose', '-v', action='store_true', help='显示详细输出')

    args = parser.parse_args()

    # 规范化股票代码
    try:
        symbol = normalize_symbol(args.symbol)
    except ValueError as e:
        print(f"错误: {e}")
        sys.exit(1)

    # 验证策略名称
    strategy = args.strategy
    if strategy not in AVAILABLE_STRATEGIES:
        print(f"错误: 未知策略 '{strategy}'")
        print(f"可用策略: {', '.join(AVAILABLE_STRATEGIES)}")
        sys.exit(1)

    print(f"=" * 50)
    print(f"策略回测: {symbol} - {strategy}")
    print(f"=" * 50)

    # 1. 下载数据（如果需要）
    if args.no_download:
        if not check_data_exists(symbol):
            print(f"错误: 本地数据不存在: {symbol}")
            sys.exit(1)
        print(f"\n[1/3] 跳过下载，使用本地数据")
    elif check_data_exists(symbol):
        print(f"\n[1/3] 本地已有数据，跳过下载")
    else:
        if not download_data(symbol, args.date):
            sys.exit(1)

    # 2. 更新配置
    if not update_config(symbol, strategy):
        sys.exit(1)

    # 3. 运行回测
    success, output = run_backtest()

    if not success:
        sys.exit(1)

    # 4. 解析并显示结果
    print(f"\n{'=' * 50}")
    print("回测结果")
    print(f"{'=' * 50}")

    result = parse_backtest_output(output)

    if result['error']:
        print(f"\n错误: {result['error']}")
        sys.exit(1)

    # 显示开盘信息
    if result['open_info'] and args.verbose:
        print(f"\n开盘信息:")
        # 解码 UTF-8 转义字符
        info = result['open_info']
        info = info.encode('utf-8').decode('unicode_escape') if '\\x' in info else info
        print(f"  {info}")

    # 显示信号
    print(f"\n{format_signal(result['signal'])}")

    if args.verbose and output:
        print(f"\n{'=' * 50}")
        print("完整输出")
        print(f"{'=' * 50}")
        print(output)


if __name__ == '__main__':
    main()
