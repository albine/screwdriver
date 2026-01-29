#!/bin/bash
#
# Trading Engine Deployment Script
# Usage: ./deploy.sh [options]
#

set -e

# ============================================
# Configuration
# ============================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_NAME="trading-engine"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PACKAGE_NAME="${PROJECT_NAME}-${TIMESTAMP}.tar.gz"

# Strategy config sync settings
CONFIG_REPO_URL="https://github.com/albine/helloworld"
CONFIG_REPO_BRANCH="pub_20260121"
CONFIG_REPO_DIR="${SCRIPT_DIR}/.config_repo"
TODAY_DATE=$(date +%Y%m%d)

# Default values (参考 fastfish/sync.sh)
SSH_CONF="$HOME/.ssh/config"
REMOTE_HOST="market-m"
REMOTE_DEST="/home/jiace/project/trading-engine"
PACK_ONLY=false

# Source directories
BUILD_DIR="${SCRIPT_DIR}/build"
LIBS_DIR="${SCRIPT_DIR}/fastfish/libs"
CONFIG_DIR="${SCRIPT_DIR}/config"
CERT_DIR="${SCRIPT_DIR}/fastfish/cert"

# Required shared libraries
REQUIRED_LIBS=(
    "libprotobuf.so.11.0.0"
    "libmdc_gateway_client.so"
    "libssl.so.1.0.2k"
    "libcrypto.so.1.0.2k"
    "libaeron_client_shared.so"
    "libaeron_driver.so"
    "libACE.so.6.4.3"
    "libACE_SSL.so.6.4.3"
    "libzmq.so.5.0.0"
    "libsodium.so.23.3.0"
    "libpgm-5.2.so.0.0.122"
)

# Symbolic links to create
declare -A SYMLINKS=(
    ["libprotobuf.so.11"]="libprotobuf.so.11.0.0"
    ["libprotobuf.so"]="libprotobuf.so.11.0.0"
    ["libssl.so.10"]="libssl.so.1.0.2k"
    ["libssl.so"]="libssl.so.1.0.2k"
    ["libcrypto.so.10"]="libcrypto.so.1.0.2k"
    ["libcrypto.so"]="libcrypto.so.1.0.2k"
    ["libACE.so"]="libACE.so.6.4.3"
    ["libACE_SSL.so"]="libACE_SSL.so.6.4.3"
    ["libzmq.so.5"]="libzmq.so.5.0.0"
    ["libzmq.so"]="libzmq.so.5.0.0"
    ["libsodium.so.23"]="libsodium.so.23.3.0"
    ["libsodium.so"]="libsodium.so.23.3.0"
    ["libpgm-5.2.so.0"]="libpgm-5.2.so.0.0.122"
)

# ============================================
# Functions
# ============================================
usage() {
    cat << EOF
Usage: $0 [options]

Options:
    -h, --host HOST       Remote server alias in ~/.ssh/config (default: market-m)
    -d, --dest PATH       Remote deployment path (default: /home/jiace/project/trading-engine)
    -p, --pack-only       Only create package, don't upload
    -n, --dryrun          Dry run mode: sync config and create package, but don't upload
    --help                Show this help message

Examples:
    $0 --pack-only                           # Only create package
    $0 --dryrun                              # Sync config + create package, no upload
    $0                                       # Deploy to market-m (default)
    $0 --host other-server                   # Deploy to other server

Note: SSH connection uses ~/.ssh/config, same as fastfish/sync.sh

EOF
    exit 0
}

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

