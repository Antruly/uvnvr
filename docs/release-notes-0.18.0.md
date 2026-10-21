# UvNVR 0.18.0

Live playback and polish.

## Highlights

- **Watch the segment that is still recording.** `GET /api/v1/recordings`
  appends the open `.ts.part` segment with `active: true`, its start time,
  bytes and codec. The playback service follows the growing part file, switches
  to the finalized file when the recorder rotates, and waits for the next
  active segment. The Playback page refreshes its list every five seconds and
  labels the live row **Recording**.
- **Draggable timeline.** Press and drag on the timeline to move the cursor;
  releasing seeks to that position. Clicking a recorded block still selects it.
- **Snapshot loading state.** Channel thumbnails show a small spinner until the
  JPEG arrives and keep the camera icon when capture fails.
- **Unlimited retention by default.** New volumes default to loop mode with
  retention `0` (never expire); the schema/API defaults changed from 30 days.
- **Localized audit values.** Level, action and result are translated when the
  UI language is Chinese; category translations were already present.
- **Light-theme control cleanup.** Plan brushes (timed/event/both/erase),
  segmented controls, layout tabs and small active buttons no longer render
  white text on white. Warning/violet/danger variants have their own tokens.
- API-token scope groups alternate row backgrounds for easier scanning.
- **System service management.** **Settings → System service** shows whether
  UvNVR is registered, running and enabled at boot, and can register or remove
  the service. Windows uses the Service Control Manager; Linux writes and
  enables a systemd unit. Both actions restart the recorder and the page
  reconnects; unregistering starts a console instance again. New flags:
  `--service` (Windows SCM entry point) and `--open` (open the web UI in the
  browser after startup).
- **Updated Windows packaging.** The Inno Setup wizard has a **Startup** page:
  keep **Register UvNVR as a Windows service** for production, clear it for a
  console install. There is now one desktop shortcut: with the service it opens
  the web UI; without it, it starts UvNVR and opens the browser. Upgrades stop
  the running service before replacing files. The MSI registers the service
  too.
- **About and help pages.** About now covers the project, repository, issues,
  documentation, build information, the MIT license and a deployment/risk
  notice. Help links to the self-hosted Swagger UI and OpenAPI document and
  describes the event, snapshot, encoder, alert and RTSP interfaces.
- **New application mark.** A play symbol inside an aperture ring, regenerated
  for Windows, macOS, Linux, the installer, PWA and favicon assets. The Windows
  executable embeds it, so shortcuts and Explorer no longer show a generic icon.

## Fixed

- Swagger UI loaded `/api/openapi.json`, which does not exist; it now uses
  `/api/v1/openapi.json`.
- The Windows installer created two desktop shortcuts and the console shortcut
  did not launch the application.

## Notes

- Playback of an active segment is read-only; download/export/delete are
  disabled for the live row until it is finalized.
- Retention `0` still respects the volume's maximum-usage and loop/alert policy;
  only age-based deletion is disabled.
- The live row disappears once the recorder finalizes the segment and the list
  refreshes; the finalized row remains indexed.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
