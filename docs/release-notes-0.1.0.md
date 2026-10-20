# UvNVR 0.1.0

First release candidate of UvNVR, a software NVR for ONVIF and RTSP cameras.

## Highlights

- ONVIF WS-Discovery/probe and manual RTSP/Hikvision/Dahua onboarding.
- Main/sub-stream live preview with 1/4/8/16/32 and custom grids, saved layouts and watermarks.
- Plan-driven recording to keyframe-aligned MPEG-TS segments with a SQLite index.
- Seamless playback across segments, timeline seeking, read-ahead and segment download.
- RTSP output (`/live/{channel}/{main|sub}`) with RTP/MP2T, TCP interleaved and UDP.
- Snapshots, motion/offline/storage alerts, SMTP email (QQ preset) and alert center.
- First-run admin setup, sessions/CSRF, users/roles, API tokens, Web/API lockouts and audit log.
- Dashboard with CPU, memory, disk, channel, session and alert counters.
- Performance tier, GPU detection/toggle, browser/NTP time synchronization.
- Chinese/English UI, cached workspace tabs and an in-app help manual.

## Packages

- Windows x64 / arm64: NSIS setup and portable zip.
- Linux x64 / arm64: deb, rpm and tar.gz.
- macOS x64 / arm64: pkg, dmg and zip.

## Install

Linux: install the deb/rpm and open `http://<host>:8080`; complete first-run setup.
Windows: run the setup and open the shortcut; data is stored under `%ProgramData%\UvNVR`.
macOS: install the pkg; open `http://<host>:8080`; data is stored under
`/Library/Application Support/UvNVR`.

See `docs/deployment.md` for ports, data directories, maintenance mode and backups.

## Known limitations

- ONVIF probing currently supports `http://` device services only.
- Motion detection is an experimental low-resolution frame-difference detector, not AI.
- Browser playback uses MSE; H.265 depends on the browser/hardware. Prefer an H.264 sub stream.
- RTSP output uses RTP/MP2T (payload type 33); some clients expect H.264 RTP.
- WebRTC, cloud access and advanced analytics are outside 0.1.0.
- macOS packages are ad-hoc signed unless a Developer ID is configured.

## FFmpeg and patent notice

Official packages invoke an LGPL FFmpeg CLI as a separate process; no FFmpeg code is linked
into UvNVR. FFmpeg source and license assets are attached to this release.

This project does **not** grant any patent license for H.264, H.265/HEVC, AAC or any other
codec. If you build or bundle x264/x265 or another GPL/patent-bearing component, GPL and
patent obligations are your responsibility. See `THIRD_PARTY_NOTICES.md`.
