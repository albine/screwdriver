#!/bin/bash
#
# Docker Build Script for CentOS 7 Compatibility
# 使用 fastfish/docker/Dockerfile 编译，确保与远程服务器兼容
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="trading-engine-builder"
CONTAINER_NAME="trading-engine-build"
DOCKERFILE_PATH="${SCRIPT_DIR}/fastfish/docker/Dockerfile"

log_info() {
    echo -e "\033[32m[INFO]\033[0m $1"
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

log_info "Building Docker image using fastfish/docker/Dockerfile..."

# Build Docker image
docker build -t "${IMAGE_NAME}" -f "${DOCKERFILE_PATH}" "${SCRIPT_DIR}/fastfish/docker"

# Remove old container if exists
docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

# Run build in container
log_info "Starting build in CentOS 7 container..."
docker run --name "${CONTAINER_NAME}" \
    -v "${SCRIPT_DIR}:/build" \
    -w /build \
    "${IMAGE_NAME}" \
    bash -c "
        set -e
        echo '=== GCC Version ==='
        gcc --version | head -1

        echo '=== CMake Version ==='
        cmake --version | head -1

        echo '=== Building engine (Release) ==='
        rm -rf build_centos7
        mkdir -p build_centos7
        cd build_centos7

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
