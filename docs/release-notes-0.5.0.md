# UvNVR 0.5.0

Camera-side events.

## Highlights

- **ONVIF PullPoint events.** ONVIF channels get a **Use camera ONVIF events**
  toggle. When enabled, UvNVR resolves the camera's event service through
  `GetCapabilities` (falling back to `http://host:port/onvif/events`), creates a
  PullPoint subscription, renews it before expiry and polls for notifications
  with reconnection backoff.
- **Alert mapping.** Motion, person, tamper and line/field-crossing topics become
  `motion`, `person`, `tamper` and `intrusion` alerts in the alert center and
  email pipeline. Notifications explicitly reporting false/cleared state are
  ignored, and each channel/type has a 60-second cooldown.
- **API and UI.** Channels accept and return `eventsEnabled`; the channel form
  exposes the toggle. A mock-camera integration test drives the complete
  GetCapabilities → CreatePullPointSubscription → PullMessages → alert flow in
  CI.

## Upgrade notes

- Existing channels default to `eventsEnabled = false`; enable it per ONVIF
  channel after upgrading.
- No database migration or configuration change is required. The event poller
  uses the channel's host, port and credentials.
- Only `http://` ONVIF event services are supported, consistent with the
  existing ONVIF probe.

## Known limitations

- Event names differ between manufacturers; topics are matched by the standard
  `RuleEngine` fragments above. Unrecognized topics are ignored.
- The poller uses PullPoint. Basic Notification Streaming / WS-BaseNotification
  push endpoints are not implemented.
- Events create alerts but do not yet trigger a dedicated event recording clip.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus SHA256SUMS, SPDX SBOM and the FFmpeg source/license assets.

## FFmpeg and patent notice

Official packages invoke an LGPL FFmpeg CLI as a separate process; no FFmpeg
code is linked into UvNVR. This project does not grant any patent license for
H.264, H.265/HEVC, AAC or any other codec.
