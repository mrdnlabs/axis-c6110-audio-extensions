#!/bin/bash
# Deploy ACAP application to Axis device
# Uploads .eap via VAPIX, then starts the application.
#
# Usage: ./scripts/deploy.sh <app-dir> [device-ip] [user] [pass]
# Example: ./scripts/deploy.sh hello-world 192.168.1.193 root pass

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${PROJECT_DIR}/.env.devices"

if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi

APP_DIR="${1:?Usage: $0 <app-dir> [device-ip] [user] [pass]}"
DEVICE_IP="${2:-${C6110_IP:-192.168.1.193}}"
DEVICE_USER="${3:-${C6110_USER:-root}}"
DEVICE_PASS="${4:-${C6110_PASS:-pass}}"

APP_PATH="${PROJECT_DIR}/${APP_DIR}"

# Find the .eap file
EAP_FILE=$(find "${APP_PATH}/build" -name "*.eap" -type f 2>/dev/null | head -1)
if [ -z "$EAP_FILE" ]; then
    echo "Error: No .eap file found in ${APP_PATH}/build/"
    echo "Run ./scripts/build.sh ${APP_DIR} first."
    exit 1
fi

# Extract app name from manifest.json
APP_NAME=""
if [ -f "${APP_PATH}/app/manifest.json" ]; then
    APP_NAME=$(python3 -c "import json; print(json.load(open('${APP_PATH}/app/manifest.json'))['acapPackageConf']['setup']['appName'])" 2>/dev/null || true)
fi

if [ -z "$APP_NAME" ]; then
    APP_NAME="$(basename "$APP_DIR" | tr '-' '_')"
    echo "Warning: Could not read appName from manifest.json, using: ${APP_NAME}"
fi

echo "=== Deploying ${APP_NAME} to ${DEVICE_IP} ==="
echo "EAP file: ${EAP_FILE}"
echo

# Stop the app if it's already running (ignore errors)
echo "--- Stopping existing app (if running) ---"
curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    "http://${DEVICE_IP}/axis-cgi/applications/control.cgi?action=stop&package=${APP_NAME}" \
    2>/dev/null || true
echo

# Remove existing installation (ignore errors)
echo "--- Removing existing installation (if any) ---"
curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    "http://${DEVICE_IP}/axis-cgi/applications/control.cgi?action=remove&package=${APP_NAME}" \
    2>/dev/null || true
echo

# Upload the .eap file
echo "--- Uploading ${EAP_FILE} ---"
UPLOAD_RESULT=$(curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    -F "packfil=@${EAP_FILE}" \
    "http://${DEVICE_IP}/axis-cgi/applications/upload.cgi" 2>/dev/null)
echo "$UPLOAD_RESULT"

if echo "$UPLOAD_RESULT" | grep -qi "error"; then
    echo "Error: Upload failed!"
    exit 1
fi
echo

# Start the application
echo "--- Starting ${APP_NAME} ---"
START_RESULT=$(curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    "http://${DEVICE_IP}/axis-cgi/applications/control.cgi?action=start&package=${APP_NAME}" 2>/dev/null)
echo "$START_RESULT"

if echo "$START_RESULT" | grep -qi "error"; then
    echo "Error: Start failed!"
    exit 1
fi
echo

# Check status
echo "--- Application status ---"
curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
    "http://${DEVICE_IP}/axis-cgi/applications/list.cgi" 2>/dev/null | \
    grep -i "$APP_NAME" || echo "(app not found in list)"
echo

echo "=== Deploy complete ==="