# Sync strategy config from remote repository
sync_strategy_config() {
    log_info "Syncing strategy config from remote repository..."

    # Clean up any existing repo
    if [[ -d "${CONFIG_REPO_DIR}" ]]; then
        rm -rf "${CONFIG_REPO_DIR}"
    fi

    # Shallow clone with limited depth (enough to find today's commit)
    log_info "Cloning config repository (shallow)..."
    git clone --branch "${CONFIG_REPO_BRANCH}" --single-branch --depth 50 "${CONFIG_REPO_URL}" "${CONFIG_REPO_DIR}" --quiet

    # Find commit with today's date in message format "{yyyymmdd}配置文件"
    cd "${CONFIG_REPO_DIR}"
    COMMIT_MSG_PATTERN="${TODAY_DATE}配置文件"
    TARGET_COMMIT=$(git log --oneline --grep="${COMMIT_MSG_PATTERN}" --format="%H" | head -1)

    if [[ -z "${TARGET_COMMIT}" ]]; then
        cd "${SCRIPT_DIR}"
        rm -rf "${CONFIG_REPO_DIR}"
        log_error "No commit found with message pattern '${COMMIT_MSG_PATTERN}'"
    fi
    log_info "Found config commit: ${TARGET_COMMIT} (${COMMIT_MSG_PATTERN})"

    # Checkout the target commit
    git checkout "${TARGET_COMMIT}" --quiet

    # Define file mappings
    declare -A CONFIG_MAPPINGS=(
        ["data/config/linux/99998/strategy_config.csv"]="${CONFIG_DIR}/strategy_live.conf"
        ["data/config/linux/99999/strategy_config.csv"]="${CONFIG_DIR}/strategy_live_dealer2.conf"
    )

    # Process each config file
    for src_file in "${!CONFIG_MAPPINGS[@]}"; do
        dst_file="${CONFIG_MAPPINGS[$src_file]}"

        if [[ ! -f "${src_file}" ]]; then
            cd "${SCRIPT_DIR}"
            rm -rf "${CONFIG_REPO_DIR}"
            log_error "Source file not found: ${src_file}"
        fi

        log_info "Converting ${src_file} -> ${dst_file}"
        convert_strategy_config "${src_file}" "${dst_file}"
        if ! verify_strategy_config "${src_file}" "${dst_file}"; then
            cd "${SCRIPT_DIR}"
            rm -rf "${CONFIG_REPO_DIR}"
            log_error "Strategy config verification failed for ${dst_file}"
        fi
    done

    cd "${SCRIPT_DIR}"

    # Clean up cloned repository
    log_info "Cleaning up temporary config repository..."
    rm -rf "${CONFIG_REPO_DIR}"

    log_info "Strategy config sync completed."
}

# Convert strategy_config.csv to strategy_live.conf format
# Input format (CSV with header): symbol,strategy (multiple strategies separated by |)
# Output format: symbol,strategy (one line per symbol-strategy pair)
convert_strategy_config() {
    local src_file="$1"
    local dst_file="$2"

    # Clear destination file
    > "${dst_file}"

    # Skip header line, process each data line
    tail -n +2 "${src_file}" | while IFS=',' read -r symbol strategies; do
        # Skip empty lines
        [[ -z "${symbol}" ]] && continue

        # Remove any carriage return (Windows line endings)
        strategies=$(echo "${strategies}" | tr -d '\r')
        symbol=$(echo "${symbol}" | tr -d '\r')

        # Split strategies by | and write each pair
        IFS='|' read -ra strategy_array <<< "${strategies}"
        for strategy in "${strategy_array[@]}"; do
            # Trim whitespace
            strategy=$(echo "${strategy}" | xargs)
            [[ -n "${strategy}" ]] && echo "${symbol},${strategy}" >> "${dst_file}"
        done
    done

    log_info "Generated $(wc -l < "${dst_file}") entries in ${dst_file}"
}

