#!/bin/bash
#
# Trading Engine - Local Build Script
# 用于本地开发和回测测试
#
# 注意：
#   - 本脚本编译的二进制文件仅用于本地运行（回测）
#   - 如需部署到生产服务器（CentOS 7），请使用: ./docker-build.sh
#   - 原因：生产服务器 glibc 版本较低，需要在 CentOS 7 容器中编译
#

set -e  # 任何命令失败时退出

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${GREEN}[BUILD]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

# 设置环境变量（必需，用于运行时库查找）
export LD_LIBRARY_PATH="${PROJECT_ROOT}/fastfish/libs:${LD_LIBRARY_PATH}"

# 解析参数
BUILD_TYPE="Release"
TARGET=""
CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --prod)
            print_error "For production build, use: ./docker-build.sh"
            exit 1
            ;;
        -h|--help)
            cat << EOF
Usage: $0 [options] [target]

⚠️  LOCAL BUILD ONLY - For production deployment, use: ./docker-build.sh

Options:
    --debug         Build with Debug configuration (default: Release)
    --clean         Clean build directory before building
    -h, --help      Show this help message

Targets:
    engine                      Main trading engine
    test_logger                 Logger functionality tests
    test_fastorderbook          OrderBook replay tests
    test_data_order             Data ordering verification
    test_price_level_strategy   Price level monitoring tests
    (no target)                 Build all targets

Examples:
    $0                          # Build all targets in Release mode
    $0 engine                   # Build only engine
    $0 --debug engine           # Build engine with debug symbols
    $0 --clean                  # Clean build and rebuild all

EOF
            exit 0
            ;;
        *)
            TARGET="$1"
            shift
            ;;
    esac
done

# 显示构建信息
print_status "Trading Engine Build System (Local)"
print_warning "This build is for LOCAL TESTING ONLY"
print_warning "For production deployment, use: ./docker-build.sh"
echo ""
print_status "Project Root: $PROJECT_ROOT"
print_status "Build Type: $BUILD_TYPE"
if [ -n "$TARGET" ]; then
    print_status "Target: $TARGET"
else
    print_status "Target: ALL"
fi

# 清理构建目录（如果需要）
if [ "$CLEAN" = true ]; then
    print_warning "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# 创建构建目录
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
    print_status "Created build directory"
fi

# 进入构建目录
cd "$BUILD_DIR"

# 运行 CMake 配置
print_status "Running CMake configuration..."
cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE .. || {
    print_error "CMake configuration failed"
    exit 1
}

# 编译
if [ -n "$TARGET" ]; then
    print_status "Building target: $TARGET"
    make "$TARGET" -j$(nproc) || {
        print_error "Build failed for target: $TARGET"
        exit 1
    }
else
    print_status "Building all targets..."
    make -j$(nproc) || {
        print_error "Build failed"
        exit 1
    }
fi

# 显示编译结果
echo ""
print_status "Build completed successfully!"
print_status "Executable locations:"

# 列出所有可执行文件
for target in engine test_logger test_fastorderbook test_data_order test_price_level_strategy; do
    if [ -f "$BUILD_DIR/$target" ]; then
        SIZE=$(du -h "$BUILD_DIR/$target" | cut -f1)
        echo "  ✓ $target ($SIZE)"
    fi
done

# 提示如何运行
echo ""
print_status "To run locally (backtest mode):"
echo "  # Method 1: Use Python helper (recommended)"
echo "  python run_backtest.py 20260114"
echo ""
echo "  # Method 2: Run directly"
echo "  export LD_LIBRARY_PATH=\$PWD/fastfish/libs:\$LD_LIBRARY_PATH"
echo "  ./build/engine backtest"
echo ""
print_warning "For production deployment:"
echo "  1. Build with Docker: ./docker-build.sh"
echo "  2. Deploy to server: ./deploy.sh"
