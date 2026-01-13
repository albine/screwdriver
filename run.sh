#!/bin/bash
#
# Trading Engine Startup Script (本地开发版)
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set library path
export LD_LIBRARY_PATH="${SCRIPT_DIR}/fastfish/libs:${LD_LIBRARY_PATH}"

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

# Run engine
echo "Starting trading engine in ${MODE} mode..."
exec "${SCRIPT_DIR}/build/engine" "$MODE"
