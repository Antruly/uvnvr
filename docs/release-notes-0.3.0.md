# UvNVR 0.3.0

Playback and download hardening.

## Highlights

- **Resumable downloads.** `GET /api/v1/recordings/{id}/download` supports HTTP
  Range (`206 Partial Content`), suffix and open ranges, and `416` for
  unsatisfiable ranges. Files are streamed from disk instead of being fully
  buffered in memory, so large segments no longer hit the old 512 MiB / 413
  limit.
- **MP4 export.** `POST /api/v1/playback/export` concatenates the selected
  segments and remuxes the time range to a fast-start MP4 with the bundled
  FFmpeg (`-c copy`, no transcoding). The Playback page has an **Export MP4**
  button. Exports are limited to six hours / 4 GiB and temporary files are
  deleted after the response.
- **Shared playback cache.** A 64 KiB-block LRU cache is shared by all playback
  sessions, with next-segment read-ahead. Configure it with
  `storage.cacheMb` (default 128) and `storage.readaheadMb` (default 4) or the
  `UVNVR_CACHE_MB` / `UVNVR_READAHEAD_MB` environment variables. Hit, miss and
  usage statistics appear in the System page and `/api/v1/system/overview`.

## Upgrade notes

- No database migration or configuration change is required. Existing
  `uvnvr.json` files keep their values; the new `storage` block is optional.
- The Windows installer and all packages are rebuilt at 0.3.0.

## Known limitations

- MP4 export is a remux. If the source is not MP4-compatible (for example,
  some H.265 profiles), FFmpeg may fail; the API returns a descriptive error.
- Reverse playback remains seek-based; browsers cannot decode a reversed GOP.
- Cache memory is per process and not written to disk.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus SHA256SUMS, SPDX SBOM and the FFmpeg source/license assets.

## FFmpeg and patent notice

Official packages invoke an LGPL FFmpeg CLI as a separate process; no FFmpeg
code is linked into UvNVR. This project does not grant any patent license for
H.264, H.265/HEVC, AAC or any other codec.
