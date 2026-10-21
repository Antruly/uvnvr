# UvNVR 0.8.0

Bulk onboarding and recording visibility.

## Highlights

- **CSV bulk import.** On the Channels page, **Import CSV** reads
  `host,username,password,brand,name,httpPort` rows (a header row is optional).
  Every row is probed through ONVIF, Hikvision ISAPI or Dahua CGI; successful
  cameras are added with their detected brand, name, streams and HTTP port.
  Duplicate hosts are skipped, the button shows progress, and failures are
  reported per row.
- **Live recording status.** The Storage page has an **Active recordings** panel
  refreshed every five seconds. It shows each recorder's channel, stream,
  current file, bytes written, start time and the volume used for new segments.
  The same data is available at `GET /api/v1/recordings/status`.

## Upgrade notes

- No database migration or configuration change is required.
- CSV import runs in the browser and uses the same detection endpoint as the
  channel form; credentials are sent only to the local recorder over the
  authenticated session.
- Rows without a brand try ONVIF first, then the brand HTTP APIs. A camera whose
  HTTP service is not on port 80 needs the optional `httpPort` column.

## Known limitations

- CSV import is sequential; large lists take one detection round per camera.
- Unrecognized brands without ONVIF still need manual RTSP configuration.
- The recording status panel reports plan-driven recorders; event-triggered
  clips are not yet produced.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
