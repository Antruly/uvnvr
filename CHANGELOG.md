# Changelog

All notable changes to UvNVR are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[Semantic Versioning](https://semver.org/).

## [0.20.1] - 2026-11-27

### Added

- **Real plate recognition.** The HyperLPR3 engine (MNN backend, Apache-2.0) is
  back behind `UVNVR_ENABLE_HYPERLPR`; the stub stays the default when the
  engine is not compiled in.
- **GM/T 0024 signalling.** `gb28181.transport=tls-gm` runs SIP over NTLS with
  the double-certificate model, `gb28181.tls.*` carries the credentials, and a
  profile that cannot validate refuses to start instead of falling back to
  plaintext.
- **Full-build pipeline.** `full-build.yml` produces Linux and Windows packages
  with AI, GB/T 28181, GB 35114 and the hardening modules all compiled in.

### Fixed

- The AI probe endpoint never passed the face model path to the engine, so it
  always reported the stub even with InspireFace configured.
- The GB/T 28181 module links libuv itself instead of relying on the
  executable's link order.
- The vendored plate helper computes the four-point homography itself, because
  OpenCV 5 no longer exports `getPerspectiveTransform`.
- A static Tongsuo build needs the Winsock helpers after `libcrypto` on MinGW.

## [0.20.0] - 2026-11-26

### Added

- **GB/T 28181 device stack (opt-in, `UVNVR_ENABLE_GB28181`).** Self-hosted SIP
  UA (REGISTER/Digest/keepalive/backoff), MANSCDP catalog and device status,
  PS/RTP media with INVITE/BYE dialogs, RecordInfo playback with speed/pause/seek,
  PTZ control and alarm reporting.
- **GB 35114 video security (opt-in, `UVNVR_ENABLE_GB35114`).** A-level SM3/SM2
  frame signatures, VKEK negotiation, B-level SM4 stream encryption and a
  reference interop run against a 35114 platform.
- **Security hardening (`UVNVR_ENABLE_SECURE`, `CHANNEL_ACL`, `CONFIG_INTEGRITY`).**
  SM3 audit hash chain, signed firmware packages, SM4 data-at-rest encryption,
  channel/group level object authorization and configuration integrity checks.
- **Session revocation.** Changing a password or role, or disabling an account,
  now drops that user's sessions and API tokens immediately.
- **AI.** HyperLPR3 plate engine, InspireFace face engine, snapshot quotas with
  face deduplication, alert rules, an events page and SDK AI subscriptions.
- **GA/T 2155.2 gap matrix** and 35 executable self-test cases in `docs/`.

### Fixed

- Snapshot writes are bounded by daily count, directory size and free space, so
  a static face can no longer fill the disk.
- Windows builds keep X509 name handling const-correct for OpenSSL 3 on MinGW.

## [Unreleased]

### Added

- **GB 35114 A 级 → B 级平台联调 (opt-in, GH-27).** `tools/gb35114_interop`
  plays the device and a reference 35114 platform over real UDP: SIP MESSAGE
  carries the mutual SM2 ClientHello/ServerHello, the signed and SM2-wrapped
  VKEK delivery and the signed acknowledgement; RTP payload type 96 carries
  the SM4-CTR/HMAC-SM3 sealed H.264 stream; a side channel carries the A-level
  SM3/SM2 frame signatures. The platform verifies the device identity,
  verifies every signed key frame, decrypts all 20 frames with the negotiated
  VKEK, requires the VkekAck to be bound to the delivered session/key id/key
  version/sequence, and rejects a one-bit signature change and a one-bit
  ciphertext-body change. `tests/integration/gb35114_interop.py` drives both
  processes, parses the endpoint pcap, and runs two forged-identity controls
  (wrong device key, wrong platform key) that must fail closed. The record and
  capture are in `docs/gb35114-interop.md` and `docs/evidence/`. Built only with
  `UVNVR_ENABLE_GB28181=ON` and `UVNVR_ENABLE_GB35114=ON` on non-Windows hosts,
  and not linked into the server.
- **GB/T 28181 云台控制与报警上报 (opt-in, P6.2e).** `DeviceControl`
  `PTZCmd` is decoded with byte/checksum/preset validation and normalised to
  pan/tilt/zoom velocities; the server dispatches it off the SIP loop to the
  ONVIF ver20 PTZ service (`ContinuousMove`, `Stop`, `SetPreset`,
  `GotoPreset`, `RemovePreset`). A new `PTZ_CONTROL` private-protocol opcode
  drives the same backend. Every `RaiseAlert` (including the AI plate/face
  workers) is published as a MANSCDP `Alarm` MESSAGE and fanned out to
  `Event: Alarm` subscribers through SIP NOTIFY. SUBSCRIBE for `Catalog` and
  `Alarm` supports refresh and `Expires: 0` unsubscribe. Still behind
  `-DUVNVR_ENABLE_GB28181=ON` (default off) and started only when
  `gb28181.enabled=1`; existing paths are unchanged. See
  `docs/gb28181-ptz-alarm.md`; evidence is
  `tests/gb28181_manscdp_test.cpp`, `tests/onvif_ptz_test.cpp` and
  `tests/integration/gb28181_ptz_alarm.py`.
- **GB/T 28181 录像查询与回放 (opt-in, P6.2d).** `RecordInfo` answers from the
  local `recordings` index (including active `.part` segments) with the
  channel's 20-digit identifier, time range and MANSCDP record type. Playback
  INVITEs carrying `s=Playback` and `u=<channel>:<start>:<end>` are answered
  with an RTP/PS dialog; SIP INFO `Control/Playback` implements `Scale`
  (forward speed, `0` pause) and `Range` seek. Live preview and playback share
  the same TS demuxer, PS muxer and RTP session path. Still behind
  `-DUVNVR_ENABLE_GB28181=ON` (default off) and only started when
  `gb28181.enabled=1`; existing preview/recording/ONVIF paths are unchanged.
  See `docs/gb28181-playback.md`; the platform-side check is
  `tests/integration/gb28181_record_playback.py`.
- **GB/T 28181 MANSCDP codec and channel catalog (opt-in, P6.2b).** Catalog,
  DeviceInfo, DeviceStatus and Keepalive XML encoding/decoding, 20-digit
  identifier generation and validation, and a mapping from the local channels
  and channel groups to catalog items with `ParentID`. `ManscdpResponder`
  answers inbound queries, and the SIP user agent now dispatches inbound
  MESSAGE requests to a callback and can send the response MESSAGE back. Still
  compiled only with `-DUVNVR_ENABLE_GB28181=ON`; default deployments are
  unchanged. See `docs/gb28181-manscdp.md`.
- **GB/T 28181 SIP user agent (opt-in, P6.2a).** A self-hosted SIP stack for
  the lower-device role: UDP/TCP transport on libuvcpp, REGISTER with Digest
  authentication and 401/407 retry, Expires refresh, MANSCDP-style Keepalive
  heartbeats, SIP retransmission for UDP and exponential reconnect backoff.
  Message/URI/Digest helpers plus `sip_reg_cli` and a local Python fake
  registrar are included. Build with `-DUVNVR_ENABLE_GB28181=ON`; the module is
  not linked into the server yet, so default deployments are unchanged. See
  `docs/gb28181-sip.md`.
- **GB/T 28181 PS/RTP media path (opt-in, P6.2c).** H.264/H.265 Annex B
  elementary streams are muxed into the Appendix C MPEG-2 PS layout (pack,
  system header, PSM with CRC-32/MPEG-2 and PES with 90 kHz PTS/DTS), then
  packetized as RTP payload type 96 with sequence, SSRC and per-frame marker
  bits. UDP, TCP active and TCP passive (RFC 4571 length framing) are
  supported, together with SDP build/parse and INVITE answer negotiation
  (`a=setup`, `PS/90000`, `y=`). Build with `-DUVNVR_ENABLE_GB28181=ON`;
  `gb28181_ps_cli` and an ffmpeg-backed integration check are included. The
  module is not linked into the server yet, so default deployments are
  unchanged. See `docs/gb28181-ps-rtp.md`.
- **GB/T 28181 RTCP and INVITE/BYE media dialogs (opt-in, P6.2c).** The media
  session now emits compound RTCP Sender Reports with SDES CNAME, consumes
  Receiver Reports (loss/jitter/RTT), parses PLI and FIR key-frame requests
  and RTCP BYE, and supports NACK diagnostics. A dialog manager binds the
  stream to INVITE/ACK/BYE: SDP offer/answer with SSRC and port allocation,
  idempotent retransmissions, re-INVITE replacement, TCP passive accept after
  ACK, ACK timeout, and RTCP BYE on close. `gb28181_session_cli` and a
  platform-side integration script validate all three transports and decode
  the received PS with ffmpeg. Still behind `-DUVNVR_ENABLE_GB28181=ON`
  (default off) and not linked into the server. See `docs/gb28181-ps-rtp.md`.
- **GB 35114 A-level I-frame signatures.** `src/gb35114/sign.{h,cpp}` demuxes
  MPEG-TS or Annex B H.264 into access units, digests each key frame with SM3
  and signs the canonical binding (channel, frame index, PTS, size, digest)
  with the device SM2 key. Records travel in an independent 88-byte-headered
  stream, so the media bytes are unchanged; `tools/gb35114_verify` signs a
  recording into a sidecar, verifies it against a public key or device
  certificate, and detects a one-byte change. The module is behind
  `UVNVR_ENABLE_GB35114` (default OFF) and is not linked into the server. See
  `docs/gb35114-a-sign.md`.
- **GB 35114 B-level video encryption.** An optional `gb35114` module wraps a
  VKEK with the device SM2 certificate, derives per-stream keys with HKDF-SM3,
  and seals H.264 access units with SM4-CTR plus an HMAC-SM3 tag before PS
  muxing. A `gb35114_cli` platform tool and an end-to-end PS/RTP test decrypt
  the stream and decode the picture. The module is off by default and is not
  linked into the server; see `docs/gb35114-b-level.md`.
- **AI alert rules.** Plate recognition now supports a blacklist and a
  whitelist (`ai.plate.blacklist` / `ai.plate.whitelist`); whitelist hits never
  alert, blacklist hits insert an `alerts` row linked to the
  `ai_plate_events` row and push it on `/ws/events`. The legacy
  `ai.plate.watchlist` is still read as a blacklist for existing installs.
  Face recognition keeps its 1:N cosine match with `ai.face.matchThreshold`
  and adds `POST /api/v1/ai/faces/search` for explicit 1:N searches; an
  omitted search threshold inherits the configured value, and equal scores
  resolve to the lower face id.
- **Alert acknowledgement flow.** Alerts carry `source_type`/`source_id`,
  snapshot and source URLs; `POST /api/v1/alerts/{id}/ack` accepts an optional
  note and records the operator and timestamp. The alerts page shows AI
  thumbnails, source-event badges, type filters and the acknowledgement note.
- **Parallel LAN discovery.** The scan endpoint now sends ONVIF
  WS-Discovery, Hikvision SADP and Dahua DHDiscover probes at the same time and
  collects replies for one configurable window (default 4 s). Results carry
  MAC, serial number, firmware and a `sources` list; devices seen by several
  protocols are merged by MAC, serial and IP. The scan also probes each local
  IPv4 interface, so cameras on a second NIC are found. The legacy
  authenticated `/24` HTTP sweep is now opt-in with `deepScan: true`.
- `scan.windowMs`, `scan.onvif`, `scan.sadp`, `scan.dahua` and the multicast
  target lists are stored settings and can be overridden per scan request.
- `POST /api/v1/channels/scan` is an alias of `/api/v1/channels/discover`.

## [0.19.0] - 2026-11-23

### Added

- **Private protocol and C SDK on port 1515.** The server now accepts the
  native protocol over TCP and UDP/QUIC on the same port (QUIC by default,
  TCP as fallback), documented in `docs/private-protocol.md`. Authentication
  returns a short-lived session id that subsequent calls carry. The C SDK
  (`sdk/include/uvnvr_sdk.h`, C++/libuvcpp internals) mirrors the REST v1
  surface for queries, live preview and recording download, and picks the
  transport by itself. Windows x64 and Linux packages ship the DLL/so, the
  import library, the header and three samples.
- **Recording calendar on the playback page.** A month grid highlights the
  days that have recordings (`GET /api/v1/recordings/days`, grouped by the
  configured time zone) and leaves empty days blank. Channels are picked from
  a status list instead of a dropdown.
- **Large playback player with fullscreen.** The player fills the main column,
  the calendar moved into a popover, playback speed runs from 0.25x to 32x with
  +/- buttons and a reset control outside the video, and `F` toggles
  fullscreen.

### Changed

- Snapshots requested without a size still return the native resolution; the
  channel page now asks for 480x270 thumbnails and 1280x720 previews, which
  takes a 1080p channel from ~1.0 s to ~0.17 s per image.

### Fixed

- ONVIF cameras that publish their media service on a separate address (for
  example `/onvif/media_service`) can be added again: the probe resolves the
  media XAddr from `GetCapabilities` instead of asking the device endpoint,
  retries without the WS-Security header when a camera rejects it after
  accepting the credentials, and bounds the TCP connect so a powered-off
  camera fails in seconds instead of hanging the request.
- Windows SDK clients no longer time out or hang at connect: the library
  stopped linking a second libuv, whose `uv_async_t` layout differed from the
  one inside `libuvcpp.dll`, so the event loop never woke up.

## [0.18.1] - 2026-11-19

### Added

- **Saved camera credentials for one-click add.** **Settings → Camera
  defaults** stores a default camera username and password (encrypted with the
  instance key). **Scan ONVIF** pre-selects **Use saved camera credentials**
  and sends `useDefaultCredentials`, so one-click and bulk add work without
  retyping credentials. The password is never returned by the API or written
  to the audit log; `GET /api/v1/channels/onboarding` exposes only the
  username and whether a password is stored.
- **Configurable channel capacity, 128 by default.** New channels are rejected
  with HTTP 409 `channel_limit` after the limit (1–1024, set on the same
  Settings card). The Channels page shows the count/limit and disables add and
  CSV import at the cap.

### Fixed

- **Installer port changes now take effect on upgrades.** An existing
  `uvnvr.json` is no longer left untouched: the HTTP and RTSP ports entered on
  the ports page are written into it while every other setting is preserved.
  The installer also stops the 0.17 boot task and any running console instance
  before replacing files, so an old process cannot keep the previous ports
  bound.
- **ONVIF cameras stay ONVIF when added from discovery.** The manufacturer no
  longer switches the channel to the Hikvision/Dahua protocol; the stream URIs
  returned by the camera are used, and the ONVIF service port is taken from the
  discovery address. One-click and bulk add now work for ONVIF devices from any
  vendor (previously a Dahua-labelled ONVIF camera was stored as a Dahua channel
  with a guessed RTSP path).
- Scan, probe and bulk-add feedback now appears inside the scan dialog; before,
  those messages were rendered behind the modal.
- **Manual ONVIF add without stream URLs** reads the profiles from the camera
  using the host, HTTP port and credentials, then stores the real URIs. The
  brand is filled from the ONVIF device information.
- **Plain RTSP channels need only the stream URL.** RTSP/generic channels hide
  the camera-specific fields, the channel name is optional (derived from the
  host or URL), and a sub-stream URL enables the sub stream automatically.
  The backend accepts an RTSP channel without host/port/credentials and no
  longer returns `invalid_name` when the name is blank.
- Channel creation errors are shown inside the add/edit dialog instead of
  behind it, and saving is disabled while the request runs.

## [0.18.0] - 2026-10-22

### Added

- **Playback of the segment being recorded.** `GET /api/v1/recordings` now
  includes the open `.ts.part` segment with `active: true`, and the playback
  service follows it through a live active-segment provider: it reads the
  growing part file, switches to the final file at rotation, and waits for the
  next segment. The playback list refreshes every five seconds and marks the
  live row as **Recording**.
- **Draggable playback timeline.** Pointer drag on the timeline moves the cursor
  and seeks on release; clicking still works.
- Snapshot thumbnails show a loading spinner until the JPEG arrives.
- Audit values (level, action, result) are localized in Chinese.
- **System service management.** **Settings → System service** reports the
  service state and registers or unregisters it. Windows uses the SCM
  (`UvNVR`, automatic start, `LocalSystem`); Linux writes and enables an
  `/etc/systemd/system/uvnvr.service` unit. The recorder restarts after either
  action, and unregistering starts a console instance again. A new
  `--service` flag is the Windows SCM entry point; `--open` starts the server
  and opens the web UI in the browser.
- **Windows installer options.** Inno Setup now offers a **Startup** task
  (service registration, checked by default; console otherwise), creates a
  single desktop shortcut that adapts to that choice, and stops the existing
  service before upgrading. The MSI registers the service instead of a boot
  task.
- **About page** now describes the project, repository, issues and docs links,
  build information, the MIT license and a deployment/risk notice.
- **Help page** links to the self-hosted Swagger UI (`/api/docs`) and the
  OpenAPI document, and documents events, snapshots, encoder, alerts and RTSP
  authentication accurately.
- **New icon.** A play mark inside an aperture ring replaces the lens mark;
  all platform, installer, PWA and favicon assets are regenerated from one SVG
  with headless Chrome, and the Windows executable now embeds the icon so
  shortcuts and Explorer show it.

### Changed

- New storage volumes default to retention **0 (unlimited)** and loop mode; the
  schema and API defaults use 0 instead of 30 days.
- API-token scope groups use alternating row colors so long checklists are
  easier to follow.
- Light theme: plan brushes (timed/event/both/erase), segmented controls,
  layout tabs and small active buttons no longer use white text on a white
  background; accent, warning, violet and danger variants use theme tokens.

### Fixed

- The Swagger UI page requested `/api/openapi.json` (404) instead of
  `/api/v1/openapi.json`.
- The Windows installer created two desktop shortcuts, and the non-service
  shortcut did not start UvNVR.

## [0.17.0] - 2026-10-22

### Added

- **ONVIF discovery multi-select.** Scan results now have checkboxes, a
  **Select all** toggle and **Add selected (n)**. Each row shows its own
  progress, `Added` state and error message; added hosts are not created twice.
- **Inno Setup Windows installer.** The NSIS setup was replaced with a modern
  Inno Setup 6 installer for x64 and arm64:
  - English and Simplified Chinese wizard languages (UTF-8 script and language
    file), with a working language selection.
  - Service-ports page, `%ProgramData%\UvNVR\uvnvr.json` creation, scheduled
    task and firewall rule.
  - Start-menu shortcuts plus **desktop shortcuts** for the web UI and the
    application (checked by default).
  - Uninstall removes the task, firewall rule and shortcuts while preserving
    the data directory.
- UI refresh: refined surface hierarchy, spacing, panels, buttons, forms,
  tables, status pills, modal and login composition; redesigned icon set.
- The dashboard hero now shows channel/recording/user/storage counters, and the
  stale milestone copy was replaced with the current feature summary.

### Fixed

- Clicking **Probe** in the ONVIF scan modal opened the channel form behind the
  scan dialog; the scan dialog now closes so the form is usable.
- ONVIF scan errors were shown behind the modal; they now appear inside it and
  next to the failing device.
- Light theme: sidebar, top bar, tabs, token form, scope tree, status pills,
  notices, modals and authentication panels now use theme tokens instead of
  dark hard-coded colors.
- Removed the empty "M3 foundation" sidebar section and the raw
  `system.scopes.*` labels in the API-token scope list.

### Changed

- Windows console output already switches to UTF-8 at the start of `main`
  (`SetConsoleOutputCP(CP_UTF8)` / `SetConsoleCP(CP_UTF8)`).

## [0.16.0] - 2026-10-22

### Changed

- **Recording I/O now goes through libuv file requests.** The segment writer no
  longer uses `std::ofstream`; it opens, writes and closes through `uv_fs_*` on
  a private `uv_loop` per recorder. On Linux, libuv hands these requests to
  **io_uring** automatically when the kernel supports it (verified with
  `strace`: `io_uring_setup`/`io_uring_enter` during recording), falling back to
  its threadpool otherwise.
- `recording.batch_mb`/`recording.batch_ms` now control the `fsync` cadence
  (`uv_fs_fsync`); each write is already completed asynchronously.
- CMake links libuv explicitly, since libuvcpp links it privately.

### Notes

- No database, configuration or API changes.
- The private loop is used only for filesystem requests; the web loop is not
  blocked by recording I/O.

## [0.15.0] - 2026-10-22

### Added

- **Hikvision ISAPI encoder control.** `GET/PUT /api/v1/channels/{id}/encoder`
  now reads and updates `/ISAPI/Streaming/channels/101`, including codec
  (`H.264`/`H.265`), resolution, frame rate (stored by ISAPI in hundredths) and
  `vbrUpperCap`/`videoBitrate`.
- **Dahua CGI encoder control.** The same endpoint reads `Encode[0].MainFormat[0]`
  through `configManager.cgi?action=getConfig&name=Encode` and applies
  `Compression`, `Width`, `Height`, `FPS` and `BitRate` through `setConfig`.
- The encoder endpoint selects ONVIF, Hikvision or Dahua from the channel
  protocol (or the detected brand) and normalizes `H.264`/`H.265` to
  `H264`/`H265` in API responses and stored profiles.

### Notes

- Brand encoder quality is not exposed; the API returns `quality: 0` and ignores
  it for ISAPI/CGI updates.
- Hikvision firmware variants that use `videoBitrate` instead of `vbrUpperCap`
  are handled by falling back to that tag when applying a bitrate.

## [0.14.0] - 2026-10-22

### Added

- **Per-channel camera encoder configuration.** ONVIF channels expose
  `GET/PUT /api/v1/channels/{id}/encoder`, backed by
  `GetVideoEncoderConfigurations` and `SetVideoEncoderConfiguration` on the
  camera's media service. Encoding (H.264/H.265), width, height, frame rate,
  bitrate (kbps) and quality can be read from and applied to the main stream.
- The channel edit dialog has a **Camera encoder** section with **Read from
  camera** and **Apply to camera** actions. Applied values are stored in the
  channel's `media.encoder` and returned by the channel API.

### Notes

- Encoder control is available for ONVIF channels; Hikvision ISAPI and Dahua CGI
  encoder setters are not implemented yet. Zero values keep the camera's current
  setting for that field.

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
