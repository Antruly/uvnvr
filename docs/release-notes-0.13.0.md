# UvNVR 0.13.0

RTSP audio and storage hardening.

## Highlights

- **RTSP audio tracks.** When the source stream carries audio, the RTSP output
  advertises a second track and serves `trackID=1` over TCP interleaved or UDP:
  - AAC in ADTS is repacketized as RFC 3640 AAC-hbr with the AudioSpecificConfig
    in SDP.
  - G.711 PCMA/PCMU is sent as raw 20 ms RTP payloads at 8 kHz.
  - Each track has its own sequence, SSRC, timestamp and RTCP sender reports;
    SETUP handles both tracks and PLAY returns RTP-Info for each.
  - AAC audio is covered by the ffprobe integration test over TCP and UDP.
- **Batched recording writes.** Segments flush at `recording.batch_mb`
  (default 8) or every `recording.batch_ms` (default 500) instead of after each
  chunk. Splits and finalization force a flush, so no data is lost when a
  segment closes.
- **Disk and service alerts.** A volume whose free space cannot be read raises a
  `disk_error` alert (hourly at most). Missing FFmpeg or an RTSP bind failure at
  startup raises `service_error`.
- **Storage browser and guards.** `GET /api/v1/storage/browse?path=` lists
  filesystem roots and subdirectories for the Storage UI. Adding a volume now
  rejects filesystem roots and unwritable directories (verified with a temporary
  file).

## Behavior notes

- Audio is best-effort: AAC and G.711 are supported; other audio codecs are
  ignored, and the video track is unaffected.
- The G.711 path is implemented but not exercised by the MediaMTX integration
  fixture (MediaMTX drops G.711 over RTSP); the AAC path is tested end to end.
- The io_uring/ring-buffer writer remains planned; this release batches writes in
  user space with bounded queues.

## Upgrade notes

- No database migration is required.
- To tune recording I/O, set `recording.batch_mb` (64 KiB–256 MiB) and
  `recording.batch_ms` (10–5000 ms) in the settings table; defaults match the
  previous effective durability within 500 ms.
- Existing RTSP clients that do not send `trackID=1` continue to receive video
  only.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
