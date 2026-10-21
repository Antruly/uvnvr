# UvNVR 0.4.0

Channel organization and personal layouts.

## Highlights

- **Channel groups.** Create a tree of groups, assign channels in the channel
  form, drag channels from the grouped Live preview rail and collapse groups.
  The API supports create/update/delete, ordering, parent changes with cycle
  protection, and channel counts. Deleting a group moves its channels to
  ungrouped and reparents child groups safely.
- **Server-side layouts.** Preview layouts are now stored per user account
  (`GET/POST/PUT/DELETE /api/v1/layouts`) instead of only in browser storage.
  Layouts follow the account across browsers and devices, with one default
  layout, a 20-layout limit and a 64 KiB grid limit. The browser keeps a local
  cache as an offline fallback and syncs changes with a short debounce.

## Upgrade notes

- New SQLite tables (`channel_groups`, `channel_group_members`, `layouts`) are
  created automatically at startup; existing channels and layouts are untouched.
- Channels start ungrouped. Existing browser-only layouts are imported into the
  server on the first change made on the Preview page.
- No configuration changes are required.

## Known limitations

- Group ordering uses the numeric `sort` field; the UI does not yet provide
  drag-and-drop reordering of groups.
- Layouts are private to each user; sharing a layout between accounts is not
  supported yet.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus SHA256SUMS, SPDX SBOM and the FFmpeg source/license assets.

## FFmpeg and patent notice

Official packages invoke an LGPL FFmpeg CLI as a separate process; no FFmpeg
code is linked into UvNVR. This project does not grant any patent license for
H.264, H.265/HEVC, AAC or any other codec.
