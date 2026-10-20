# UvNVR 0.2.0

Configuration and playback update.

## Highlights

- **Startup configuration file.** `uvnvr.json` sets the HTTP port, RTSP port, bind
  address, data directory, TLS, FFmpeg path and log level. Command-line flags
  override environment variables, which override the file. `uvnvr --print-config`
  shows the merged settings and the file that was loaded.
- **Installer port selection.** The Windows NSIS installer has a **Service ports**
  page, writes `%ProgramData%\UvNVR\uvnvr.json`, and adds a **UvNVR Configuration**
  Start-menu shortcut. Linux packages use `/etc/uvnvr/uvnvr.json`; the macOS pkg
  uses `/Library/Application Support/UvNVR/uvnvr.json`.
- **Faster playback controls.** 0.5×–8× speed, −30s/−10s/+10s/+30s skip buttons,
  click-to-seek on the timeline and a live position cursor. The server paces the
  MPEG-TS stream by segment bitrate and starts near a keyframe when seeking.
- **Configuration visibility.** The System page shows the effective ports, bind
  address, data directory and configuration path. `GET /api/v1/system/config`
  exposes the same values to API clients.

## Upgrade notes

- 0.1.0 deployments have no configuration file and keep their existing defaults.
  Installing 0.2.0 creates one with ports 8080/554 unless a file already exists;
  existing files are preserved on upgrade.
- Changing ports: edit the file and restart the service. The Windows installer
  updates the Start-menu web shortcut to the selected HTTP port.
- The data directory is not migrated. Copy `uvnvr.db`, `secret.key` and recordings
  before changing `dataDir`.
- Playback rewind is implemented as repeated backward seeks; browsers cannot decode
  a reversed GOP. Fast-forward uses server-paced delivery plus the media element's
  playback rate.

## Packages

Same six-target matrix as 0.1.0: Windows x64/arm64, Linux x64/arm64 and macOS
x64/arm64, plus checksums and the SPDX SBOM.

## FFmpeg and patent notice

Official packages invoke an LGPL FFmpeg CLI as a separate process; no FFmpeg code is
linked into UvNVR. This project does not grant any patent license for H.264,
H.265/HEVC, AAC or any other codec. See `THIRD_PARTY_NOTICES.md`.
