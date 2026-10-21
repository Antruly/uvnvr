# UvNVR 0.10.0

Operational safeguards.

## Highlights

- **Maintenance mode is now functional.** Starting with `--maintenance` (or
  `"maintenance": true` in `uvnvr.json`) exposes administrator-only destructive
  actions, each requiring an explicit confirmation and written to the audit log:
  - delete a recording segment from Playback;
  - purge audit entries from Logs (or export them first);
  - change the serial number and the display version;
  - format a storage volume.
  Outside maintenance mode these endpoints return `403 maintenance_required`;
  non-admin accounts get `403 admin_required`.
- **Configurable motion detection.** **System → Motion detection** (and the
  `motion` group in `GET/PUT /api/v1/settings`) controls the enable switch,
  sensitivity (1–100), cooldown (10–3600 s) and a percentage region of the
  64×36 sampled frame. Sensitivity 50 preserves the previous threshold. The
  performance tier now caps simultaneous sampling (low 2, medium 4, high 8).
- **Audit CSV export.** `GET /api/v1/audit/export` downloads the filtered audit
  log as UTF-8 CSV (BOM included for Excel, 100,000-row limit), and the Logs
  page has an **Export CSV** button.
- **Windows snapshot and motion capture.** The Windows build now runs FFmpeg
  through `CreateProcess` pipes for JPEG snapshots and grayscale motion frames.
  The previous “not wired on Windows yet” paths are gone.
- **GPU capture with fallback.** With GPU acceleration enabled, snapshots and
  motion samples try `-hwaccel auto` first and retry in software if the driver
  or pixel format cannot produce the frame.

## Behavior notes

- **Volume format scope.** Formatting removes UvNVR's `<volume>/uvnvr` recording
  tree and the volume's index rows. It does not run `mkfs` and does not touch
  unrelated files on the disk. The confirmation must repeat the volume path.
- **Display version.** The override changes the UI and `/api/v1/system/overview`
  only; `/healthz` continues to report the build version and includes the
  `maintenance` flag.
- **Motion region.** One rectangular region is supported, not polygon masks or
  object tracking. Sampling remains a low-resolution frame-difference detector,
  not AI.
- **GPU scope.** The GPU switch affects snapshot and motion sampling only;
  preview and recording are stream-copied and do not use the GPU.

## Fixed

- The dashboard reported channel and recording counts as zero. Both counts now
  come from the database.

## Upgrade notes

- No database migration is required. Existing motion settings are created with
  the defaults (enabled, sensitivity 50, 60-second cooldown, full-frame region).
- Maintenance endpoints disappear from normal-mode deployments automatically;
  set `maintenance: true` only for an explicit operations session.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
