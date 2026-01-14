#!/usr/bin/env python3
"""
自动化回测脚本
用法: python run_backtest.py <日期>
示例: python run_backtest.py 20260112
"""

import sys
import os
import subprocess
import re
from pathlib import Path


class BacktestRunner:
    def __init__(self, date: str):
        self.date = date
        self.project_root = Path(__file__).parent.absolute()
        self.config_file = self.project_root / "config" / "backtest.conf"
        self.download_script = self.project_root / "script" / "download_market_data.sh"
        self.engine_binary = self.project_root / "build" / "engine"
        self.lib_path = self.project_root / "fastfish" / "libs"

    def validate_date(self):
        """验证日期格式"""
        if not re.match(r'^\d{8}$', self.date):
            print(f"错误: 日期格式不正确: {self.date}")
            print("日期格式应为: YYYYMMDD (例如: 20260112)")
            return False
        return True

    def check_files_exist(self):
        """检查必要文件是否存在"""
        if not self.config_file.exists():
            print(f"错误: 配置文件不存在: {self.config_file}")
            return False

        if not self.download_script.exists():
            print(f"错误: 下载脚本不存在: {self.download_script}")
            return False

        if not self.engine_binary.exists():
            print(f"错误: 回测引擎不存在: {self.engine_binary}")
            print("请先运行 ./build.sh 编译项目")
            return False

        return True

    def parse_config(self):
        """解析配置文件，获取股票代码列表"""
        stocks = []

        with open(self.config_file, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()

                # 跳过空行和注释
                if not line or line.startswith('#'):
                    continue

                # 解析 "股票代码,策略名称"
                parts = [p.strip() for p in line.split(',')]
                if len(parts) < 2:
                    continue

                stock_code = parts[0]
                strategy_name = parts[1]

                # 自动补全交易所后缀
                if '.' not in stock_code:
                    if stock_code.startswith('6'):
                        stock_code += '.SH'
                    else:
                        stock_code += '.SZ'

                stocks.append({
                    'code': stock_code,
                    'strategy': strategy_name
                })

        return stocks

    def download_market_data(self, stock_code: str):
        """下载指定股票的行情数据"""
        print(f"\n{'='*60}")
        print(f"下载 {stock_code} 的行情数据 (日期: {self.date})")
        print(f"{'='*60}")

        cmd = [str(self.download_script), stock_code, self.date]

        try:
            result = subprocess.run(
                cmd,
                cwd=str(self.project_root),
                check=True,
                text=True,
                capture_output=False
            )
            print(f"✓ {stock_code} 数据下载成功")
            return True
        except subprocess.CalledProcessError as e:
            print(f"✗ {stock_code} 数据下载失败: {e}")
            return False

    def run_backtest(self):
        """运行回测"""
        print(f"\n{'='*60}")
        print(f"开始回测")
        print(f"{'='*60}")

        # 设置环境变量
        env = os.environ.copy()
        if self.lib_path.exists():
            ld_library_path = str(self.lib_path)
            if 'LD_LIBRARY_PATH' in env:
                env['LD_LIBRARY_PATH'] = f"{ld_library_path}:{env['LD_LIBRARY_PATH']}"
            else:
                env['LD_LIBRARY_PATH'] = ld_library_path

        cmd = [str(self.engine_binary), "backtest"]

        try:
            result = subprocess.run(
                cmd,
                cwd=str(self.project_root),
                env=env,
                check=True,
                text=True
            )
            print(f"\n✓ 回测完成")
            return True
        except subprocess.CalledProcessError as e:
            print(f"\n✗ 回测失败: {e}")
            return False

    def run(self):
        """执行完整的回测流程"""
        print(f"自动化回测脚本")
        print(f"日期: {self.date}")
        print(f"项目路径: {self.project_root}")
        print()

        # 1. 验证日期格式
        if not self.validate_date():
            return 1

        # 2. 检查必要文件
        if not self.check_files_exist():
            return 1

        # 3. 解析配置文件
        print(f"读取配置文件: {self.config_file}")
        stocks = self.parse_config()

        if not stocks:
            print("错误: 配置文件中没有找到有效的股票配置")
            return 1

        print(f"找到 {len(stocks)} 个股票配置:")
        for stock in stocks:
            print(f"  - {stock['code']}: {stock['strategy']}")

        # 4. 下载行情数据
        print(f"\n开始下载行情数据...")
        success_count = 0
        failed_stocks = []

        for stock in stocks:
            if self.download_market_data(stock['code']):
                success_count += 1
            else:
                failed_stocks.append(stock['code'])

        print(f"\n下载结果: {success_count}/{len(stocks)} 成功")
        if failed_stocks:
            print(f"失败的股票: {', '.join(failed_stocks)}")
            print("\n提示: 部分股票数据下载失败，回测程序会跳过这些股票")

        # 5. 运行回测
        if not self.run_backtest():
            return 1

        # 6. 显示日志位置
        print(f"\n{'='*60}")
        print(f"回测日志:")
        print(f"  - 系统日志: logs/backtest.log")
        print(f"  - 业务日志: logs/backtest_biz.log")
        print(f"{'='*60}")

        return 0


def main():
    if len(sys.argv) != 2:
        print("用法: python run_backtest.py <日期>")
        print("示例: python run_backtest.py 20260112")
        return 1

    date = sys.argv[1]
    runner = BacktestRunner(date)
    return runner.run()


if __name__ == "__main__":
    sys.exit(main())
