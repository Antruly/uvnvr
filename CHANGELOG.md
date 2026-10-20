# Changelog

All notable changes to UvNVR are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

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

### Notes

- 0.1.0 is a release candidate. WebRTC, cloud access, face/plate recognition and advanced
  analytics are outside this release. See `docs/development-plan.md` for details.

## [0.1.0] - Not released

Planned first release. See `docs/development-plan.md`.
