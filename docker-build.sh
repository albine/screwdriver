#!/bin/bash
#
# Docker Build Script for CentOS 7 Compatibility
# 使用 fastfish/docker/Dockerfile 编译，确保与远程服务器兼容
#
# 用法:
#   ./docker-build.sh              # 普通编译（使用缓存）
#   ./docker-build.sh --clean-cache # 清理依赖缓存后编译
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="trading-engine-builder"
CONTAINER_NAME="trading-engine-build"
DOCKERFILE_PATH="${SCRIPT_DIR}/fastfish/docker/Dockerfile"
DEPS_CACHE_VOLUME="trading-engine-deps-cache"

# 解析参数
CLEAN_CACHE=false
for arg in "$@"; do
    case $arg in
        --clean-cache)
            CLEAN_CACHE=true
            shift
            ;;
        --help|-h)
            echo "用法: ./docker-build.sh [选项]"
            echo ""
            echo "选项:"
            echo "  --clean-cache  清理依赖缓存（quill, json）后重新下载编译"
            echo "  --help, -h     显示此帮助信息"
            exit 0
            ;;
    esac
done

log_info() {
    echo -e "\033[32m[INFO]\033[0m $1"
}

log_warn() {
    echo -e "\033[33m[WARN]\033[0m $1"
}

log_error() {
    echo -e "\033[31m[ERROR]\033[0m $1"
    exit 1
}

# Check Docker
if ! command -v docker &> /dev/null; then
    log_error "Docker is not installed. Please install Docker first."
fi

# Check Dockerfile
if [[ ! -f "${DOCKERFILE_PATH}" ]]; then
    log_error "Dockerfile not found: ${DOCKERFILE_PATH}"
fi

# 清理缓存（如果指定了 --clean-cache）
if [[ "${CLEAN_CACHE}" == "true" ]]; then
    log_warn "Cleaning dependency cache volume: ${DEPS_CACHE_VOLUME}"
    docker volume rm "${DEPS_CACHE_VOLUME}" 2>/dev/null || true
    log_info "Cache cleaned. Dependencies will be re-downloaded."
fi

log_info "Building Docker image using fastfish/docker/Dockerfile..."

# Build Docker image
docker build -t "${IMAGE_NAME}" -f "${DOCKERFILE_PATH}" "${SCRIPT_DIR}/fastfish/docker"

# Remove old container if exists
docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

# Run build in container
# 使用 Docker volume 缓存 FetchContent 下载的依赖（quill, json）
log_info "Starting build in CentOS 7 container..."
docker run --name "${CONTAINER_NAME}" \
    -v "${SCRIPT_DIR}:/build" \
    -v "${DEPS_CACHE_VOLUME}:/build/build_centos7/_deps" \
    -w /build \
    "${IMAGE_NAME}" \
    bash -c "
        set -e
        echo '=== GCC Version ==='
        gcc --version | head -1

        echo '=== CMake Version ==='
        cmake --version | head -1

        echo '=== Building engine (Release) ==='
        mkdir -p build_centos7
        cd build_centos7

        # 只删除编译产物，保留 _deps 缓存
        rm -f engine CMakeCache.txt 2>/dev/null || true
        rm -rf CMakeFiles 2>/dev/null || true

        cmake -DCMAKE_BUILD_TYPE=Release ..
        make engine -j\$(nproc)

        echo ''
        echo '=== Build completed ==='
        ls -lh engine

        echo ''
        echo '=== Checking GLIBC requirements ==='
        objdump -T engine | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | sort -u || true
    "

# Copy result to main build directory
log_info "Copying build result..."
mkdir -p "${SCRIPT_DIR}/build"
cp "${SCRIPT_DIR}/build_centos7/engine" "${SCRIPT_DIR}/build/engine"

# Cleanup container
docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

log_info "Build completed successfully!"
log_info "Binary: ${SCRIPT_DIR}/build/engine"
log_info ""
log_info "Now you can deploy with: ./deploy.sh"
