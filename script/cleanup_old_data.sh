#!/bin/bash
# ============================================================================
# 市场数据清理脚本
# ============================================================================
# 功能:
#   - 保留最近 N 天的热数据
#   - 将超过 N 天的数据压缩归档到 ARCHIVE_DIR
#   - 可选删除已归档的原始数据
#
# 用法:
#   ./cleanup_old_data.sh [--days N] [--archive-dir DIR] [--dry-run]
#
# 环境变量:
#   DATA_DIR:    原始数据目录 (默认 /data/raw)
#   ARCHIVE_DIR: 归档目录 (默认 /data/archive)
#   KEEP_DAYS:   保留天数 (默认 5)
#
# 示例:
#   ./cleanup_old_data.sh                      # 使用默认配置
#   ./cleanup_old_data.sh --days 7             # 保留 7 天
#   ./cleanup_old_data.sh --dry-run            # 仅显示要处理的目录
#
# 建议配置 cron 每日执行:
#   0 3 * * * /path/to/cleanup_old_data.sh >> /var/log/market_data_cleanup.log 2>&1
# ============================================================================

set -e

# 默认配置 (相对路径，适合本地开发；生产环境通过环境变量覆盖)
DATA_DIR="${DATA_DIR:-data/raw}"
ARCHIVE_DIR="${ARCHIVE_DIR:-data/archive}"
KEEP_DAYS="${KEEP_DAYS:-5}"
DRY_RUN=false

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --days)
            KEEP_DAYS="$2"
            shift 2
            ;;
        --data-dir)
            DATA_DIR="$2"
            shift 2
            ;;
        --archive-dir)
            ARCHIVE_DIR="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--days N] [--data-dir DIR] [--archive-dir DIR] [--dry-run]"
            echo ""
            echo "Options:"
            echo "  --days N          保留最近 N 天的数据 (默认: 5)"
            echo "  --data-dir DIR    原始数据目录 (默认: /data/raw)"
            echo "  --archive-dir DIR 归档目录 (默认: /data/archive)"
            echo "  --dry-run         仅显示要处理的目录，不实际执行"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "============================================"
echo "Market Data Cleanup"
echo "============================================"
echo "Time:         $(date)"
echo "Data dir:     $DATA_DIR"
echo "Archive dir:  $ARCHIVE_DIR"
echo "Keep days:    $KEEP_DAYS"
echo "Dry run:      $DRY_RUN"
echo ""

# 检查数据目录
if [ ! -d "$DATA_DIR" ]; then
    echo "Data directory not found: $DATA_DIR"
    exit 1
fi

# 创建归档目录
if [ "$DRY_RUN" = false ]; then
    mkdir -p "$ARCHIVE_DIR"
fi

# 计算截止日期 (YYYYMMDD 格式)
CUTOFF_DATE=$(date -d "-${KEEP_DAYS} days" +%Y%m%d)
echo "Cutoff date: $CUTOFF_DATE (archiving data older than this)"
echo ""

# 查找并处理旧数据目录
# 目录结构: /data/raw/YYYY/MM/DD/
archived_count=0
archived_size=0

find "$DATA_DIR" -mindepth 3 -maxdepth 3 -type d | sort | while read -r dir; do
    # 提取日期 (YYYYMMDD)
    year=$(basename "$(dirname "$(dirname "$dir")")")
    month=$(basename "$(dirname "$dir")")
    day=$(basename "$dir")

    # 验证日期格式
    if [[ ! "$year" =~ ^[0-9]{4}$ ]] || [[ ! "$month" =~ ^[0-9]{2}$ ]] || [[ ! "$day" =~ ^[0-9]{2}$ ]]; then
        continue
    fi

    date_str="${year}${month}${day}"

    # 跳过新数据
    if [[ "$date_str" > "$CUTOFF_DATE" ]] || [[ "$date_str" == "$CUTOFF_DATE" ]]; then
        echo "  [KEEP]    $dir ($date_str)"
        continue
    fi

    # 计算目录大小
    dir_size=$(du -sh "$dir" 2>/dev/null | cut -f1)

    # 归档文件路径
    archive_file="$ARCHIVE_DIR/${date_str}.tar.zst"

    if [ "$DRY_RUN" = true ]; then
        echo "  [ARCHIVE] $dir ($date_str, $dir_size) -> $archive_file"
    else
        echo "  [ARCHIVE] $dir ($date_str, $dir_size)"

        # 压缩归档 (使用 zstd，高压缩比 + 快速)
        if command -v zstd &> /dev/null; then
            tar -c -C "$(dirname "$dir")" "$(basename "$dir")" | zstd -T0 -19 -o "$archive_file"
        else
            # fallback 到 gzip
            echo "  [WARN] zstd not found, using gzip instead"
            archive_file="$ARCHIVE_DIR/${date_str}.tar.gz"
            tar -czf "$archive_file" -C "$(dirname "$dir")" "$(basename "$dir")"
        fi

        # 验证归档文件
        if [ -f "$archive_file" ]; then
            archive_size=$(ls -lh "$archive_file" | awk '{print $5}')
            echo "  [OK]      Created $archive_file ($archive_size)"

            # 删除原始目录
            rm -rf "$dir"
            echo "  [OK]      Removed $dir"

            ((archived_count++)) || true
        else
            echo "  [ERROR]   Failed to create archive for $dir"
        fi
    fi
done

echo ""
echo "============================================"
echo "Cleanup Summary"
echo "============================================"
echo "Archived directories: $archived_count"
echo ""

# 显示当前磁盘使用情况
echo "Current disk usage:"
df -h "$DATA_DIR" 2>/dev/null || true
echo ""
df -h "$ARCHIVE_DIR" 2>/dev/null || true
echo ""

# 显示数据目录内容
echo "Remaining data directories:"
find "$DATA_DIR" -mindepth 3 -maxdepth 3 -type d 2>/dev/null | sort | while read -r dir; do
    size=$(du -sh "$dir" 2>/dev/null | cut -f1)
    echo "  $dir ($size)"
done

echo ""
echo "Cleanup completed at $(date)"
