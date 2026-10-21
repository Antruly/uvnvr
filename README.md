<div align="center">

<img src="resources/branding/uvnvr-256.png" alt="UvNVR" width="120" />

# UvNVR

**Open-source software NVR for ONVIF and RTSP cameras.**

ONVIF discovery · RTSP ingest · scheduled recording · seamless playback · RTSP output · web UI

[![CI](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![libuvcpp](https://img.shields.io/badge/libuvcpp-1.8.1-2c3e50)](https://github.com/Antruly/libuvcpp)
[![Vue 3](https://img.shields.io/badge/Vue-3-42b883)](https://vuejs.org/)
[![Status](https://img.shields.io/badge/status-0.9.0-22c55e)](#status)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

---

## Status

**0.9.0 — event-triggered recording.** The recorder supports
ONVIF discovery/probing, channel management, RTSP ingest with automatic reconnects,
live multi-client preview, plan-driven MPEG-TS recording, SQLite indexing, playback,
RTSP output, snapshots, alerts, email notifications, users/roles, audit logging and a
bilingual web UI.

0.9.0 turns event plans into real recordings: motion and ONVIF person/tamper/line
events start clips with buffered pre-roll, extend them with follow-up alerts and
stop after the post-roll, with configurable pre/post/maximum durations. It builds
on 0.8.0's CSV bulk import and live recording status, 0.7.0's Hikvision/Dahua HTTP
fallback, per-channel HTTP port, x64 MSI and codec-boundary continuation, 0.6.0's
recording fixes and operator UI, 0.5.0's ONVIF events, 0.4.0's channel groups and
layouts, 0.3.0's downloads/export/cache, 0.2.0's startup configuration, and the
core NVR loop. Peer-to-peer/WebRTC, cloud access and advanced analytics remain out
of scope. Track the milestones in
[`docs/development-plan.md`](docs/development-plan.md).

## What UvNVR will be

UvNVR is a software NVR for small and medium camera deployments. A single service discovers
ONVIF cameras, pulls their RTSP streams once, fans them out to many browser clients, records
keyframe-aligned MPEG-TS segments with a SQLite index, and serves seamless playback and RTSP
output.

### Implemented now

- First-run setup, scrypt passwords, sessions, CSRF, users/roles and API tokens.
- ONVIF WS-Discovery/probe and manual RTSP; Hikvision/Dahua path presets.
- Channel tree, main/sub streams, credentials encrypted with AES-256-GCM.
- Live preview grid (1/4/8/16/32 + custom), saved layouts, watermarks, offline overlays.
- One FFmpeg ingest process per channel stream; shared TS fan-out to WebSocket clients.
- Plan-driven recording, keyframe-aligned segments, time/size slicing, loop/alert volumes.
- Playback timeline, WebSocket streaming across segments, read-ahead and segment download.
- RTSP output (`/live/{channel}/{main|sub}`, RTP/MP2T over TCP/UDP) with Basic auth.
- Snapshots, motion/offline/storage alerts, SMTP email (QQ preset), audit log.
- Performance tier, GPU detection and explicit GPU toggle; system time API.
- Chinese/English UI, cached tabs, dashboard, help manual and API tokens.
- Startup `uvnvr.json` with ports/bind/data/TLS, CLI/env/config precedence and
  `--print-config`; Windows installer port page and macOS/Linux service configs.
- Playback speed control (0.5×–8×), ±10s/±30s seek controls, timeline click-to-seek
  and a playback position cursor.
- Streaming Range downloads with resume, MP4 export (FFmpeg remux) and a shared
  64 KiB-block playback cache with configurable size and read-ahead.
- Channel groups with parent/order and a collapsible preview tree; per-user
  server-side preview layouts with a default layout and offline browser cache.
- ONVIF PullPoint events (motion/person/tamper/line crossing) mapped to alerts,
  with a per-channel toggle and 60-second cooldowns.
- Plan rectangle selection, auto/light/dark themes, channel thumbnails/snapshots,
  brand auto-detect and one-click add, preview play-all/paging, and a Recordings
  shortcut.
- Playback 0.25×–32×, timeline zoom down to 5 minutes, reverse step mode and
  time-range MP4 download.
- Per-token API scopes with a grouped selector; camera time sync from browser/NTP
  updates; codec-change segment roll and codec-aware playback.
- Hikvision ISAPI/Dahua CGI Digest fallback for detection and time sync without
  ONVIF; per-channel HTTP port; x64 MSI with port properties; automatic playback
  continuation at codec boundaries.
- CSV bulk channel import with per-row detection and error reporting; a live
  recording-status API and Storage-page panel showing current files and the
  active recording volume.
- Event-triggered clips for motion and ONVIF person/tamper/line events, with
  configurable pre-roll, post-roll and maximum length; event plans and
  `record_type=event` indexing are wired end to end.

### Delivered scope

- ONVIF WS-Discovery and one-click add; manual RTSP; Hikvision/Dahua/URL templates.
- Channel tree, groups, main/sub stream, reconnection, online/offline history, sync.
- Live grid: 1 / 4 / 8 / 16 / 32 and custom layouts, drag/resize, saved layouts, watermarks.
- Scheduled + event recording, time- or size-based segments, per-volume loop/alert policy.
- MPEG-TS segments + SQLite index; ring-buffer writes; io_uring on Linux; crash recovery.
- Playback timeline, seeking, read-ahead, LRU cache, seamless cross-file playback, download.
- RTSP server (`/live/{channel}/{main|sub}`), REST API, snapshots, alerts and email.
- Users/roles/permissions, first-run admin setup, login/API lockout, audit log.
- Performance tiers, GPU detection, NTP/browser time sync, maintenance mode.
- Windows x64/arm64 (MinGW), Linux x64/arm64 (deb/rpm), macOS x64/arm64 packages.
- Chinese/English UI with automatic browser-language detection.

See [`docs/plan.md`](docs/plan.md) for the full requirements and
[`docs/development-plan.md`](docs/development-plan.md) for the architecture, milestones and
release plan.

## Architecture at a glance

```
Browser (Vue 3)
   │ HTTPS / REST / WebSocket(MPEG-TS)
libuvcpp 1.8.1 service
   ├─ HTTP/2, WebSocket, REST API, auth, audit
   ├─ RTSP server (RTSP out)
   ├─ ONVIF discovery/client
   ├─ MediaCore: one RTSP pull per channel → timestamp normalisation → TS muxer
   │    ├─ LiveHub (drop to keyframe under pressure)
   │    ├─ RecordingSpool (reliable, back-pressure)
   │    └─ Decoder tap (snapshot / motion)
   └─ StorageCore: segment index, ring buffer, io_uring/uv_fs, LRU cache, read-ahead
SQLite (WAL)
```

The media path is deliberately stream-copy: recording never transcodes. FFmpeg is used for
RTSP ingest, demuxing, snapshots, optional compatibility transcoding and MP4 export.

## Build from source (M0)

### Prerequisites

- CMake 3.24+, a C++20 compiler, `pkg-config`, `ninja` or `make`
- **libuvcpp 1.8.1** (release archive or an installed CMake package)
- Node.js 22.19+, 24.11+, or 26+ and npm (frontend only)
- Linux: `libssl-dev`, `libuv1-dev`, `libsqlite3-dev`, `zlib1g-dev` (the libuvcpp release
  archive bundles most dependencies; these are needed for UvNVR's OpenSSL/libuv calls and if
  you build libuvcpp from source)

FFmpeg development libraries are **not** linked into UvNVR. The service runs an FFmpeg
executable as an isolated child process. Point `UVNVR_FFMPEG` at a compatible binary, or
place `ffmpeg` next to the executable. Release packages bundle a static LGPL FFmpeg build.

### Linux x64 quick start

```bash
git clone https://github.com/Antruly/uvnvr.git
cd uvnvr

# Frontend
cd frontend
npm ci
npm run build
cd ..

# libuvcpp 1.8.1 release archive
curl -fL -o /tmp/libuvcpp.zip \
  https://github.com/Antruly/libuvcpp/releases/download/v1.8.1/libuvcpp-1.8.1-linux-x64.zip
mkdir -p "$HOME/.local/uvcpp-1.8.1"
unzip -q /tmp/libuvcpp.zip -d "$HOME/.local/uvcpp-1.8.1"

# Backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT="$HOME/.local/uvcpp-1.8.1/libuvcpp-1.8.1-linux-x64"
cmake --build build -j

UVNVR_FFMPEG=/path/to/ffmpeg ./build/uvnvr --port 8080 --rtsp-port 554
```

Open <http://127.0.0.1:8080>. On first launch you are redirected to `/setup` to create the
administrator account. Health check: <http://127.0.0.1:8080/healthz>.

For a local HTTPS listener with a generated certificate, run
`./build/uvnvr --port 8080 --tls-self-signed`. Production deployments should provide
`UVNVR_TLS_CERT` and `UVNVR_TLS_KEY` instead of using the development certificate.

For frontend development, run `npm run dev` in `frontend/` (Vite proxies `/api`, `/healthz`
and `/ws` to `127.0.0.1:8080`).

### Build options

| Option | Default | Meaning |
|---|---|---|
| `UVNVR_LIBUVCPP_ROOT` | empty | Unpacked libuvcpp release archive (uses pkg-config) |
| `UVNVR_BUILD_TESTS` | `OFF` | Build unit tests |
| `UVNVR_EMBED_WEB` | `OFF` | Embed `frontend/dist` into the binary (later milestone) |

## Known limitations (0.9.0)

- ONVIF probing currently supports `http://` device services only.
- Motion detection is an experimental low-resolution frame-difference detector, not AI.
- Browser playback uses MSE; H.265 depends on the browser/hardware. Prefer an H.264 sub stream.
- RTSP output uses RTP/MP2T (payload type 33). Some clients expect H.264 RTP; VLC/ffplay work.
- macOS packages are ad-hoc signed; Gatekeeper may require a right-click Open unless a
  Developer ID is configured.
- Playback rewind is implemented as repeated backward seeks; browsers do not decode a
  reversed GOP. Fast-forward uses server-paced playback plus the media element rate.
- MP4 export is a remux (`-c copy`); sources that cannot be placed in MP4 are rejected
  with an error rather than transcoded.
- Group order uses the API's numeric `sort` field; the UI does not yet reorder groups
  by drag and drop. Layouts are private to each account.
- ONVIF event names vary by manufacturer; unrecognized topics are ignored.
- Event recording starts from motion or ONVIF alerts only when the channel's
  weekly plan includes event recording. Pre-roll is buffered continuously for
  those channels and is lost across a camera reconnect.
- Auto-detect and brand one-click add require ONVIF on the camera; pure RTSP devices
  still use the manual URL form.
- Reverse playback is implemented as timed backward seeks (choppy); decoders cannot
  play a reversed GOP smoothly. Camera time sync covers ONVIF-capable devices only.
- An x64 MSI is available alongside NSIS; arm64 and portable installs use NSIS/zip.
  The MSI is unsigned, so SmartScreen may warn.
- Hikvision/Dahua HTTP fallback covers ISAPI/CGI cameras; other brands without
  ONVIF still need manual configuration.
- CSV import is sequential. The recording-status panel lists both scheduled and
  event recorders while they are active.
- Audit logs and recordings share the data volume; a separate audit path is a later enhancement.

## License

UvNVR source code is released under the [MIT License](LICENSE).

Official builds bundle an **LGPL FFmpeg CLI binary**; UvNVR runs it as a separate process and
does not link FFmpeg code. The build must not enable `--enable-gpl` or `--enable-nonfree`.
Every release that ships FFmpeg includes the corresponding FFmpeg source, configure line and
license notices; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Your own code can remain MIT while the
distributed package carries LGPL FFmpeg, provided the LGPL conditions are met.

> [!WARNING]
> **No patent license is granted by this project.**
>
> UvNVR does not grant — and cannot grant — any patent license for H.264, H.265/HEVC, AAC or
> any other codec. The MIT license covers copyright only and grants no patent rights.
>
> If you build or bundle **x264, x265**, or any other GPL/patent-bearing component, the
> resulting distribution is subject to GPL obligations and to whatever patent licenses apply.
> Obtaining those licenses is your responsibility. Official UvNVR release builds do **not**
> include x264/x265; they bundle an LGPL FFmpeg CLI and run it as a separate process.

## Install packages

Download the artifacts from the [latest release](https://github.com/Antruly/uvnvr/releases/latest):

| Platform | Packages |
|---|---|
| Windows x64 | NSIS `setup.exe`, **MSI**, portable `.zip` |
| Windows arm64 | NSIS `setup.exe`, portable `.zip` |
| Linux x64 / arm64 | `.deb`, `.rpm`, `.tar.gz` |
| macOS x64 / arm64 | `.pkg`, `.dmg`, `.zip` |

Linux packages install under `/opt/uvnvr`; the systemd unit runs as `uvnvr` and uses `/var/lib/uvnvr`.
The web UI listens on port 8080 by default; complete first-run setup in the browser.
See [`docs/deployment.md`](docs/deployment.md) for ports, data directories and service details.

## Repository layout

```
src/               C++ service (libuvcpp)
frontend/          Vue 3 + TypeScript + Vite
resources/         brand icons and web assets
docs/              requirements, development plan, architecture, manuals
packaging/         Windows/Linux/macOS packaging (M5)
scripts/           icon and release tooling
tests/             unit/integration/e2e tests
.github/           CI, issue/PR templates, repository rules
```

## Roadmap

| Milestone | Scope |
|---|---|
| M0 | Repository, rules, service scaffold, frontend shell, icons, CI smoke test |
| M1 | ✅ Config, SQLite, first-run setup, auth/RBAC, lockout, audit, dashboard |
| M2 | ✅ Channels, ONVIF discovery, RTSP ingest, live view, snapshots |
| M3 | ✅ Recording, segments, index, storage policies, playback, read-ahead |
| M4 | ✅ RTSP server, API, alerts, GPU/perf, system settings |
| M5 | ✅ Six-platform installers and CI matrix |
| M6 | ✅ Release hardening, bilingual manuals, `v0.1.0` |
| 0.2.0 | ✅ Startup config file and ports, installer port page, playback speed/seek |
| 0.3.0 | ✅ Range downloads, MP4 export, shared playback cache with read-ahead |
| 0.4.0 | ✅ Channel groups/tree, per-user server-side preview layouts |
| 0.5.0 | ✅ ONVIF PullPoint camera events integrated with the alert center |
| 0.6.0 | ✅ Recording fixes, themes, thumbnails, brand detect, token scopes, camera time sync |
| 0.7.0 | ✅ Hikvision/Dahua HTTP fallback, HTTP port, x64 MSI, codec auto-continue |
| 0.8.0 | ✅ CSV bulk import, live recording status and active-volume visibility |
| 0.9.0 | ✅ Event-triggered clipping (pre/post-roll), event plans, event settings |

## Contributing and security

- Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a PR.
- Report vulnerabilities privately through GitHub Security Advisories; see [SECURITY.md](SECURITY.md).
- By contributing you agree to the [Code of Conduct](CODE_OF_CONDUCT.md) and certify your
  contribution with a DCO sign-off (`git commit -s`).

## Acknowledgements

- [libuvcpp](https://github.com/Antruly/libuvcpp) — MIT, event-driven C++ web framework.
- [uvp](https://github.com/Antruly/uvp) — MIT, scaffolding and verification for libuvcpp services.
- [FFmpeg](https://ffmpeg.org/) — LGPL media toolkit, invoked as a separate process.
- [Vue 3](https://vuejs.org/), [Vite](https://vite.dev/), [Naive UI](https://www.naiveui.com/),
  [ECharts](https://echarts.apache.org/) — frontend stack.
- [Desk2ONVIF](https://github.com/Antruly/Desk2ONVIF) — a useful reference for ONVIF Profile S
  behaviour and virtual-camera testing.
