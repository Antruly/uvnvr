# UvNVR 0.18.1

Installer and camera onboarding fixes, saved camera credentials and a channel
capacity limit.

## Highlights

- **One-click add with saved camera credentials.** **Settings → Camera
  defaults** stores a default camera username and password. The password is
  encrypted with the instance key and never returned by the API. **Scan
  ONVIF** shows a **Use saved camera credentials** checkbox, pre-checked when
  defaults exist; one-click add and **Add selected** then work without
  retyping the account for every camera. The saved account is also stored on
  the resulting channels, so recording and ONVIF events authenticate the same
  way.
- **Channel capacity, 128 by default.** **Settings → Camera defaults** sets the
  maximum number of channels (1–1024, default 128). New channels, CSV imports
  and bulk adds stop with HTTP 409 `channel_limit` at the cap; the Channels
  page shows `count / limit` and disables add/import at the limit. Lowering
  the limit keeps existing channels.

- **Port changes now apply on upgrades.** The ports page previously wrote
  `%ProgramData%\UvNVR\uvnvr.json` only when the file did not exist, so an
  upgrade from 0.17 kept the old HTTP/RTSP ports. The installer now updates the
  two port values in an existing file (all other keys are preserved) before the
  service starts. It also ends the 0.17 boot task and any running console
  instance before replacing files, so the old process cannot keep the previous
  ports bound.
- **ONVIF cameras stay ONVIF.** A device found by **Scan ONVIF** is always added
  with the ONVIF transport and the stream URIs returned by the camera, and the
  ONVIF service port is taken from the discovery address. Its manufacturer is
  stored only as the brand (for time and encoder fallbacks). Previously a
  camera that reported "Dahua" or "Hikvision" over ONVIF was converted to that
  vendor's protocol and a guessed RTSP path was used, which broke one-click add
  and bulk add for many cameras.
- **Manual ONVIF add reads the profiles.** Enter the host, HTTP/ONVIF port and
  credentials, leave the stream URLs empty and save: UvNVR queries the camera
  and stores the real main/sub stream URIs. **Auto detect** still fills them
  immediately when you prefer.
- **Plain RTSP channels need only the stream URL.** For RTSP the dialog hides
  host, port, credentials, event and ONVIF fields; generic URL templates keep
  their placeholder fields. The name is optional (derived from the host or
  URL), a sub-stream URL enables the sub stream automatically, and the server
  accepts the channel without any other camera settings.
- **Errors are visible in the dialog.** Channel creation failures appear inside
  the add/edit dialog instead of behind the modal, scan/probe/bulk-add feedback
  appears inside the scan dialog, and the save button is disabled while the
  request (including an ONVIF probe) runs.

## Notes

- Existing channels keep their stored protocol. To convert a channel that was
  previously misclassified as Hikvision/Dahua, edit it, set **Protocol** to
  **ONVIF** and run **Auto detect** to refresh the profile URIs.
- The MSI applies explicitly passed non-default `HTTPPORT`/`RTSPPORT` values
  on upgrade and otherwise keeps the existing ports; the Inno Setup wizard
  treats its ports page as authoritative. Both stop a legacy scheduled-task
  process before replacing files.
- No database or configuration-file format changes. The API gains
  `GET /api/v1/channels/onboarding`, the `camera` settings group and the
  `useDefaultCredentials` request flag. `VERSION` is 0.18.1.

## Packages

Windows x64/arm64 (Inno Setup setup, MSI, portable zip), Linux x64/arm64
(deb/rpm/tar.gz), macOS x64/arm64 (pkg/dmg/zip), SHA256SUMS, SPDX SBOM and
FFmpeg source/license assets.
