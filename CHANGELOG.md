# Changelog

All notable changes to UvNVR are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[Semantic Versioning](https://semver.org/).

## [0.13.0] - 2026-10-22

### Added

- **RTSP audio.** The RTSP output now advertises a second track when the source
  carries AAC (ADTS, RFC 3640 AAC-hbr) or G.711 (PCMA/PCMU): SDP includes the
  audio `m=` line and AudioSpecificConfig, SETUP handles `trackID=1` for both
  TCP interleaved and UDP, and per-track RTP/RTCP sequence, SSRC and sender
  reports are maintained.
- **Batched recording writes.** The segment writer flushes when
  `recording.batch_mb` (default 8) or `recording.batch_ms` (default 500) is
  reached instead of after every chunk, while splits and finalization still
  force a flush.
- **Disk and service alerts.** A volume whose free space cannot be read raises a
  `disk_error` alert (at most hourly), and missing FFmpeg or an RTSP bind failure
  raises `service_error` at startup.
- **Storage browser and guards.** `GET /api/v1/storage/browse` lists filesystem
  roots and subdirectories for the Storage page, and adding a volume rejects
  filesystem roots and paths that cannot be written.

### Changed

- RTSP audio is available as AAC or G.711; video-only sources produce the same
  SDP as before.
- Volume creation now proves writability with a temporary file before inserting
  the volume.

## [0.12.0] - 2026-10-22

### Added

- **RTSP Digest authentication.** The RTSP server now advertises both Digest
  (MD5, `qop=auth`, per-process nonce, stored HA1) and Basic during the 401
  challenge. HA1 is derived from the password on setup, user create/update and
  the next successful login for upgraded installations, and both paths enforce
  `rtsp.access`.
- **Real-time event push.** `POST /api/v1/events/ticket` issues a single-use
  ticket for `/ws/events`; the socket pushes `hello`, `alert` and `channel`
  messages. The web UI connects after login, shows a toast for new alerts and
  refreshes the Alerts page in place. Alerts also appear in the main navigation.
- The OpenAPI document includes the events-ticket endpoint.

### Changed

- The Alerts page is now linked from the primary navigation instead of being
  listed as a planned, disabled item.

## [0.11.0] - 2026-10-22

### Added

- **OpenAPI 3.1 and Swagger UI.** `GET /api/v1/openapi.json` returns the spec
  (permission `api.access`) and `GET /api/docs` serves a self-hosted Swagger UI
  that loads its assets from `/api-docs/`, with no external scripts or CDNs.
- **Standard RTP output.** The RTSP server now packetizes H.264 (RFC 6184) and
  H.265 (RFC 7798) as RTP payload type 96 with SDP parameter sets, FU-A/FU
  fragmentation and RTCP sender reports, over TCP interleaved or UDP unicast.
- **Snapshot sizing.** `GET /api/v1/channels/{id}/snapshot` accepts optional
  `width` (0 or 16–4096), `height` (0 or 16–4096) and `quality` (1–31) query
  parameters.

### Changed

- RTSP output no longer sends RTP/MP2T. It waits up to two seconds during
  `DESCRIBE` for the stream's PAT/PMT and parameter sets and returns 503 while
  the channel is offline. Audio is not packetized in this release.
- The RTSP output advertises H.264/H.265 video only; clients that require the
  former MP2T payload must use a different profile.

## [0.10.0] - 2026-10-21

### Added

- **Maintenance mode operations.** `--maintenance` now gates administrator-only
  destructive actions: deleting a recording segment, purging audit entries,
  changing the serial number, setting a display version and formatting a storage
  volume. Each action requires an explicit confirmation and is audited; outside
  maintenance mode they return `403`.
- **Motion detection settings.** **System → Motion detection** and
  `GET/PUT /api/v1/settings` (`motion.enabled`, `motion.sensitivity`,
  `motion.cooldownSeconds`, `motion.region.x/y/width/height`) control the
  frame-difference detector. The region is a percentage rectangle of the 64×36
  sample frame.
- **Audit CSV export.** `GET /api/v1/audit/export` streams the filtered audit
  log as CSV (UTF-8 with BOM) and the Logs page has an **Export CSV** button.
- **Windows snapshot and motion capture.** The Windows build now runs FFmpeg
  through `CreateProcess` pipes for JPEG snapshots and grayscale motion frames
  instead of returning “not wired yet”.
- **GPU-assisted capture.** When GPU acceleration is enabled, snapshot and
  motion sampling try `-hwaccel auto` first and fall back to software decoding.

### Changed

- The performance tier now limits simultaneous motion sampling (low 2, medium
  4, high 8 channels), so the tier affects runtime behavior instead of only
  being displayed.
- Formatting a volume removes UvNVR's `uvnvr` recording tree and its index rows;
  it does not run a filesystem `mkfs` or touch unrelated files.
- The display version override affects the UI and system overview only;
  `/healthz` continues to report the build version.

### Fixed

- The dashboard reported channel and recording counts as zero; both counts now
  come from the database.

## [0.9.0] - 2026-10-21

### Added

- **Event-triggered recording.** Motion alerts and ONVIF person, tamper and
  line/field-crossing events now produce clips for channels whose active plan
  includes event recording. A clip starts with buffered pre-roll, is extended by
  follow-up alerts and stops after the configured post-roll.
- Event recording settings under **System → Event recording** and in
  `GET/PUT /api/v1/settings` (`event.enabled`, `event.preSeconds`,
  `event.postSeconds`, `event.maxSeconds`).
- The recording-status API reports `recordType` and `untilMs`; the Storage page
  distinguishes scheduled and event clips while they are written.

### Changed

- Event plans (`event`/`both`) now actually record. They were stored and shown in
  the weekly planner before, but did not start a recorder.
- Event clips are indexed with `record_type=event`, so playback can identify
  them alongside scheduled segments.

## [0.8.0] - 2026-10-21

### Added

- **Bulk channel import from CSV.** The Channels page accepts
  `host,username,password,brand,name,httpPort` rows (header optional). Each row
  is detected through ONVIF/Hikvision ISAPI/Dahua CGI and created, with a
  progress counter, duplicate-host protection and per-row error reporting.
- **Recording status visibility.** `GET /api/v1/recordings/status` reports every
  active recorder with its current segment path, bytes, start time and volume,
  plus the volume used for new recordings. The Storage page shows the same live
  panel (refreshed every five seconds), so operators can confirm that data is
  actually being written.

### Changed

- The Storage page labels the active volume for new segments and lists the
  current files being written.

## [0.7.0] - 2026-10-21

### Added

- Hikvision ISAPI and Dahua CGI HTTP Digest fallback for cameras without ONVIF:
  auto-detect reads the device name/model and brand, and time sync sets the
  camera clock even when ONVIF is disabled.
- `POST /api/v1/channels/detect` tries ONVIF first, then Hikvision ISAPI/Dahua
  CGI (or brand-first when a brand is supplied). Channels gain an
  `httpPort` (default 80) used for ONVIF/brand HTTP and time sync, separate from
  the RTSP port.
- A WiX-based **x64 MSI** alongside the NSIS setup. It supports
  `HTTPPORT`/`RTSPPORT` properties, creates desktop and Start-menu shortcuts, the
  boot task and firewall rule, writes a non-BOM JSON config, and preserves data
  on uninstall. The release workflow installs and uninstalls it as a smoke test.
- Playback automatically continues with a fresh player at a codec boundary, so an
  H.264/H.265 switch needs no manual seek.

### Changed

- `SyncCameraTimes` prefers ONVIF for ONVIF channels and falls back to the brand
  HTTP API; Hikvision/Dahua channels try their HTTP API first and ONVIF second.
- The channel form exposes the HTTP/ONVIF port next to the RTSP port.

## [0.6.0] - 2026-10-21

### Fixed

- Recording now uses the storage volume the operator configured. The
  auto-created volume under the data directory is only a fallback, and the
  newest explicit volume wins.
- Plans can cover a full day (`endMin` 1440); previously a schedule painted to
  midnight was silently rejected, which left channels online but recording
  nothing.
- Retention `0` means unlimited; retention cleanup now actually deletes expired
  segments (the setting was previously unused).
- Segments are flushed as they are written, and directory/open/finalize errors
  are logged instead of failing silently.
- A codec change (H.264 ↔ H.265) finalizes the current segment and starts a new
  one, so a file never mixes codecs; the recordings API exposes the codec and
  playback stops at a codec boundary.
- Windows installs on a non-C: drive default to `<install>\data`; the installer
  writes that path and preserves it on uninstall. The Windows console uses UTF-8.
- The Windows installer creates a desktop shortcut.

### Added

- Drag a rectangle in the recording plan grid to paint many rows/cells at once.
- Auto / light / dark theme with system-preference following, persisted in the
  browser; switches on the login page and top bar.
- Channel thumbnails captured on first load, a per-channel snapshot button and a
  large preview (double-click the thumbnail); channel `#id` is shown in the list.
- ONVIF discovery quick-add and an **Auto detect** action: Hikvision/Dahua
  manufacturers are detected and use their generated main/sub stream templates
  (no manual RTSP URL); the channel name comes from the profile/model.
- Preview **Play all** / **Stop all**, page navigation when there are more
  channels than cells, a Recordings shortcut, and a time-range MP4 download
  form in Playback.
- Playback speeds 0.25×–32×, zoomable timeline (24h down to 5m) with a
  proportional skip step, and a reverse mode (backward seek stepping).
- Per-token API scopes with a grouped, select-all selector; granular
  `channel.create` / `channel.delete` permissions (manage implies both).
- Time synchronization now also updates ONVIF-capable cameras (including
  Hikvision/Dahua through their ONVIF endpoint) after browser-time or NTP
  updates; the NTP sync endpoint is implemented and reports camera results.
- MediaMTX-backed recording integration test and API token scope test.

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
