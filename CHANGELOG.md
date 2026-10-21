# Changelog

All notable changes to UvNVR are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[Semantic Versioning](https://semver.org/).

## [0.5.0] - 2026-10-21

### Added

- ONVIF PullPoint event subscription per ONVIF channel (`eventsEnabled`): resolves
  the event service through `GetCapabilities` with a `/onvif/events` fallback,
  creates and renews subscriptions, and polls for notifications.
- Motion, person, tamper and line/field-crossing topics are mapped to alerts with
  a 60-second per-channel/type cooldown; false/cleared values are ignored.
- Channel form toggle **Use camera ONVIF events**, channel JSON `eventsEnabled`,
  and a mock-camera integration test covering the full SOAP/PullPoint flow.

## [0.4.0] - 2026-10-21

### Added

- Channel groups: CRUD API, parent/order fields, cycle protection, channel
  assignment (`groupId`) and a group manager on the Channels page. The Live
  preview rail renders channels as a collapsible group tree.
- Server-side personal preview layouts: `GET/POST/PUT/DELETE /api/v1/layouts`
  with per-user storage, one default layout, a 20-layout limit and a 64 KiB grid
  limit. The Preview page loads and saves layouts through the API and keeps the
  browser cache as an offline fallback.

### Changed

- Deleting a group moves its channels to ungrouped and reparents its child
  groups to the deleted group's parent.
- Layout changes are synced with a short debounce; switching accounts or
  browsers picks up the server copy.
- Preview-only roles use `GET /api/v1/preview/channels`, which omits stream
  URLs, usernames and stored error text; the full channel list still requires
  `channel.view`.

## [0.3.0] - 2026-10-21

### Added

- HTTP Range support for recording downloads (`206`, `Content-Range`,
  suffix/open ranges, `416` for unsatisfiable ranges) with streaming file
  delivery instead of buffering the whole segment in memory.
- `POST /api/v1/playback/export`: remuxes a channel/time range to a fast-start
  MP4 with the bundled FFmpeg, limited to six hours / 4 GiB, with temporary
  files cleaned up after the response. The Playback page has an **Export MP4**
  button.
- Shared 64 KiB-block LRU cache for playback with next-segment read-ahead
  (`storage.cacheMb`, `storage.readaheadMb`, `UVNVR_CACHE_MB`,
  `UVNVR_READAHEAD_MB`), plus cache hit/miss/usage statistics in the System page.
- Portable Windows packages keep the default `uvnvr.json`; `0.3.0` version
  metadata across packages and the UI.

### Fixed

- Recording downloads no longer fail with `413` above 512 MiB and no longer
  allocate the entire file in memory.
- Playback reads through the shared cache and preloads the next segment instead
  of issuing one open/read per chunk against the recording file.

## [0.2.0] - 2026-10-21

### Added

- Startup JSON configuration (`uvnvr.json`): HTTP/RTSP ports, bind address, data
  directory, TLS, FFmpeg path and log level. Precedence is CLI flags, environment
  variables, the file, then defaults; `--config` and `--print-config` are available.
- `GET /api/v1/system/config` and a read-only **Startup configuration** panel on the
  System page showing ports, bind address, data directory and the loaded file.
- Windows installer **Service ports** page with validation; the installer writes
  `%ProgramData%\UvNVR\uvnvr.json`, adds a **UvNVR Configuration** shortcut and starts
  the service with `--config`.
- Linux packages create `/etc/uvnvr/uvnvr.json` and the systemd unit uses `--config`;
  the macOS pkg creates the Application Support config and launchd uses it. Portable
  Windows zips include a default `uvnvr.json`.
- Playback speed control (API 0.25×–8×, UI 0.5×–8×), ±10s/±30s seek controls,
  timeline click-to-seek and a playback position cursor. The server paces MPEG-TS by
  segment bitrate and starts near the nearest keyframe when seeking.
- `--host`, `--tls-cert`, `--tls-key`, `--ffmpeg`, `--log-level`,
  `UVNVR_PORT`/`UVNVR_RTSP_PORT`/`UVNVR_DATA_DIR`/`UVNVR_HOST` and
  `docs/configuration.md`.

- M0 engineering foundation: repository rules, MIT license, third-party/patent notices.
- libuvcpp 1.8.1 service with `/healthz` and `/api/v1/system/info`.
- M1 first-run setup and session authentication (scrypt, HttpOnly cookies, CSRF).
- Users, roles and granular Web/API permissions; built-in admin/operator/viewer roles.
- API bearer tokens with per-account listing and revocation.
- Web and API failure lockouts with configurable thresholds.
- Audit log with categories, keyword/date filters, pagination and retention cleanup.
- SQLite migrations for users, roles, sessions, tokens, attempts, settings and audit.
- Live dashboard: CPU, memory, disk, users, sessions and audit counters.
- Vue 3 + TypeScript admin shell: setup/login, cached workspace tabs, users/roles,
  audit and system settings pages; Chinese/English UI.
- Unified brand icon for web, Windows and macOS packaging.
- Linux x64 CI build, HTTP smoke test and M1 integration test.

- ONVIF discovery/probe, RTSP ingest, channel tree and encrypted credentials.
- Live preview grid with saved layouts, main/sub streams and watermark overlays.
- Keyframe-aligned MPEG-TS recording, plan scheduling, SQLite index and loop cleanup.
- Playback over WebSocket across segments with read-ahead and downloads.
- RTSP output with RTP/MP2T (TCP/UDP) and Basic authentication.
- Snapshots, motion/offline/storage alerts, SMTP email and alert UI.
- Performance tier, GPU detection/toggle, time API and help manual.
- Linux deb/tar.gz packaging and Windows portable/NSIS packaging workflow.

### Fixed

- Windows: an installed build under `Program Files` now defaults to
  `%ProgramData%\UvNVR` instead of trying to create `.\data` in a read-only
  directory; data-directory failures report the absolute path and the
  `--data-dir` remedy.
- Windows arm64 (MSVC): `NOMINMAX` keeps the Windows `min`/`max` macros from breaking
  `std::min`/`std::max` in the RTSP server; packaging looks for the executable in
  the build root as well as `build\Release`.
- Release automation now publishes only when the complete six-platform matrix succeeds,
  is idempotent on re-runs, and attaches `SHA256SUMS`.
- macOS packaging resolves `@rpath` dependencies through the build tree and Homebrew
  search paths (previously `dylibbundler` waited on an interactive prompt), writes the
  zip/pkg/dmg to an absolute output path, reports each stage, and bounds
  `dylibbundler`, `zip`, `pkgbuild` and `hdiutil` so a stuck external tool cannot
  consume the whole job.
- The release job now requires every platform job to succeed before publishing.

### Notes

- 0.1.0 is a release candidate. WebRTC, cloud access, face/plate recognition and advanced
  analytics are outside this release. See `docs/development-plan.md` for details.
- 0.1.0 ships an English and a Chinese user guide (`docs/user-guide.md`,
  `docs/user-guide.zh-CN.md`).

## [0.1.0] - 2026-10-21

First release candidate with the complete core NVR loop:

- ONVIF discovery/probe and RTSP/Hikvision/Dahua channel onboarding.
- Live preview, layouts, recording plans, MPEG-TS segments, SQLite index
  and loop/alert storage policies.
- Playback over WebSocket with read-ahead and segment download.
- RTSP output, snapshots, alerts, SMTP email, users/roles, audit and help.
- Linux x64/arm64, Windows x64/arm64 and macOS x64/arm64 packaging matrix.

Known limitations are listed in `README.md`. WebRTC, cloud access and advanced
analytics are outside 0.1.0.
