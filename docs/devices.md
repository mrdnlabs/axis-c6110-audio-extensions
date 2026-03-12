# Device Reference

This file is a checked-in template. Keep lab-specific IPs, credentials, and
other private device details in a local `.env.devices` file at the repository
root instead of committing them here.

## C6110 Network Paging Console (Primary)

| Field | Value |
|-------|-------|
| **Model** | Axis C6110 Network Paging Console |
| **IP Address** | `<C6110_IP>` |
| **Web UI** | `http://<C6110_IP>` |
| **Credentials** | `<C6110_USER> / <C6110_PASS>` |
| **SSH** | `<SSH configuration>` |
| **Role** | ACAP host device, audio source |

### Known Behavior
- Built-in speaker is disabled at hardware level when 3.5mm headphone jack has a cable plugged in.
- No web UI toggle exists to override this behavior.

---

## C1110-E Network Speaker (Remote)

| Field | Value |
|-------|-------|
| **Model** | Axis C1110-E Mk II Network Speaker |
| **IP Address** | `<C1110E_IP>` |
| **Web UI** | `http://<C1110E_IP>` |
| **Credentials** | `<C1110E_USER> / <C1110E_PASS>` |
| **Role** | Remote audio output target |

### Audio Input
- Accepts audio via VAPIX `transmit.cgi` endpoint
- URL: `http://<C1110E_IP>/axis-cgi/audio/transmit.cgi`
- Supported codecs: G.711 u-law (mu-law), G.711 A-law, G.726, Opus, AAC

---

## Network

All devices are on the same LAN subnet `<LAN_SUBNET>`.
