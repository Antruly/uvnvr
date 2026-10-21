# UvNVR 0.12.0

RTSP Digest and live events.

## Highlights

- **RTSP Digest authentication.** The RTSP server now advertises Digest and
  Basic in its 401 challenge. Digest uses MD5 with `qop=auth`, a per-process
  nonce and the stored HA1 (`MD5(username:UvNVR:password)`); Basic keeps using
  the scrypt password hash. Both require the `rtsp.access` permission.
  - HA1 is written when the administrator is created, when users are created or
    their password changes, and on the next successful login for upgraded
    installations. No password reset is needed after upgrading.
  - The database adds `users.rtsp_ha1` automatically.
- **Live events socket.** `POST /api/v1/events/ticket` issues a single-use
  ticket for `/ws/events`. The socket pushes:
  - `hello` with the server version;
  - `alert` messages as soon as an alert row is written (motion, ONVIF, channel
    offline, storage, …);
  - `channel` messages when a channel changes online/offline state.
- **UI integration.** The web UI requests a ticket after login, reconnects with
  backoff, shows a toast for new alerts that links to the Alerts page, and
  refreshes the Alerts list in place. The Alerts page is now part of the primary
  navigation instead of a disabled "planned" item.
- The OpenAPI document includes the events-ticket endpoint.

## Upgrade notes

- No manual migration is required. Existing accounts get `rtsp_ha1` on their
  next web login; until then their RTSP access uses Basic (or Digest fails and
  the client falls back to Basic if it supports both).
- Event tickets are short-lived (30 seconds) and single-use. Reconnect with a
  fresh ticket; the UI does this automatically.

## Known limitations

- Digest is MD5/`qop=auth` only (RFC 7616 does not define a stronger required
  digest for RTSP clients).
- RTSP output still forwards video only; audio packetization remains planned.
- Real-time events are pushed while the browser session is open; missed events
  are still available through `GET /api/v1/alerts`.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
