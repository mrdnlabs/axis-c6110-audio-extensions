# Axis C6110 Audio Extensions

**ACAP for Axis C6110-E Network Horn Speaker**
Vendor: MRDN Labs | Version: 1.8.0

---

## Overview

This ACAP runs on the **Axis C6110-E** and provides two features:

1. **Speaker Guard** — Prevents the C6110's built-in speaker from going silent when headphones are plugged into the 3.5mm jack. Without this, the device firmware mutes the speaker whenever headphones are detected.

2. **Audio Forwarding** — Captures audio from the C6110's output and streams it in real time to a remote Axis network speaker (tested with Axis C1110-E). Audio is encoded as G.711 µ-law at 8 kHz and sent via HTTP POST to the remote device's VAPIX audio clip/live API.

3. **Instructor / Classroom Mode** — Two operating modes that reroute audio to the appropriate output:
   - **Instructor mode**: Headphone output muted, built-in speaker active, remote speaker muted. Used when an instructor is wearing a headset.
   - **Classroom mode**: Headphone output active, built-in speaker muted, remote speaker active. Used when broadcasting to the room.

   Mode is switched instantly via the settings UI or via paging console button actions (auto-configured on first run).

---

## Hardware Setup

| Device | Role | Default IP |
|--------|------|-----------|
| Axis C6110-E | Local device (runs this ACAP) | — |
| Axis C1110-E | Remote speaker | 192.168.0.90 |

The C6110 connects to the C1110-E over the local network. Both devices must be reachable from each other.

---

## Installation

1. Download `Axis_C6110_Audio_Extensions_1_8_0_aarch64.eap` from the `app/` directory.
2. Log into the C6110's web interface.
3. Navigate to **Apps** and click **Add app**.
4. Upload the `.eap` file and enable the app.

---

## Configuration

Open the settings page at:

```
http://<C6110-IP>/local/audio_control/
```

Or navigate to **Apps → Axis C6110 Audio Extensions → Open**.

### Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| **Remote IP** | IP address of the remote C1110-E speaker | `192.168.0.90` |
| **Remote Username** | Admin username on the C1110-E | `root` |
| **Remote Password** | Admin password on the C1110-E | *(empty)* |
| **Local Username** | Admin username on this C6110 (for VAPIX calls) | `root` |
| **Local Password** | Admin password on this C6110 | *(empty)* |
| **Audio Forwarding** | Enable/disable audio stream to remote speaker | `yes` |
| **Speaker Guard** | Keep built-in speaker active with headphones | `yes` |

> **Note:** After saving Remote IP, Username, Password, or toggling features, restart the ACAP for changes to take effect. Mode switching takes effect immediately without a restart. In local development, keep actual device values in `.env.devices` rather than committing them.

### Mode Switching

Click **Instructor** or **Classroom** in the settings UI. The mode applies instantly — no Save required. The ACAP polls for mode changes every second.

**Instructor mode muting:**
- C6110 headphone output: **muted**
- C6110 built-in speaker: **active**
- C1110-E speaker: **muted**

**Classroom mode muting:**
- C6110 headphone output: **active**
- C6110 built-in speaker: **muted**
- C1110-E speaker: **active**

---

## Paging Console Button Actions

On first start, the ACAP automatically creates two button actions in the C6110's paging console:

- **Classroom Mode** — switches to classroom mode when a paging console button is pressed
- **Instructor Mode** — switches to instructor mode when a paging console button is pressed

These actions call `param.cgi` to update the `ActiveMode` parameter, which the ACAP detects within one second and applies.

---

## Architecture

```
audio_control (main)
├── PipeWire event loop (main thread)
├── vapix_client       — libcurl wrapper for local/remote VAPIX JSON-RPC calls
├── speaker_guard      — PipeWire node monitoring to keep speaker unmuted (Approach B)
├── audio_forwarder    — PipeWire capture → G.711 µ-law → HTTP POST to C1110-E
└── mode_controller    — 1Hz poll of ActiveMode parameter, spawns thread to apply
```

All VAPIX calls use Digest authentication. Local calls go to `http://127.0.0.1/axis-cgi/`. Remote calls go to `http://<RemoteIP>/axis-cgi/`.

---

## Building from Source

Requires Docker and the Axis ACAP Native SDK image.

```bash
cd audio-control
docker run --rm \
  -v "$(pwd)":/workspace \
  -w /workspace/app \
  axisecp/acap-native-sdk:12.4.0-aarch64 \
  /bin/bash -c "source /opt/axis/acapsdk/environment-setup-cortexa53-crypto-poky-linux && acap-build ."
```

Output: `app/Axis_C6110_Audio_Extensions_1_8_0_aarch64.eap`

---

## Troubleshooting

**Mode not switching on remote speaker:**
Check that `RemotePass` is set correctly. An incorrect password causes a 401 response which logs as a JSON parse error. After updating the password in settings, restart the ACAP.

**Speaker Guard not working:**
Ensure `EnableSpeakerGuard` is `yes` and the ACAP has the `pipewire` group (set in `manifest.json`). Check syslog for speaker guard errors.

**Audio not forwarding:**
Verify `EnableAudioForward` is `yes`, the remote IP is reachable, and credentials are correct. Check `journalctl` or the device syslog (facility `LOCAL4`) for errors.

**Checking logs:**
```bash
ssh root@<C6110-IP> "journalctl -t audio_control -f"
```
