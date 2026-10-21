# UvNVR 0.6.0

Recording reliability, operator workflow and API control.

## Recording fixes

- **Configured volumes are respected.** The auto-created volume under the data
  directory is now only a fallback; a volume the operator adds is preferred.
  This fixes recordings that went to the C: data directory despite a configured
  disk.
- **Full-day plans save.** A plan painted to midnight (`endMin` 1440) is accepted;
  previously it was dropped, leaving online channels with no recording.
- **Retention 0 is unlimited**, and retention days are actually enforced.
- Segments are flushed while writing and open/finalize errors are logged.
- **Codec changes start a new file.** Switching a camera from H.264 to H.265
  (or back) finalizes the segment, starts a new one and records the codec.
- Windows installs on a non-C: drive keep data under `<install>\data`; the
  installer writes that path and preserves it on uninstall.
- The Windows console switches to UTF-8, and the installer creates a desktop
  shortcut.

## UI and workflow

- **Plan box selection:** drag a rectangle across days/half-hours to paint or
  erase many cells at once.
- **Themes:** Auto (follow the OS), Light and Dark, with a switch on the login
  page and top bar.
- **Channel snapshots:** thumbnails load on first visit, a per-channel button
  refreshes the snapshot and double-click opens a large view; channel `#id` is
  visible in the list.
- **Brand auto-detect:** from ONVIF discovery, use **Add now** or **Auto detect**
  in the channel form. Hikvision/Dahua are recognized and use generated
  main/sub stream paths; names come from the profile/model. Manual RTSP is still
  available for other cameras.
- **Preview:** Play all / Stop all, paging when there are more channels than
  cells, and a Recordings shortcut for the selected channel.
- **Playback:** 0.25×–32× speeds, timeline zoom from 24h to 5m with a
  proportional skip step, and a reverse mode. The Playback page also downloads
  an MP4 for a chosen time range.

## API and time

- **Per-token API scopes** with a grouped selector and select-all. Scopes include
  preview, snapshots, channel query/add/delete/manage, playback/download, alerts,
  plans, storage, time/system, RTSP, audit and users. Empty selection inherits
  the account role; scopes are intersected with the role.
- **Camera time sync:** browser-time and NTP updates now also set the clock on
  ONVIF-capable cameras (Hikvision/Dahua included when ONVIF is enabled). The
  NTP sync endpoint is implemented.
- Recordings expose their codec and file path; playback stops at a codec
  boundary so a player never receives a mixed-codec stream.

## Upgrade notes

- No manual database migration is needed; new columns/tables are created at
  startup.
- Existing recording plans painted to midnight now save correctly. Review any
  schedules that appeared empty.
- If recordings were going to the data directory, add your storage volume again
  or edit it; the newest explicit volume is selected.
- Reverse playback is implemented as timed backward seeks, not a decoded reverse
  stream; browsers cannot decode a reversed GOP.
- Brand auto-detect requires ONVIF to be enabled on the camera. Pure RTSP
  cameras have no control channel for time sync.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus SHA256SUMS, SPDX SBOM and the FFmpeg source/license assets.
The Windows setup creates a desktop shortcut.