# Verify generated config matches source CSV
# Returns 0 if match, 1 if mismatch
verify_strategy_config() {
    local src_file="$1"
    local dst_file="$2"

    # Generate expected content from CSV (skip header, expand | to multiple lines, sort)
    local expected=$(tail -n +2 "${src_file}" | tr -d '\r' | while IFS=',' read -r symbol strategies; do
        [[ -z "${symbol}" ]] && continue
        IFS='|' read -ra strategy_array <<< "${strategies}"
        for strategy in "${strategy_array[@]}"; do
            strategy=$(echo "${strategy}" | xargs)
            [[ -n "${strategy}" ]] && echo "${symbol},${strategy}"
        done
    done | sort)

    # Get actual content from generated file (sort)
    local actual=$(sort "${dst_file}")

    # Compare
    if [[ "${expected}" == "${actual}" ]]; then
        log_info "Verification passed: ${dst_file}"
        return 0
    else
        log_warn "Verification failed: ${dst_file} does not match ${src_file}"
        return 1
    fi
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check engine binary
    if [[ ! -f "${BUILD_DIR}/engine" ]]; then
        log_error "Engine binary not found at ${BUILD_DIR}/engine. Please build first."
    fi

    # Check required libraries
    for lib in "${REQUIRED_LIBS[@]}"; do
        if [[ ! -f "${LIBS_DIR}/${lib}" ]]; then
            log_error "Required library not found: ${LIBS_DIR}/${lib}"
        fi
    done

    # Check config files
    if [[ ! -f "${CONFIG_DIR}/htsc-insight-cpp-config.conf" ]]; then
        log_warn "Gateway config file not found at ${CONFIG_DIR}/htsc-insight-cpp-config.conf"
    fi

    if [[ ! -f "${CONFIG_DIR}/backtest.conf" ]]; then
        log_warn "Strategy config file not found: ${CONFIG_DIR}/backtest.conf"
    fi

    if [[ ! -f "${CONFIG_DIR}/strategy_live.conf" ]]; then
        log_warn "Strategy config file not found: ${CONFIG_DIR}/strategy_live.conf"
    fi

    if [[ ! -f "${CONFIG_DIR}/engine.conf" ]]; then
        log_warn "Engine config file not found: ${CONFIG_DIR}/engine.conf"
    fi

    # Check certificates
    if [[ ! -f "${CERT_DIR}/InsightClientCert.pem" ]] || [[ ! -f "${CERT_DIR}/InsightClientKeyPkcs8.pem" ]]; then
        log_warn "Certificate files not found in ${CERT_DIR}. Live mode may not work."
    fi

    log_info "Prerequisites check passed."
}

