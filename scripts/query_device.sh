#!/bin/bash
# Phase 1: Device Investigation via VAPIX HTTP APIs
# Queries C6110 and C1110-E to determine architecture, audio topology, and capabilities.
#
# Usage: ./scripts/query_device.sh [c6110|c1110e|both]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="${PROJECT_DIR}/.env.devices"

if [ -f "$ENV_FILE" ]; then
    # shellcheck disable=SC1090
    . "$ENV_FILE"
fi

# Device credentials
C6110_IP="${C6110_IP:-192.168.1.193}"
C6110_USER="${C6110_USER:-root}"
C6110_PASS="${C6110_PASS:-pass}"

C1110E_IP="${C1110E_IP:-192.168.1.219}"
C1110E_USER="${C1110E_USER:-root}"
C1110E_PASS="${C1110E_PASS:-pass}"

OUTPUT_DIR="docs"
FINDINGS_FILE="${OUTPUT_DIR}/device_findings.md"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[+]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err() { echo -e "${RED}[-]${NC} $1"; }
section() { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

# VAPIX JSON-RPC helper
vapix_jsonrpc() {
    local ip="$1" user="$2" pass="$3" cgi="$4" method="$5"
    local params="${6:-{}}"
    local url="http://${ip}/axis-cgi/${cgi}"
    local payload="{\"apiVersion\":\"1.0\",\"method\":\"${method}\",\"params\":${params}}"

    log "POST ${url} → ${method}"
    curl -s --digest -u "${user}:${pass}" \
        -H "Content-Type: application/json" \
        -d "${payload}" \
        "${url}" 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "(raw response or error)"
}

# VAPIX param.cgi helper
vapix_param() {
    local ip="$1" user="$2" pass="$3" group="$4"
    local url="http://${ip}/axis-cgi/param.cgi?action=list&group=${group}"

    log "GET ${url}"
    curl -s --digest -u "${user}:${pass}" "${url}" 2>/dev/null || echo "(error)"
}

# Generic GET helper
vapix_get() {
    local ip="$1" user="$2" pass="$3" path="$4"
    local url="http://${ip}/axis-cgi/${path}"

    log "GET ${url}"
    curl -s --digest -u "${user}:${pass}" "${url}" 2>/dev/null || echo "(error)"
}

query_c6110() {
    section "C6110 Network Paging Console (${C6110_IP})"

    echo "--- Basic Device Info ---"
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "basicdeviceinfo.cgi" "getAllProperties"
    echo

    echo "--- Audio Device Capabilities ---"
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "getDevicesCapabilities"
    echo

    echo "--- Audio Device Settings ---"
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "getDevicesSettings"
    echo

    echo "--- Audio Parameters ---"
    vapix_param "$C6110_IP" "$C6110_USER" "$C6110_PASS" "Audio"
    echo

    echo "--- Audio Properties ---"
    vapix_param "$C6110_IP" "$C6110_USER" "$C6110_PASS" "Properties.Audio"
    echo

    echo "--- System Properties ---"
    vapix_param "$C6110_IP" "$C6110_USER" "$C6110_PASS" "Properties.System"
    echo

    echo "--- Firmware Version ---"
    vapix_param "$C6110_IP" "$C6110_USER" "$C6110_PASS" "Properties.Firmware"
    echo
}

query_c1110e() {
    section "C1110-E Network Speaker (${C1110E_IP})"

    echo "--- Basic Device Info ---"
    vapix_jsonrpc "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" \
        "basicdeviceinfo.cgi" "getAllProperties"
    echo

    echo "--- Audio Device Capabilities ---"
    vapix_jsonrpc "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" \
        "audiodevicecontrol.cgi" "getDevicesCapabilities"
    echo

    echo "--- Audio Device Settings ---"
    vapix_jsonrpc "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" \
        "audiodevicecontrol.cgi" "getDevicesSettings"
    echo

    echo "--- Audio Transmit Info ---"
    vapix_get "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" \
        "audio/transmit.cgi" || true
    echo

    echo "--- Audio Parameters ---"
    vapix_param "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" "Audio"
    echo

    echo "--- Audio Properties ---"
    vapix_param "$C1110E_IP" "$C1110E_USER" "$C1110E_PASS" "Properties.Audio"
    echo
}

ssh_investigate_c6110() {
    section "C6110 SSH Investigation (${C6110_IP})"
    local SSH_CMD="sshpass -p ssh ssh -o StrictHostKeyChecking=no ssh@${C6110_IP}"

    if ! command -v sshpass &>/dev/null; then
        warn "sshpass not installed. Install with: sudo apt install sshpass"
        warn "Or run these commands manually via: ssh ssh@${C6110_IP}"
        echo
        echo "Commands to run on C6110:"
        echo "  uname -m"
        echo "  pw-cli list-objects"
        echo "  pw-cli info all"
        echo "  aplay -l"
        echo "  amixer"
        echo "  dmesg | grep -i audio"
        echo "  dmesg | grep -i jack"
        echo "  cat /proc/asound/cards"
        echo "  cat /proc/asound/card0/codec#0 2>/dev/null"
        return
    fi

    local cmds=(
        "uname -m"
        "pw-cli list-objects"
        "aplay -l"
        "amixer"
        "dmesg | grep -i -E 'audio|jack|headphone|codec' | tail -30"
        "cat /proc/asound/cards"
        "ls -la /sys/class/sound/"
    )

    for cmd in "${cmds[@]}"; do
        echo "--- ${cmd} ---"
        $SSH_CMD "$cmd" 2>/dev/null || warn "Command failed: ${cmd}"
        echo
    done
}

test_unmute_speaker() {
    section "Test: Unmute Speaker via VAPIX (Goal 1 Quick Test)"
    warn "This test attempts to unmute the speaker while headphones may be plugged in."
    warn "Run this WITH headphones plugged in to test if VAPIX can override the mute."
    echo

    echo "--- Current settings before unmute attempt ---"
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "getDevicesSettings"
    echo

    echo "--- Attempting to unmute all outputs ---"
    # Try setting mute=false on device 0, output 0
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "setDevicesSettings" \
        '{"devices":[{"id":0,"outputs":[{"id":0,"mute":false}]}]}'
    echo

    # Also try output 1 if it exists
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "setDevicesSettings" \
        '{"devices":[{"id":0,"outputs":[{"id":1,"mute":false}]}]}'
    echo

    echo "--- Settings after unmute attempt ---"
    vapix_jsonrpc "$C6110_IP" "$C6110_USER" "$C6110_PASS" \
        "audiodevicecontrol.cgi" "getDevicesSettings"
}

save_findings() {
    mkdir -p "$OUTPUT_DIR"
    log "Saving all output to ${FINDINGS_FILE}"
    {
        echo "# Device Findings"
        echo ""
        echo "Generated: $(date -Iseconds)"
        echo ""
        echo "## Instructions"
        echo ""
        echo "This file should be populated with findings after running:"
        echo '```'
        echo "./scripts/query_device.sh both 2>&1 | tee docs/device_findings_raw.txt"
        echo '```'
        echo ""
        echo "Then fill in the sections below based on the output."
        echo ""
        echo "## C6110 Architecture"
        echo "- **SoC:** (from basicdeviceinfo)"
        echo "- **Architecture:** (from uname -m: armv7hf or aarch64)"
        echo "- **Firmware:** (from basicdeviceinfo)"
        echo ""
        echo "## C6110 Audio Topology"
        echo "- **Audio Device IDs:** (from getDevicesCapabilities)"
        echo "- **Input IDs:** (list)"
        echo "- **Output IDs:** (list with connection types)"
        echo "- **Speaker Output ID:** ?"
        echo "- **Headphone Output ID:** ?"
        echo ""
        echo "## Headphone Jack Behavior"
        echo "- **Without headphones:** (getDevicesSettings output)"
        echo "- **With headphones:** (getDevicesSettings output)"
        echo "- **VAPIX unmute works?** Yes/No"
        echo ""
        echo "## C1110-E Capabilities"
        echo "- **Architecture:** (from basicdeviceinfo)"
        echo "- **Accepts transmit.cgi:** Yes/No"
        echo "- **Supported codecs:** (list)"
        echo ""
        echo "## PipeWire Topology (from SSH)"
        echo "- **Nodes:** (from pw-cli list-objects)"
        echo "- **Changes with headphones:** (diff)"
        echo ""
        echo "## Decision: Speaker Guard Approach"
        echo "- [ ] Approach A: VAPIX polling (setDevicesSettings unmute)"
        echo "- [ ] Approach B: PipeWire loopback"
    } > "$FINDINGS_FILE"
    log "Template saved. Run queries and fill in findings."
}

# Main
TARGET="${1:-both}"

case "$TARGET" in
    c6110)
        query_c6110
        ;;
    c1110e)
        query_c1110e
        ;;
    ssh)
        ssh_investigate_c6110
        ;;
    unmute)
        test_unmute_speaker
        ;;
    both)
        query_c6110
        query_c1110e
        ;;
    all)
        query_c6110
        query_c1110e
        ssh_investigate_c6110
        test_unmute_speaker
        ;;
    *)
        echo "Usage: $0 [c6110|c1110e|ssh|unmute|both|all]"
        echo ""
        echo "  c6110  - Query C6110 via VAPIX"
        echo "  c1110e - Query C1110-E via VAPIX"
        echo "  ssh    - SSH investigation of C6110"
        echo "  unmute - Test VAPIX speaker unmute (Goal 1 quick test)"
        echo "  both   - Query both devices via VAPIX (default)"
        echo "  all    - Run everything"
        exit 1
        ;;
esac

save_findings
log "Done! Review output above and update ${FINDINGS_FILE}"
