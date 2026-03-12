#!/bin/bash
# Fetch syslog from Axis device and filter for ACAP app messages
#
# Usage: ./scripts/logs.sh [app-name] [device-ip] [user] [pass]
# Example: ./scripts/logs.sh hello_world 192.168.1.193 root pass

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${PROJECT_DIR}/.env.devices"

if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi

APP_NAME="${1:-}"
DEVICE_IP="${2:-${C6110_IP:-192.168.1.193}}"
DEVICE_USER="${3:-${C6110_USER:-root}}"
DEVICE_PASS="${4:-${C6110_PASS:-pass}}"

echo "=== Fetching logs from ${DEVICE_IP} ==="

# Fetch syslog via VAPIX
SYSLOG=$(curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    "http://${DEVICE_IP}/axis-cgi/systemlog.cgi" 2>/dev/null)

if [ -z "$SYSLOG" ]; then
    echo "Error: Could not fetch syslog. Check device IP and credentials."
    exit 1
fi

if [ -n "$APP_NAME" ]; then
    echo "--- Filtering for: ${APP_NAME} ---"
    echo "$SYSLOG" | grep -i "$APP_NAME" | tail -50
else
    echo "--- Last 100 lines ---"
    echo "$SYSLOG" | tail -100
fi
