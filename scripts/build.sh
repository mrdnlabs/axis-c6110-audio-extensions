#!/bin/bash
# Build ACAP application using Docker cross-compilation
#
# Usage: ./scripts/build.sh <app-dir> [arch]
# Example: ./scripts/build.sh hello-world aarch64
#          ./scripts/build.sh audio-control armv7hf

set -euo pipefail

APP_DIR="${1:?Usage: $0 <app-dir> [arch]}"
ARCH="${2:-aarch64}"

# Validate arch
case "$ARCH" in
    aarch64|armv7hf) ;;
    *) echo "Error: ARCH must be aarch64 or armv7hf (got: $ARCH)"; exit 1 ;;
esac

# Resolve paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_PATH="${PROJECT_DIR}/${APP_DIR}"

if [ ! -d "$APP_PATH" ]; then
    echo "Error: App directory not found: $APP_PATH"
    exit 1
fi

if [ ! -f "$APP_PATH/Dockerfile" ]; then
    echo "Error: No Dockerfile found in $APP_PATH"
    exit 1
fi

APP_NAME="$(basename "$APP_DIR")"
IMAGE_NAME="acap-${APP_NAME}:${ARCH}"

echo "=== Building ${APP_NAME} for ${ARCH} ==="
echo "App dir:  ${APP_PATH}"
echo "Image:    ${IMAGE_NAME}"
echo

# Build the Docker image
docker build \
    --build-arg ARCH="$ARCH" \
    -t "$IMAGE_NAME" \
    "$APP_PATH"

# Extract the .eap file from the container
CONTAINER_ID=$(docker create "$IMAGE_NAME")
mkdir -p "${APP_PATH}/build"

# Copy all .eap files from the build output
docker cp "${CONTAINER_ID}:/opt/app/" - | tar -xf - -C "${APP_PATH}/build/" --strip-components=1 2>/dev/null || true

# Find the .eap file
EAP_FILE=$(find "${APP_PATH}/build" -name "*.eap" -type f 2>/dev/null | head -1)

docker rm "$CONTAINER_ID" > /dev/null

if [ -n "$EAP_FILE" ]; then
    echo
    echo "=== Build successful ==="
    echo "EAP file: ${EAP_FILE}"
    echo "Size:     $(du -h "$EAP_FILE" | cut -f1)"
else
    echo
    echo "=== Build failed: no .eap file produced ==="
    echo "Check Docker build output for errors."
    exit 1
fi
