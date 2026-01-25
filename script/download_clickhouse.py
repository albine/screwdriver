#!/usr/bin/env python3
"""
ClickHouse TGZ 包下载、部署和安装脚本

功能：
1. 下载 ClickHouse TGZ 安装包到本地
2. 通过 SCP 传输到远程服务器
3. 在远程服务器上解压
4. 安装 ClickHouse（执行 doinst.sh）
5. 配置数据目录软链接（使用大分区）

用法：
    python script/download_clickhouse.py [--version VERSION] [--download-only] [--install]
"""

import argparse
import subprocess
import sys
import os
from pathlib import Path

# 配置
REMOTE_HOST = "market-m"
REMOTE_DIR = "/home/jiace/download"
LOCAL_DOWNLOAD_DIR = Path("/tmp/clickhouse_download")
ARCH = "amd64"

# 数据目录配置（使用大分区）
DATA_BASE_DIR = "/home/server_data/clickhouse"
DATA_DIR = f"{DATA_BASE_DIR}/data"
LOG_DIR = f"{DATA_BASE_DIR}/logs"

# 需要下载的包
PACKAGES = [
    "clickhouse-common-static",
    "clickhouse-server",
    "clickhouse-client",
]

# 可选包
OPTIONAL_PACKAGES = [
    "clickhouse-common-static-dbg",  # 调试符号
]

BASE_URL = "https://packages.clickhouse.com/tgz/stable"


def run_cmd(cmd: list[str], check: bool = True, capture: bool = False) -> subprocess.CompletedProcess:
    """执行命令"""
    print(f"[CMD] {' '.join(cmd)}")
    return subprocess.run(cmd, check=check, capture_output=capture, text=True)


def get_latest_version() -> str:
    """获取最新稳定版本号"""
    print("[INFO] 获取最新版本号...")
    url = "https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/utils/list-versions/version_date.tsv"
    result = run_cmd(["curl", "-s", url], capture=True)

    versions = []
    for line in result.stdout.strip().split('\n'):
        parts = line.split('\t')
        if parts:
            version = parts[0].strip()
            # 过滤有效版本号格式（格式：v25.12.4.35-stable）
            if version and version.startswith('v') and '-stable' in version:
                # 提取纯版本号：去掉 v 前缀和 -stable 后缀
                clean_version = version[1:].replace('-stable', '')
                versions.append(clean_version)

    if not versions:
        raise RuntimeError("无法获取版本列表")

    # 按版本号排序，取最新
    versions.sort(key=lambda v: [int(x) for x in v.split('.')], reverse=True)
    return versions[0]


def download_packages(version: str, include_dbg: bool = False) -> list[Path]:
    """下载 TGZ 包到本地"""
    LOCAL_DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)

    packages = PACKAGES.copy()
    if include_dbg:
        packages.extend(OPTIONAL_PACKAGES)

    downloaded = []

    for pkg in packages:
        filename = f"{pkg}-{version}-{ARCH}.tgz"
        url = f"{BASE_URL}/{filename}"
        local_path = LOCAL_DOWNLOAD_DIR / filename

        if local_path.exists():
            print(f"[SKIP] {filename} 已存在")
            downloaded.append(local_path)
            continue

        print(f"[DOWN] 下载 {filename}...")
        try:
            run_cmd(["curl", "-fL", "-o", str(local_path), url])
            downloaded.append(local_path)
            print(f"[OK] {filename} 下载完成")
        except subprocess.CalledProcessError:
            print(f"[WARN] {filename} 下载失败，尝试不带架构后缀...")
            # 某些包可能不带架构后缀
            filename_alt = f"{pkg}-{version}.tgz"
            url_alt = f"{BASE_URL}/{filename_alt}"
            local_path_alt = LOCAL_DOWNLOAD_DIR / filename_alt
            try:
                run_cmd(["curl", "-fL", "-o", str(local_path_alt), url_alt])
                downloaded.append(local_path_alt)
                print(f"[OK] {filename_alt} 下载完成")
            except subprocess.CalledProcessError:
                print(f"[ERROR] {pkg} 下载失败")
                raise

    return downloaded


def upload_to_remote(files: list[Path]) -> None:
    """上传文件到远程服务器"""
    print(f"\n[INFO] 创建远程目录 {REMOTE_HOST}:{REMOTE_DIR}")
    run_cmd(["ssh", REMOTE_HOST, f"mkdir -p {REMOTE_DIR}"])

    for f in files:
        print(f"[SCP] 上传 {f.name}...")
        run_cmd(["scp", str(f), f"{REMOTE_HOST}:{REMOTE_DIR}/"])
        print(f"[OK] {f.name} 上传完成")


