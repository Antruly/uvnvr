# UvNVR 0.16.0

io_uring-backed recording I/O.

## Highlights

- **Segment writes use libuv file requests.** Each recorder owns a private
  `uv_loop` and performs `uv_fs_open`, `uv_fs_write`, `uv_fs_fsync`,
  `uv_fs_close`, `uv_fs_rename` and `uv_fs_unlink` on it. The web loop never
  blocks on disk I/O.
- **io_uring on Linux.** libuv 1.48 hands filesystem requests to io_uring when
  the kernel supports it. The recording integration test was traced with
  `strace -e io_uring_setup,io_uring_enter`; both syscalls appear while segments
  are written. Systems without io_uring fall back to libuv's threadpool.
- **Explicit libuv link in CMake.** libuvcpp links libuv privately, so UvNVR now
  links the libuv library (pkg-config when available, `find_library` otherwise).
- `recording.batch_mb` / `recording.batch_ms` now control the `fsync` cadence
  (`uv_fs_fsync`). Writes themselves complete asynchronously per chunk.

## Notes

- No database, API or configuration changes.
- The Windows console already switches to UTF-8 at the start of `main`
  (`SetConsoleOutputCP(CP_UTF8)`), so Chinese help and logs are not mojibake.

## Verification

The recording integration test produces and indexes a segment with the new
writer. A traced run confirmed `io_uring_setup` and more than a thousand
`io_uring_enter` calls.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
