# UvNVR 0.15.0

Brand encoder control.

## Highlights

- **Hikvision ISAPI encoder read/apply.** `GET/PUT
  /api/v1/channels/{id}/encoder` reads `/ISAPI/Streaming/channels/101` and
  applies:
  - `videoCodecType` (H.264/H.265)
  - `videoResolutionWidth` / `videoResolutionHeight`
  - `videoFrameRate` / `maxFrameRate` (ISAPI uses hundredths of fps)
  - `vbrUpperCap` (falling back to `videoBitrate`)
- **Dahua CGI encoder read/apply.** Reads
  `configManager.cgi?action=getConfig&name=Encode` and applies
  `Encode[0].MainFormat[0].Video.Compression`, `Width`, `Height`, `FPS` and
  `BitRate` through `setConfig`.
- The endpoint now selects the backend from the channel protocol (or detected
  brand): `onvif`, `hikvision` or `dahua`. Responses normalize `H.264`/`H.265` to
  `H264`/`H265` so the UI and stored `media.encoder` stay consistent.
- The same **Camera encoder** UI works for all three protocols.

## Verification

The brand HTTP integration mock now implements both ISAPI and CGI encoder
endpoints. The test reads defaults, applies H.265/1280×720/30 fps/2048 kbps for a
Hikvision and a Dahua channel, and asserts the camera-side values.

## Known limitations

- Brand encoder quality is not exposed; `quality` is returned as 0 and ignored
  for ISAPI/CGI updates.
- Only the main stream is configured; sub-stream settings are untouched.
- Vendor firmware variants with different XML/CGI field names may need their own
  mapping; the ONVIF, Dahua CGI and Hikvision ISAPI paths above are the tested
  variants.

## Upgrade notes

- No migration or configuration change is required.
- Existing channels keep their camera settings until **Apply to camera** is
  used.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