create_package() {
    log_info "Creating deployment package..."

    # Create temporary staging directory
    STAGING_DIR=$(mktemp -d)
    trap "rm -rf ${STAGING_DIR}" EXIT

    DEPLOY_ROOT="${STAGING_DIR}/${PROJECT_NAME}"

    # Create directory structure
    mkdir -p "${DEPLOY_ROOT}/bin"
    mkdir -p "${DEPLOY_ROOT}/lib"
    mkdir -p "${DEPLOY_ROOT}/config"
    mkdir -p "${DEPLOY_ROOT}/cert"
    mkdir -p "${DEPLOY_ROOT}/logs"

    # Copy engine binary
    log_info "Copying engine binary..."
    cp "${BUILD_DIR}/engine" "${DEPLOY_ROOT}/bin/"
    chmod +x "${DEPLOY_ROOT}/bin/engine"

    # Copy mmap_to_clickhouse tool (if exists)
    if [[ -f "${BUILD_DIR}/mmap_to_clickhouse" ]]; then
        log_info "Copying mmap_to_clickhouse tool..."
        cp "${BUILD_DIR}/mmap_to_clickhouse" "${DEPLOY_ROOT}/bin/"
        chmod +x "${DEPLOY_ROOT}/bin/mmap_to_clickhouse"
    fi

    # Copy verify_orderbook tool (if exists)
    if [[ -f "${BUILD_DIR}/verify_orderbook" ]]; then
        log_info "Copying verify_orderbook tool..."
        cp "${BUILD_DIR}/verify_orderbook" "${DEPLOY_ROOT}/bin/"
        chmod +x "${DEPLOY_ROOT}/bin/verify_orderbook"
    fi

    # Copy scripts (if exist)
    mkdir -p "${DEPLOY_ROOT}/script"
    if [[ -f "${SCRIPT_DIR}/script/import_mmap_fast.sh" ]]; then
        log_info "Copying import script..."
        cp "${SCRIPT_DIR}/script/import_mmap_fast.sh" "${DEPLOY_ROOT}/script/"
        chmod +x "${DEPLOY_ROOT}/script/import_mmap_fast.sh"
    fi
    if [[ -f "${SCRIPT_DIR}/script/verify_orderbook_rebuild.sh" ]]; then
        log_info "Copying verify_orderbook script..."
        cp "${SCRIPT_DIR}/script/verify_orderbook_rebuild.sh" "${DEPLOY_ROOT}/script/"
        chmod +x "${DEPLOY_ROOT}/script/verify_orderbook_rebuild.sh"
    fi

    # Copy shared libraries
    log_info "Copying shared libraries..."
    for lib in "${REQUIRED_LIBS[@]}"; do
        cp "${LIBS_DIR}/${lib}" "${DEPLOY_ROOT}/lib/"
    done

    # Create symbolic links
    log_info "Creating symbolic links..."
    cd "${DEPLOY_ROOT}/lib"
    for link in "${!SYMLINKS[@]}"; do
        ln -sf "${SYMLINKS[$link]}" "$link"
    done
    cd "${SCRIPT_DIR}"

    # Copy config files
    if [[ -f "${CONFIG_DIR}/htsc-insight-cpp-config.conf" ]]; then
        log_info "Copying gateway config file..."
        cp "${CONFIG_DIR}/htsc-insight-cpp-config.conf" "${DEPLOY_ROOT}/config/"
    fi

    # Copy strategy config files (backtest.conf, strategy_live.conf, strategy_live_dealer2.conf, engine.conf)
    if [[ -d "${CONFIG_DIR}" ]]; then
        log_info "Copying strategy config files..."
        if [[ -f "${CONFIG_DIR}/backtest.conf" ]]; then
            cp "${CONFIG_DIR}/backtest.conf" "${DEPLOY_ROOT}/config/"
        fi
        if [[ -f "${CONFIG_DIR}/strategy_live.conf" ]]; then
            cp "${CONFIG_DIR}/strategy_live.conf" "${DEPLOY_ROOT}/config/"
        fi
        if [[ -f "${CONFIG_DIR}/strategy_live_dealer2.conf" ]]; then
            cp "${CONFIG_DIR}/strategy_live_dealer2.conf" "${DEPLOY_ROOT}/config/"
        fi
        if [[ -f "${CONFIG_DIR}/engine.conf" ]]; then
            cp "${CONFIG_DIR}/engine.conf" "${DEPLOY_ROOT}/config/"
        fi
    fi

    # Copy certificates
    if [[ -f "${CERT_DIR}/InsightClientCert.pem" ]]; then
        log_info "Copying certificates..."
        cp "${CERT_DIR}/InsightClientCert.pem" "${DEPLOY_ROOT}/cert/"
        cp "${CERT_DIR}/InsightClientKeyPkcs8.pem" "${DEPLOY_ROOT}/cert/"
    fi

    # Create run.sh script
    log_info "Creating run.sh script..."
    cat > "${DEPLOY_ROOT}/run.sh" << 'RUNSCRIPT'
#!/bin/bash
#
# Trading Engine Startup Script
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set library path
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH}"

# Check required environment variables for live mode
check_env() {
    local missing=()
    [[ -z "${FF_USER}" ]] && missing+=("FF_USER")
    [[ -z "${FF_PASSWORD}" ]] && missing+=("FF_PASSWORD")
    [[ -z "${FF_IP}" ]] && missing+=("FF_IP")
    [[ -z "${FF_PORT}" ]] && missing+=("FF_PORT")
    [[ -z "${FF_CERT_DIR}" ]] && missing+=("FF_CERT_DIR")

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Missing required environment variables: ${missing[*]}"
        echo ""
        echo "Please set the following in your ~/.bashrc or environment:"
        echo "  export FF_USER=\"your_username\""
        echo "  export FF_PASSWORD=\"your_password\""
        echo "  export FF_IP=\"gateway_ip\""
        echo "  export FF_PORT=\"gateway_port\""
        echo "  export FF_CERT_DIR=\"local_interface_ip\""
        return 1
    fi
    return 0
}

