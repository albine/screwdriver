#!/bin/bash
# ============================================================================
# 高频市场数据 ClickHouse 导入脚本
# ============================================================================
# 用法:
#   ./import_to_clickhouse.sh /data/raw/2026/01/18
#   ./import_to_clickhouse.sh /data/raw/2026/01/18 --dry-run
#
# 环境变量:
#   CLICKHOUSE_HOST: ClickHouse 服务器地址 (默认 localhost)
#   CLICKHOUSE_PORT: ClickHouse 端口 (默认 9000)
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${1:-}"
DRY_RUN="${2:-}"

if [ -z "$DATA_DIR" ]; then
    echo "Usage: $0 <data_dir> [--dry-run]"
    echo "  data_dir: Path to daily data directory (e.g., /data/raw/2026/01/18)"
    echo ""
    echo "Example:"
    echo "  $0 /data/raw/2026/01/18"
    echo "  $0 /data/raw/\$(date +%Y/%m/%d)"
    exit 1
fi

if [ ! -d "$DATA_DIR" ]; then
    echo "Error: Directory not found: $DATA_DIR"
    exit 1
fi

echo "============================================"
echo "ClickHouse Market Data Import"
echo "============================================"
echo "Data directory: $DATA_DIR"
echo "Time: $(date)"
echo ""

# 检查文件存在性
check_file() {
    local file="$1"
    if [ -f "$file" ]; then
        local size=$(ls -lh "$file" | awk '{print $5}')
        echo "  [OK] $file ($size)"
        return 0
    else
        echo "  [--] $file (not found)"
        return 1
    fi
}

echo "Checking files:"
check_file "$DATA_DIR/orders.bin" || true
check_file "$DATA_DIR/transactions.bin" || true
check_file "$DATA_DIR/ticks.bin" || true
check_file "$DATA_DIR/snapshots.bin" || true
echo ""

# 读取文件头信息
read_header() {
    local file="$1"
    if [ -f "$file" ]; then
        # 读取 magic (4 bytes) + version (2 bytes) + struct_size (2 bytes) + record_count (8 bytes)
        python3 -c "
import struct
with open('$file', 'rb') as f:
    magic, ver, size, count, offset = struct.unpack('<IHHQQ', f.read(24))
    print(f'  Magic: 0x{magic:08X}  Version: {ver}  StructSize: {size}  Records: {count:,}')
"
    fi
}

echo "File headers:"
echo "orders.bin:"
read_header "$DATA_DIR/orders.bin"
echo "transactions.bin:"
read_header "$DATA_DIR/transactions.bin"
echo "ticks.bin:"
read_header "$DATA_DIR/ticks.bin"
echo "snapshots.bin:"
read_header "$DATA_DIR/snapshots.bin"
echo ""

# 调用 Python 导入脚本
echo "Starting import..."
python3 "$SCRIPT_DIR/import_to_clickhouse.py" "$DATA_DIR" $DRY_RUN

echo ""
echo "============================================"
echo "Import completed at $(date)"
echo "============================================"
