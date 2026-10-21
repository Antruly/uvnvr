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
