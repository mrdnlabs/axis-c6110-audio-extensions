# Axis C6110 Audio Extensions

This repository contains a set of Axis ACAP applications and helper scripts
used to investigate, prototype, build, and deploy audio behavior for the
Axis C6110 Network Paging Console and related Axis speaker devices.

## Repository Layout

- `audio-control/`: Primary ACAP that provides speaker guard behavior, remote
  audio forwarding, and instructor/classroom mode switching.
- `audio-monitor/`: Diagnostic ACAP that monitors audio levels from PipeWire
  and logs them to syslog.
- `audio-investigate/`: Investigation ACAP used to enumerate PipeWire objects
  and inspect device audio topology on target hardware.
- `hello-world/`: Minimal ACAP used to validate the build/deploy/run cycle.
- `scripts/`: Local helper scripts for building packages, deploying to devices,
  collecting logs, querying VAPIX endpoints, and testing audio transmit.
- `docs/`: Checked-in reference documentation and device templates.

## Main Application

The main deliverable is `audio-control`, packaged as an Axis ACAP extension for
the C6110. Its detailed application-specific documentation lives in
[`audio-control/README.md`](audio-control/README.md).

## Local Configuration

Keep lab-specific IP addresses, usernames, and passwords in a local
`.env.devices` file at the repository root. A template is provided in
`.env.devices.example`. The helper scripts will load `.env.devices`
automatically when it exists.

## Build

Build an ACAP package with:

```bash
./scripts/build.sh audio-control aarch64
```

The generated package is written under the target app's `build/` directory.

## Notes

Generated binaries, `.eap` packages, temp files, and other build artifacts are
intentionally excluded from version control.