# Show help
if [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    echo "Usage: $0 [backtest|live]"
    echo ""
    echo "Modes:"
    echo "  backtest  - Run in backtest mode (default)"
    echo "  live      - Run in live trading mode (requires environment variables)"
    echo ""
    echo "Environment variables for live mode:"
    echo "  FF_USER       - Gateway username"
    echo "  FF_PASSWORD   - Gateway password"
    echo "  FF_IP         - Gateway IP address"
    echo "  FF_PORT       - Gateway port"
    echo "  FF_CERT_DIR   - Local UDP interface IP"
    exit 0
fi

MODE="${1:-backtest}"

# Check environment for live mode
if [[ "$MODE" == "live" ]]; then
    if ! check_env; then
        exit 1
    fi
fi

# Ensure logs directory exists
mkdir -p "${SCRIPT_DIR}/logs"

# Change to script directory
cd "${SCRIPT_DIR}"

# Run engine
echo "Starting trading engine in ${MODE} mode..."
exec "${SCRIPT_DIR}/bin/engine" "$MODE"
RUNSCRIPT
    chmod +x "${DEPLOY_ROOT}/run.sh"

    # Create the tarball
    log_info "Creating tarball: ${PACKAGE_NAME}"
    cd "${STAGING_DIR}"
    tar -czf "${SCRIPT_DIR}/${PACKAGE_NAME}" "${PROJECT_NAME}"
    cd "${SCRIPT_DIR}"

    # Show package info
    PACKAGE_SIZE=$(du -h "${PACKAGE_NAME}" | cut -f1)
    log_info "Package created: ${PACKAGE_NAME} (${PACKAGE_SIZE})"
}

deploy_to_server() {
    if [[ -z "${REMOTE_HOST}" ]]; then
        log_error "Remote host not specified. Use --host option."
    fi

    log_info "Deploying to ${REMOTE_HOST}:${REMOTE_DEST}..."

    # Create remote directory structure
    log_info "Creating remote directory..."
    ssh -F "${SSH_CONF}" "${REMOTE_HOST}" "mkdir -p ${REMOTE_DEST}"

    # Upload package using rsync (same as fastfish/sync.sh)
    log_info "Uploading package via rsync..."
    rsync -avz -e "ssh -F ${SSH_CONF}" "${PACKAGE_NAME}" "${REMOTE_HOST}:/tmp/"

    # Extract on remote server
    log_info "Extracting on remote server..."
    ssh -F "${SSH_CONF}" "${REMOTE_HOST}" << EOF
        set -e

        # Backup existing deployment if exists
        if [[ -d "${REMOTE_DEST}/bin" ]]; then
            BACKUP_DIR="${REMOTE_DEST}.backup.\$(date +%Y%m%d_%H%M%S)"
            echo "Backing up existing deployment to \${BACKUP_DIR}..."
            mv "${REMOTE_DEST}" "\${BACKUP_DIR}"
            mkdir -p "${REMOTE_DEST}"
        fi

        # Extract new deployment
        cd "${REMOTE_DEST}"
        tar -xzf "/tmp/${PACKAGE_NAME}" --strip-components=1

        # Cleanup
        rm -f "/tmp/${PACKAGE_NAME}"

        echo ""
        echo "Deployment completed successfully!"
        echo ""
        echo "To run the engine:"
        echo "  cd ${REMOTE_DEST}"
        echo "  ./run.sh backtest    # for backtest mode"
        echo "  ./run.sh live        # for live mode (set env vars first)"
EOF

    log_info "Deployment completed!"
}

# ============================================
# Main
# ============================================
# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)
            REMOTE_HOST="$2"
            shift 2
            ;;
        -d|--dest)
            REMOTE_DEST="$2"
            shift 2
            ;;
        -p|--pack-only)
            PACK_ONLY=true
            shift
            ;;
        -n|--dryrun)
            PACK_ONLY=true
            shift
            ;;
        --help)
            usage
            ;;
        *)
            log_error "Unknown option: $1"
            ;;
    esac
done

# Run
sync_strategy_config
check_prerequisites
create_package

if [[ "${PACK_ONLY}" == false ]]; then
    deploy_to_server
else
    log_info "Pack-only mode. Skipping deployment."
    log_info "Package file: ${SCRIPT_DIR}/${PACKAGE_NAME}"
fi

log_info "Done!"
