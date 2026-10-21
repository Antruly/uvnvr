# UvNVR 0.9.0

Event-triggered recording.

## Highlights

- **Event clips.** Motion alerts and ONVIF person, tamper and line/field-crossing
  events now start a recording when the channel's weekly plan includes **Event**
  or **Timed + event**. Previously those plan cells were stored and displayed but
  did not record anything.
- **Pre-roll and post-roll.** Event-capable streams are buffered continuously, so
  a clip begins a few seconds before the alert. Each follow-up alert extends the
  current clip without exceeding the configured maximum; the clip is finalized
  after the post-roll even if the camera goes quiet.
- **Settings.** **System → Event recording** controls the global toggle, pre-roll
  (0–60 s, default 10), post-roll (1–3600 s, default 30) and maximum clip length
  (1–3600 s, default 300). The same values are available as the `event` group in
  `GET/PUT /api/v1/settings`.
- **Visibility.** `GET /api/v1/recordings/status` now includes `recordType` and
  `untilMs`, and the Storage page's **Active recordings** panel shows whether each
  row is a scheduled or an event clip. Event segments are indexed with
  `record_type=event` and play back through the normal timeline.

## Upgrade notes

- No database migration or configuration change is required. The new settings
  default to event recording enabled with 10/30/300 seconds.
- Event recording is gated by the weekly plan: paint **Event** (or
  **Timed + event**) cells for the channels that should react to alerts, then
  enable motion or ONVIF events for those channels.
- Event clips use the active storage volume and are subject to the same loop,
  retention and space policies as scheduled recordings.
- A clip whose stream delivered no media (for example, the camera was offline)
  produces no empty index row or file.

## Known limitations

- Pre-roll starts when the scheduler observes the event plan and the stream is
  delivering MPEG-TS; a camera reconnect resets the buffer.
- Unrecognized ONVIF topics are ignored, and pure RTSP cameras have no ONVIF
  events. Motion detection remains the experimental low-resolution detector.
- The maximum clip length caps extensions. A new alert after a clip has reached
  the maximum starts a new clip once the previous one is finalized.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