def extract_on_remote(version: str) -> None:
    """在远程服务器上解压"""
    print(f"\n[INFO] 在远程服务器上解压...")

    # 构建解压命令
    extract_cmds = []
    for pkg in PACKAGES:
        filename = f"{pkg}-{version}-{ARCH}.tgz"
        extract_cmds.append(f"cd {REMOTE_DIR} && tar -xzf {filename}")

    full_cmd = " && ".join(extract_cmds)
    run_cmd(["ssh", REMOTE_HOST, full_cmd])
    print("[OK] 解压完成")

    # 显示解压后的目录
    print("\n[INFO] 解压后的目录结构:")
    run_cmd(["ssh", REMOTE_HOST, f"ls -la {REMOTE_DIR}/"])


def install_on_remote(version: str) -> None:
    """在远程服务器上安装 ClickHouse"""
    print(f"\n[INFO] 在远程服务器上安装 ClickHouse...")

    # 按顺序执行安装脚本
    install_cmds = [
        f"cd {REMOTE_DIR} && sudo clickhouse-common-static-{version}/install/doinst.sh",
        f"cd {REMOTE_DIR} && sudo clickhouse-server-{version}/install/doinst.sh",
        f"cd {REMOTE_DIR} && sudo clickhouse-client-{version}/install/doinst.sh",
    ]

    for cmd in install_cmds:
        print(f"[INSTALL] 执行: {cmd.split('&&')[-1].strip()}")
        run_cmd(["ssh", "-t", REMOTE_HOST, cmd])

    print("[OK] ClickHouse 安装完成")


def setup_symlinks() -> None:
    """配置数据目录软链接（使用大分区）"""
    print(f"\n[INFO] 配置数据目录软链接...")

    cmds = [
        # 创建数据目录
        f"mkdir -p {DATA_DIR}",
        f"mkdir -p {LOG_DIR}",

        # 删除原有目录（如果存在）
        "sudo rm -rf /var/lib/clickhouse",
        "sudo rm -rf /var/log/clickhouse-server",

        # 创建软链接
        f"sudo ln -s {DATA_DIR} /var/lib/clickhouse",
        f"sudo ln -s {LOG_DIR} /var/log/clickhouse-server",

        # 设置权限
        f"sudo chown -R clickhouse:clickhouse {DATA_BASE_DIR}",
    ]

    for cmd in cmds:
        print(f"[SYMLINK] {cmd}")
        run_cmd(["ssh", REMOTE_HOST, cmd])

    print("[OK] 软链接配置完成")

    # 验证软链接
    print("\n[INFO] 验证软链接:")
    run_cmd(["ssh", REMOTE_HOST, "ls -la /var/lib/clickhouse /var/log/clickhouse-server"])


def create_custom_config() -> None:
    """创建自定义配置文件"""
    print(f"\n[INFO] 创建自定义配置文件...")

    config_content = """<?xml version="1.0"?>
<clickhouse>
    <!-- 时区设置 -->
    <timezone>Asia/Shanghai</timezone>

    <!-- 监听地址，0.0.0.0 允许远程访问 -->
    <listen_host>0.0.0.0</listen_host>

    <!-- 端口配置（使用默认端口） -->
    <http_port>8123</http_port>
    <tcp_port>9000</tcp_port>

    <!-- 压缩配置：ZSTD 压缩 -->
    <compression>
        <case>
            <min_part_size>1024</min_part_size>
            <min_part_size_ratio>0.01</min_part_size_ratio>
            <method>zstd</method>
            <level>12</level>
        </case>
    </compression>

    <!-- 日志配置 -->
    <logger>
        <level>information</level>
        <log>/var/log/clickhouse-server/clickhouse-server.log</log>
        <errorlog>/var/log/clickhouse-server/clickhouse-server.err.log</errorlog>
        <size>1000M</size>
        <count>10</count>
    </logger>

</clickhouse>
"""

    # 将配置写入远程服务器
    config_path = "/etc/clickhouse-server/config.d/custom.xml"

    # 使用 heredoc 写入配置文件
    cmd = f"sudo tee {config_path} > /dev/null << 'EOF'\n{config_content}EOF"
    print(f"[CONFIG] 写入配置文件: {config_path}")
    run_cmd(["ssh", REMOTE_HOST, cmd])

    # 设置权限
    run_cmd(["ssh", REMOTE_HOST, f"sudo chown clickhouse:clickhouse {config_path}"])

    print("[OK] 自定义配置文件创建完成")

    # 显示配置内容
    print("\n[INFO] 配置文件内容:")
    run_cmd(["ssh", REMOTE_HOST, f"cat {config_path}"])


