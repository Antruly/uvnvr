# UvNVR 0.11.0

API contract and standard RTSP.

## Highlights

- **OpenAPI 3.1 + Swagger UI.** `GET /api/v1/openapi.json` returns the spec
  (permission `api.access`). `GET /api/docs` serves a self-hosted Swagger UI;
  its CSS/JS are installed by the frontend build into `/api-docs/`, so no CDN or
  external script is loaded. Cookie sessions and bearer tokens are both
  described, and mutating cookie requests document the `X-CSRF-Token` header.
- **H.264/H.265 RTP output.** `/live/{channel}/{main|sub}` now sends standard
  RTP payload type 96 instead of RTP/MP2T:
  - H.264 follows RFC 6184 with single-NAL and FU-A packetization.
  - H.265 follows RFC 7798 with single-NAL and FU packetization.
  - SDP carries `packetization-mode=1` plus `sprop-parameter-sets` (H.264) or
    `sprop-vps/sps/pps` (H.265).
  - RTCP sender reports are sent every five seconds; TCP interleaved and UDP
    unicast both work.
  - `DESCRIBE` waits up to two seconds for the stream's PAT/PMT and parameter
    sets, returning `503` while the channel is offline.
- **Snapshot sizing.** `GET /api/v1/channels/{id}/snapshot` accepts `width`
  (0 or 16–4096), `height` (0 or 16–4096) and `quality` (1–31, default 2);
  missing dimensions preserve the aspect ratio.

## Behavior changes

- **RTP/MP2T is gone.** Clients that only understood payload type 33 must use a
  client with H.264/H.265 RTP support (VLC, ffplay, GStreamer, ffprobe).
- **Audio is not forwarded over RTSP in this release.** The live preview,
  recordings and downloads still carry the camera's audio track.
- **RTSP authentication remains Basic.** Digest is planned separately.

## Verification

The integration test publishes H.264 and H.265 test streams through MediaMTX and
uses `ffprobe` against the UvNVR output over both TCP and UDP. It asserts
`codec_name=h264`/`hevc`, RTP packetization, and the expected frame size.

## Upgrade notes

- No database migration or configuration change is required.
- If a third-party integration depends on the MP2T payload, keep using 0.10.x
  until that integration supports standard RTP.
- `npm ci && npm run build` now copies the Swagger UI assets into the frontend
  bundle; packaged builds include them automatically.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
