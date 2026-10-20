<div align="center">

<img src="resources/branding/uvnvr-256.png" alt="UvNVR" width="120" />

# UvNVR

**Open-source software NVR for ONVIF and RTSP cameras.**

ONVIF discovery · RTSP ingest · scheduled recording · seamless playback · RTSP output · web UI

[![CI](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/uvnvr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![libuvcpp](https://img.shields.io/badge/libuvcpp-1.8.1-2c3e50)](https://github.com/Antruly/libuvcpp)
[![Vue 3](https://img.shields.io/badge/Vue-3-42b883)](https://vuejs.org/)
[![Status](https://img.shields.io/badge/status-M1%20foundation-38bdf8)](#status)

[English](README.md) · [简体中文](README.zh-CN.md)

</div>

---

## Status

**M1 — authentication, roles, audit and persistence.** The recorder now has first-run
setup, session authentication, CSRF protection, users/roles/permissions, API tokens, audit
logging, SQLite migrations and a live dashboard. Camera, recording and playback features
arrive in M2/M3. Track the milestones in
[`docs/development-plan.md`](docs/development-plan.md).

## What UvNVR will be

UvNVR is a software NVR for small and medium camera deployments. A single service discovers
ONVIF cameras, pulls their RTSP streams once, fans them out to many browser clients, records
keyframe-aligned MPEG-TS segments with a SQLite index, and serves seamless playback and RTSP
output.

### Implemented in M1

- First-run setup and the default `admin` account; scrypt password hashing.
- Session cookies (HttpOnly/SameSite), CSRF protection and API bearer tokens.
- Users, roles and granular Web/API permissions; built-in admin/operator/viewer roles.
- Web and API failure lockouts with configurable thresholds.
- Audit log with categories, filters, pagination and 180-day retention cleanup.
- SQLite schema migrations, system metadata, serial number and system metrics.
- Dashboard with CPU, memory, disk, users, sessions and audit counters.
- Chinese/English UI, cached workspace tabs and a responsive admin shell.

### Planned 0.1.0 scope

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

FFmpeg development libraries are not required for M1. They will be added with the media
core in M2.

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

./build/uvnvr --port 8080
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

## License

UvNVR source code is released under the [MIT License](LICENSE).

Official builds use **FFmpeg shared libraries under LGPL-2.1-or-later**, loaded dynamically.
The build must not enable `--enable-gpl` or `--enable-nonfree`. Every release that ships
FFmpeg includes the corresponding FFmpeg source, the configure line and license notices; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). Your own code can remain MIT while the
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
> include x264/x265; they use an LGPL FFmpeg build and load its shared libraries at runtime.

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
| M2 | Channels, ONVIF discovery, RTSP ingest, live view, snapshots |
| M3 | Recording, segments, index, storage policies, playback, read-ahead |
| M4 | RTSP server, API, alerts, GPU/perf, system settings |
| M5 | Six-platform installers and CI matrix |
| M6 | Soak/security hardening, docs, `v0.1.0` release |

## Contributing and security

- Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a PR.
- Report vulnerabilities privately through GitHub Security Advisories; see [SECURITY.md](SECURITY.md).
- By contributing you agree to the [Code of Conduct](CODE_OF_CONDUCT.md) and certify your
  contribution with a DCO sign-off (`git commit -s`).

## Acknowledgements

- [libuvcpp](https://github.com/Antruly/libuvcpp) — MIT, event-driven C++ web framework.
- [uvp](https://github.com/Antruly/uvp) — MIT, scaffolding and verification for libuvcpp services.
- [FFmpeg](https://ffmpeg.org/) — LGPL-2.1-or-later, media toolkit.
- [Vue 3](https://vuejs.org/), [Vite](https://vite.dev/), [Naive UI](https://www.naiveui.com/),
  [ECharts](https://echarts.apache.org/) — frontend stack.
- [Desk2ONVIF](https://github.com/Antruly/Desk2ONVIF) — a useful reference for ONVIF Profile S
  behaviour and virtual-camera testing.