def start_service() -> None:
    """启动 ClickHouse 服务"""
    print(f"\n[INFO] 启动 ClickHouse 服务...")
    run_cmd(["ssh", REMOTE_HOST, "sudo /etc/init.d/clickhouse-server start"])
    print("[OK] 服务已启动")

    # 检查状态
    print("\n[INFO] 检查服务状态:")
    run_cmd(["ssh", REMOTE_HOST, "ps aux | grep clickhouse-server | grep -v grep || echo '服务未运行'"], check=False)


def print_manual_instructions(version: str) -> None:
    """打印手动操作说明"""
    print("\n" + "=" * 60)
    print("部署完成！以下是后续操作说明：")
    print("=" * 60)
    print(f"""
# SSH 登录到服务器
ssh {REMOTE_HOST}

# 验证 ClickHouse 版本
clickhouse-client --version

# 连接数据库（需要输入安装时设置的密码）
clickhouse-client --password

# 测试查询
SELECT version();
SHOW DATABASES;

# 服务管理
sudo /etc/init.d/clickhouse-server status
sudo /etc/init.d/clickhouse-server restart
sudo /etc/init.d/clickhouse-server stop

# 查看日志
tail -f /var/log/clickhouse-server/clickhouse-server.log
""")


def main():
    parser = argparse.ArgumentParser(description="ClickHouse TGZ 包下载、部署和安装")
    parser.add_argument("--version", "-v", help="指定版本号，默认获取最新版")
    parser.add_argument("--download-only", action="store_true", help="仅下载，不上传")
    parser.add_argument("--upload-only", action="store_true", help="仅上传已下载的包")
    parser.add_argument("--include-dbg", action="store_true", help="包含调试符号包")
    parser.add_argument("--install", action="store_true", help="安装并配置软链接")
    parser.add_argument("--start", action="store_true", help="安装后启动服务")
    parser.add_argument("--full", action="store_true", help="完整流程：下载+上传+解压+安装+配置+启动")
    args = parser.parse_args()

    # --full 相当于启用所有步骤
    if args.full:
        args.install = True
        args.start = True

    # 获取版本
    if args.version:
        version = args.version
        print(f"[INFO] 使用指定版本: {version}")
    else:
        version = get_latest_version()
        print(f"[INFO] 最新版本: {version}")

    step = 0

    try:
        if not args.upload_only:
            # 下载
            step += 1
            print(f"\n{'=' * 50}")
            print(f"第{step}步：下载 TGZ 包")
            print(f"{'=' * 50}")
            downloaded_files = download_packages(version, args.include_dbg)
            print(f"\n[OK] 共下载 {len(downloaded_files)} 个文件到 {LOCAL_DOWNLOAD_DIR}")
        else:
            # 仅上传模式，查找已下载的文件
            downloaded_files = list(LOCAL_DOWNLOAD_DIR.glob(f"*-{version}*.tgz"))
            if not downloaded_files:
                print(f"[ERROR] 未找到版本 {version} 的本地文件")
                sys.exit(1)

        if not args.download_only:
            # 上传
            step += 1
            print(f"\n{'=' * 50}")
            print(f"第{step}步：上传到远程服务器")
            print(f"{'=' * 50}")
            upload_to_remote(downloaded_files)

            # 解压
            step += 1
            print(f"\n{'=' * 50}")
            print(f"第{step}步：在远程服务器上解压")
            print(f"{'=' * 50}")
            extract_on_remote(version)

            # 安装
            if args.install:
                step += 1
                print(f"\n{'=' * 50}")
                print(f"第{step}步：安装 ClickHouse")
                print(f"{'=' * 50}")
                install_on_remote(version)

                # 配置软链接
                step += 1
                print(f"\n{'=' * 50}")
                print(f"第{step}步：配置数据目录软链接")
                print(f"{'=' * 50}")
                setup_symlinks()

                # 创建自定义配置文件
                step += 1
                print(f"\n{'=' * 50}")
                print(f"第{step}步：创建自定义配置文件")
                print(f"{'=' * 50}")
                create_custom_config()

                # 启动服务
                if args.start:
                    step += 1
                    print(f"\n{'=' * 50}")
                    print(f"第{step}步：启动服务")
                    print(f"{'=' * 50}")
                    start_service()

            # 打印后续说明
            print_manual_instructions(version)

    except subprocess.CalledProcessError as e:
        print(f"\n[ERROR] 命令执行失败: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"\n[ERROR] {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
