#!/bin/bash
#
# 从远程服务器下载股票市场数据
# 用法: ./download_market_data.sh <股票代码> [日期]
# 示例: ./download_market_data.sh 002777.SZ
#       ./download_market_data.sh 002777.SZ 20260112
#

set -e

# 配置
SSH_CONF="$HOME/.ssh/config"
REMOTE_HOST="market-m"
REMOTE_BASE_DIR="/home/jiace/project/fastfish"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/test_data"

# 颜色输出
log_info() { echo -e "\033[32m[INFO]\033[0m $1"; }
log_error() { echo -e "\033[31m[ERROR]\033[0m $1"; exit 1; }

# 帮助信息
usage() {
    cat << EOF
用法: $0 <股票代码> [日期]

参数:
    股票代码    格式如 002777.SZ 或 600000.SH
    日期        可选，格式如 20260112，默认使用最新的 export_folder

示例:
    $0 002777.SZ              # 下载 002777.SZ 的数据（使用最新目录）
    $0 002777.SZ 20260112     # 下载指定日期的数据
    $0 600000.SH              # 下载上海股票数据

下载文件:
    - MD_ORDER_StockType_<代码>.csv
    - MD_TRANSACTION_StockType_<代码>.csv
    - MD_TICK_StockType_<代码>.csv

EOF
    exit 0
}

# 检查参数
if [[ $# -lt 1 ]] || [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    usage
fi

SECURITY_ID="$1"
DATE="${2:-}"

# 验证股票代码格式
if [[ ! "$SECURITY_ID" =~ ^[0-9]{6}\.(SZ|SH)$ ]]; then
    log_error "股票代码格式错误: $SECURITY_ID (应为 XXXXXX.SZ 或 XXXXXX.SH)"
fi

# 确定远程目录
if [[ -n "$DATE" ]]; then
    REMOTE_DIR="${REMOTE_BASE_DIR}/export_folder_${DATE}"
else
    # 查找最新的 export_folder
    log_info "查找最新的 export_folder..."
    REMOTE_DIR=$(ssh -F "$SSH_CONF" "$REMOTE_HOST" "ls -d ${REMOTE_BASE_DIR}/export_folder_* 2>/dev/null | sort -r | head -1")
    if [[ -z "$REMOTE_DIR" ]]; then
        log_error "未找到 export_folder 目录"
    fi
fi

log_info "远程目录: $REMOTE_DIR"

# 创建本地目录
mkdir -p "$LOCAL_DIR"

# 定义要下载的文件
FILES=(
    "MD_ORDER_StockType_${SECURITY_ID}.csv"
    "MD_TRANSACTION_StockType_${SECURITY_ID}.csv"
    "MD_TICK_StockType_${SECURITY_ID}.csv"
)

# 检查远程文件是否存在
log_info "检查远程文件..."
for FILE in "${FILES[@]}"; do
    REMOTE_PATH="${REMOTE_DIR}/${FILE}"
    if ! ssh -F "$SSH_CONF" "$REMOTE_HOST" "test -f '$REMOTE_PATH'"; then
        log_error "文件不存在: $REMOTE_PATH"
    fi
done

# 下载文件
log_info "开始下载 ${SECURITY_ID} 的市场数据..."
for FILE in "${FILES[@]}"; do
    REMOTE_PATH="${REMOTE_DIR}/${FILE}"
    LOCAL_PATH="${LOCAL_DIR}/${FILE}"

    log_info "下载: $FILE"
    rsync -avz --progress -e "ssh -F $SSH_CONF" "${REMOTE_HOST}:${REMOTE_PATH}" "$LOCAL_PATH"
done

# 显示下载结果
log_info "下载完成！文件保存在: $LOCAL_DIR"
echo ""
ls -lh "$LOCAL_DIR"/*"${SECURITY_ID}"* 2>/dev/null || true
