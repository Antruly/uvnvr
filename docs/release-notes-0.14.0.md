# UvNVR 0.14.0

Camera encoder control.

## Highlights

- **Read and apply ONVIF encoder settings per channel.** The channel editor now
  has a **Camera encoder** section backed by
  `GET/PUT /api/v1/channels/{id}/encoder`:
  - Encoding: H.264 or H.265.
  - Resolution width/height, frame rate, bitrate (kbps) and quality.
  - **Read from camera** fills the form from
    `GetVideoEncoderConfigurations`.
  - **Apply to camera** modifies the current configuration and sends
    `SetVideoEncoderConfiguration` with `ForcePersistence=true`.
  - Zero values keep the camera's setting for that field. Applied values are
    stored in the channel's `media.encoder` and returned by the channel API.
- The ONVIF client resolves the media service through `GetCapabilities`
  (`Media` XAddr), falling back to the device service URL when capabilities are
  incomplete.

## Verification

A mock ONVIF media service exposes a 1920×1080 H.264 configuration; the
integration test reads it, applies H.265/1280×720/30 fps/2048 kbps, verifies the
camera-side values and the channel's stored `media.encoder`, and checks that
non-ONVIF channels return `400 unsupported_protocol`.

## Known limitations

- Encoder control is ONVIF-only. Hikvision ISAPI and Dahua CGI encoder setters
  are not implemented yet.
- The camera's ONVIF user needs permission to change encoder configuration.
- Applying a setting interrupts the camera stream briefly while the encoder
  restarts; UvNVR reconnects automatically.

## Upgrade notes

- No database migration or configuration change is required.
- Existing channels keep their current camera settings until **Apply to camera**
  is used.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
