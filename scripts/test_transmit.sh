#!/bin/bash
# Test audio transmission to remote Axis device via transmit.cgi
# Sends a generated test tone (G.711 u-law) to verify the remote device accepts audio.
#
# Usage: ./scripts/test_transmit.sh [device-ip] [user] [pass] [duration-secs]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${PROJECT_DIR}/.env.devices"

if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi

DEVICE_IP="${1:-${C1110E_IP:-192.168.1.219}}"
DEVICE_USER="${2:-${C1110E_USER:-root}}"
DEVICE_PASS="${3:-${C1110E_PASS:-pass}}"
DURATION="${4:-5}"

TRANSMIT_URL="http://${DEVICE_IP}/axis-cgi/audio/transmit.cgi"

echo "=== Testing audio transmit to ${DEVICE_IP} ==="
echo "URL:      ${TRANSMIT_URL}"
echo "Duration: ${DURATION}s"
echo "Codec:    G.711 u-law (8kHz, mono)"
echo

# Check if sox is available for tone generation
if command -v sox &>/dev/null; then
    echo "--- Generating and streaming 440Hz test tone ---"
    # Generate a 440Hz sine wave, encode as G.711 u-law, stream via HTTP POST
    sox -n -t raw -r 8000 -e mu-law -b 8 -c 1 - synth "$DURATION" sine 440 | \
        curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
            -H "Content-Type: audio/basic" \
            -H "Content-Length: 0" \
            -H "Transfer-Encoding: chunked" \
            --data-binary @- \
            "${TRANSMIT_URL}" &
    CURL_PID=$!

    sleep "$DURATION"
    kill $CURL_PID 2>/dev/null || true
    wait $CURL_PID 2>/dev/null || true

    echo
    echo "=== Transmit test complete ==="
    echo "Did you hear a tone on the remote device?"

elif command -v python3 &>/dev/null; then
    echo "--- Generating test tone with Python ---"
    python3 -c "
import struct, math, sys, audioop

RATE = 8000
FREQ = 440
DURATION = ${DURATION}

# Generate 16-bit PCM sine wave
samples = []
for i in range(RATE * DURATION):
    sample = int(32767 * 0.5 * math.sin(2 * math.pi * FREQ * i / RATE))
    samples.append(struct.pack('<h', sample))
pcm_data = b''.join(samples)

# Convert to u-law
ulaw_data = audioop.lin2ulaw(pcm_data, 2)
sys.stdout.buffer.write(ulaw_data)
" | curl -s --digest -u "${DEVICE_USER}:${DEVICE_PASS}" \
        -H "Content-Type: audio/basic" \
        --data-binary @- \
        "${TRANSMIT_URL}"

    echo
    echo "=== Transmit test complete ==="
    echo "Did you hear a tone on the remote device?"

else
    echo "Error: Neither sox nor python3 available for tone generation."
    echo "Install sox: sudo apt install sox"
    echo
    echo "Alternative: test with a simple connectivity check:"
    echo "  curl -v --digest -u ${DEVICE_USER}:${DEVICE_PASS} ${TRANSMIT_URL}"
    exit 1
fi
