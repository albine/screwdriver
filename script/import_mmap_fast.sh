#!/bin/bash
# import_mmap_fast.sh - Fast C++ based import tool for mmap files to ClickHouse
#
# Usage:
#   ./script/import_mmap_fast.sh /path/to/data/dir [options]
#
# Options:
#   --password PWD     ClickHouse password
#   --host HOST        ClickHouse host (default: localhost)
#   --database DB      ClickHouse database (default: default)
#   --threads N        Number of threads (default: 16)
#   --only TYPE        Import only specified type (orders, transactions, ticks)
#   --limit N          Limit records per file (for testing)
#   --dry-run          Parse files but don't import
#
# Examples:
#   ./script/import_mmap_fast.sh /path/to/data/raw/2026/01/23 --password mypassword
#   ./script/import_mmap_fast.sh /path/to/data --only orders --limit 1000 --dry-run

set -e

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default values
DATA_DIR=""
PASSWORD=""
HOST="localhost"
DATABASE="default"
THREADS=16
ONLY=""
LIMIT=0
DRY_RUN=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --password)
            PASSWORD="$2"
            shift 2
            ;;
        --host)
            HOST="$2"
            shift 2
            ;;
        --database)
            DATABASE="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --only)
            ONLY="$2"
            shift 2
            ;;
        --limit)
            LIMIT="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        -h|--help)
            head -30 "$0" | tail -27
            exit 0
            ;;
        -*)
            echo "Unknown option: $1"
            exit 1
            ;;
        *)
            if [[ -z "$DATA_DIR" ]]; then
                DATA_DIR="$1"
            else
                echo "Unexpected argument: $1"
                exit 1
            fi
            shift
            ;;
    esac
done

# Validate data directory
if [[ -z "$DATA_DIR" ]]; then
    echo "Error: Data directory not specified"
    echo "Usage: $0 /path/to/data/dir [options]"
    exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
    echo "Error: Directory not found: $DATA_DIR"
    exit 1
fi

# Check for binary
BINARY="$PROJECT_ROOT/bin/mmap_to_clickhouse"
if [[ ! -x "$BINARY" ]]; then
    echo "Error: Binary not found: $BINARY"
    echo "Please build first: ./build.sh mmap_to_clickhouse"
    exit 1
fi

# Build clickhouse-client arguments
CH_ARGS="--host $HOST --database $DATABASE"
if [[ -n "$PASSWORD" ]]; then
    CH_ARGS="$CH_ARGS --password $PASSWORD"
fi

# Build mmap_to_clickhouse arguments
TOOL_ARGS="--threads $THREADS"
if [[ $LIMIT -gt 0 ]]; then
    TOOL_ARGS="$TOOL_ARGS --limit $LIMIT"
fi

# File type to table mapping
declare -A TYPE_TABLE=(
    [orders]="MDOrderStruct"
    [transactions]="MDTransactionStruct"
    [ticks]="MDStockStruct"
)

declare -A TYPE_FILE=(
    [orders]="orders.bin"
    [transactions]="transactions.bin"
    [ticks]="ticks.bin"
)

echo "============================================================"
echo "mmap to ClickHouse Fast Import (C++)"
echo "============================================================"
echo "Data directory: $DATA_DIR"
echo "ClickHouse: $HOST:$DATABASE"
echo "Threads: $THREADS"
echo "Limit: $([[ $LIMIT -gt 0 ]] && echo "$LIMIT" || echo "unlimited")"
echo "Dry run: $DRY_RUN"
echo ""

# Determine which types to process
if [[ -n "$ONLY" ]]; then
    TYPES=("$ONLY")
else
    TYPES=("orders" "transactions" "ticks")
fi

TOTAL_RECORDS=0

for type in "${TYPES[@]}"; do
    FILE="$DATA_DIR/${TYPE_FILE[$type]}"
    TABLE="${TYPE_TABLE[$type]}"

    if [[ ! -f "$FILE" ]]; then
        echo "SKIP: ${TYPE_FILE[$type]} not found"
        continue
    fi

    echo ""
    echo "Processing ${TYPE_FILE[$type]} -> $TABLE"

    if [[ "$DRY_RUN" == "true" ]]; then
        # Dry run: just parse and count
        echo "  [DRY-RUN] Parsing $FILE..."
        COUNT=$("$BINARY" --type "$type" $TOOL_ARGS "$FILE" | wc -l)
        echo "  [DRY-RUN] Would import $COUNT records to $TABLE"
    else
        # Real import: pipe to clickhouse-client
        echo "  Importing to $TABLE..."
        START_TIME=$(date +%s.%N)

        # Run the import and capture count
        COUNT=$("$BINARY" --type "$type" $TOOL_ARGS "$FILE" | \
            tee >(clickhouse-client $CH_ARGS --query "INSERT INTO $TABLE FORMAT TabSeparated" >/dev/null) | \
            wc -l)

        END_TIME=$(date +%s.%N)
        ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)

        echo "  [OK] Imported $COUNT records in ${ELAPSED}s"

        # Calculate throughput
        if (( $(echo "$ELAPSED > 0" | bc -l) )); then
            THROUGHPUT=$(echo "scale=0; $COUNT / $ELAPSED" | bc)
            echo "  Throughput: $THROUGHPUT records/sec"
        fi
    fi

    TOTAL_RECORDS=$((TOTAL_RECORDS + COUNT))
done

echo ""
echo "============================================================"
echo "Summary"
echo "============================================================"
echo "Total records: $TOTAL_RECORDS"

if [[ "$DRY_RUN" == "true" ]]; then
    echo ""
    echo "[DRY-RUN] No data was actually imported."
fi
