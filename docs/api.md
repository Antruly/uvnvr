# UvNVR API (0.1.0)

All endpoints are under `/api/v1` unless noted.

## Authentication

- Web UI: session cookie (`uvnvr_session`, HttpOnly, SameSite=Strict) plus `X-CSRF-Token` on state-changing requests.
- API: `Authorization: Bearer <token>` created in **System → API Token**.
- RTSP: `rtsp://user:pass@host:554/live/{channel}/{main|sub}` using RTSP Basic authentication.
- A short-lived WebSocket ticket is required for live/playback streams.

## Core endpoints

| Method | Path | Permission |
|---|---|---|
| GET | `/healthz` | public |
| GET | `/api/v1/setup/status` | public |
| POST | `/api/v1/setup` | first run |
| POST | `/api/v1/auth/login` | public |
| GET | `/api/v1/auth/me` | session |
| POST | `/api/v1/auth/logout` | session |
| GET | `/api/v1/dashboard/summary` | dashboard.view |
| GET | `/api/v1/system/info` | public (minimal) |
| GET | `/api/v1/system/overview` | system.view |
| GET | `/api/v1/system/config` | system.view |
| GET/PUT | `/api/v1/settings` | system.view/manage |
| GET | `/api/v1/system/time` | system.view |
| POST | `/api/v1/system/time/sync` | system.manage |
| POST | `/api/v1/system/ntp/sync` | system.manage |
| POST | `/api/v1/system/email/test` | system.manage |
| GET | `/api/v1/channels` | channel.view |
| GET | `/api/v1/preview/channels` | preview.view |
| POST | `/api/v1/channels/detect` | channel.manage |
| POST | `/api/v1/channels` | channel.create |
| PUT | `/api/v1/channels/{id}` | channel.manage |
| DELETE | `/api/v1/channels/{id}` | channel.delete |
| GET | `/api/v1/channel-groups` | session |
| POST | `/api/v1/channel-groups` | channel.manage |
| PUT/DELETE | `/api/v1/channel-groups/{id}` | channel.manage |
| GET/POST | `/api/v1/layouts` | preview.view |
| PUT/DELETE | `/api/v1/layouts/{id}` | preview.view |
| POST | `/api/v1/channels/discover` | channel.manage |
| POST | `/api/v1/channels/onvif/probe` | channel.manage |
| POST | `/api/v1/channels/{id}/reconnect` | channel.manage |
| POST | `/api/v1/channels/sync` | channel.manage |
| GET | `/api/v1/channels/{id}/snapshot` | api.snapshot |
| POST | `/api/v1/streams/ticket` | preview.view |
| WS | `/ws/live?ticket=...` | preview.view |
| GET | `/api/v1/plans` | plan.view |
| PUT | `/api/v1/plans` | plan.manage |
| POST | `/api/v1/plans/sync` | plan.manage |
| GET | `/api/v1/storage/volumes` | storage.view |
| POST/PUT/DELETE | `/api/v1/storage/volumes[/{id}]` | storage.manage |
| GET | `/api/v1/recordings/status` | storage.view |
| GET | `/api/v1/recordings` | playback.view |
| GET | `/api/v1/recordings/{id}/download` | playback.download |
| POST | `/api/v1/playback/export` | playback.download |
| POST | `/api/v1/playback/ticket` | playback.view |
| WS | `/ws/playback?ticket=...` | playback.view |
| GET | `/api/v1/alerts` | alert.view |
| POST | `/api/v1/alerts/{id}/ack` | alert.manage |
| GET | `/api/v1/alerts/{id}/snapshot` | alert.view |
| GET | `/api/v1/users` | user.view |
| POST/PUT/DELETE | `/api/v1/users[/{id}]` | user.manage |
| GET | `/api/v1/roles` | user.view |
| POST/PUT/DELETE | `/api/v1/roles[/{id}]` | user.manage |
| GET | `/api/v1/permissions` | user.view |
| GET/POST/DELETE | `/api/v1/tokens[/{id}]` | api.access |
| GET | `/api/v1/audit` | log.view |

## RTSP output

```
rtsp://user:password@host:554/live/1/main
rtsp://user:password@host:554/live/1/sub
```

RTP payload is MPEG-TS (payload type 33) over TCP interleaved or UDP unicast. If port 554 cannot be bound, the service tries 8554/8555 and prints the active port.

## Streaming tickets

`POST /api/v1/streams/ticket` with `{ "channelId": 1, "stream": "main" }` returns a `wsPath` and `rtspPath`/`rtspPort`. Tickets expire quickly and are single-use.

`POST /api/v1/playback/ticket` with
`{ "channelId": 1, "stream": "main", "startMs": 0, "endMs": 0, "speed": 1 }`
returns a playback `wsPath`. `speed` accepts 0.25–8.0 (default 1.0); the server
paces the MPEG-TS stream by the segment bitrate, and the web player additionally
sets the media element's playback rate. Seeking (fast-forward/rewind buttons,
timeline clicks) requests a new ticket starting at the target time; the server
starts at the nearest keyframe.

## Downloads and export

`GET /api/v1/recordings` returns `path`, `codec`, `startMs`, `endMs`,
`durationMs`, `sizeBytes` and `recordType` for each segment. A codec change starts
a new segment; playback sessions stop at that boundary so one player never
receives mixed H.264/H.265 data. Seek to the next segment to switch codecs.

`GET /api/v1/recordings/{id}/download` streams the segment without buffering
the whole file in memory and supports HTTP Range:

```
Range: bytes=0-1048575
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-1048575/52428800
Accept-Ranges: bytes
```

A suffix range (`bytes=-65536`) and an open range (`bytes=1048576-`) are
accepted. Unsatisfiable ranges return 416 with `Content-Range: bytes */<size>`.

`POST /api/v1/playback/export` with
`{ "channelId": 1, "stream": "main", "startMs": 0, "endMs": 0 }` concatenates
the segments in the range and remuxes them to a fast-start MP4 with the bundled
FFmpeg. It returns `video/mp4` as an attachment. The range is limited to six
hours and 4 GiB of input, and temporary files are removed after the response.
`503 ffmpeg_unavailable` is returned when no FFmpeg executable is configured.

`GET /api/v1/preview/channels` returns the display metadata and stream state
needed by preview-only roles (no usernames, URLs or stored error text) and
requires only `preview.view`.

## Recording status

`GET /api/v1/recordings/status` returns a live snapshot of every plan-driven
recorder and the volume new segments will use:

```json
{
  "recordings": [
    {
      "channelId": 1,
      "stream": "main",
      "recordType": "event",
      "active": true,
      "path": "/var/lib/uvnvr/recordings/uvnvr/1/2026-10-21/1_main_...ts",
      "bytes": 1048576,
      "startMs": 1792564800000,
      "volumeId": 1,
      "volumePath": "/var/lib/uvnvr/recordings",
      "untilMs": 1792564860000
    }
  ],
  "volume": { "id": 1, "path": "/var/lib/uvnvr/recordings", "mode": "loop" }
}
```

`active` is true while a segment file is open; `path` and `startMs` are empty or
zero for idle recorders, and `bytes` counts data written to the current
segment. `recordType` is `timed` for scheduled plans and `event` for
alert-triggered clips; `untilMs` is the event deadline (zero for scheduled
recorders). `volume` is the volume selected for the next segment (`{}` when none
is available). The endpoint reads in-memory recorder state and does not scan the
file system, so it is safe to poll; the Storage page refreshes it every five
seconds. A channel can have a scheduled and an event recorder in the list at the
same time when both plans are active.

## API tokens and scopes

Create tokens with `POST /api/v1/tokens`:

```json
{
  "name": "wall display",
  "expiresInDays": 365,
  "scopes": ["preview", "channels.read"]
}
```

`scopes` is optional; an empty/omitted list inherits the account role. Scopes
are intersected with the role, so a token can never grant more than its owner.
Scopes are returned by `GET /api/v1/tokens`.

| Scope | Grants |
|---|---|
| `all` | The account role's full permission set |
| `preview` | Live streams |
| `snapshots` | JPEG snapshots |
| `channels.read` | Query channels |
| `channels.create` | Add channels |
| `channels.delete` | Delete channels |
| `channels.manage` | Edit channels, ONVIF discovery (implies read/create/delete) |
| `playback` | Query recordings and playback |
| `download` | Download/export video |
| `alerts` | Query/acknowledge alerts |
| `plans` | Recording plans |
| `storage` | Storage volumes |
| `time` | Read/set recorder time and sync cameras |
| `system` | System information |
| `rtsp` | RTSP output |
| `audit` | Audit log |
| `users` | Users and roles |

## Time synchronization

`POST /api/v1/system/time/sync` with `{ "epochMs": ... }` sets the recorder
clock and then attempts to set the clock on every enabled ONVIF-capable camera
(ONVIF channels, plus Hikvision/Dahua devices when ONVIF is enabled). It returns:

```json
{
  "ok": true,
  "serverMs": 0,
  "serverTimeApplied": true,
  "applyError": "",
  "cameras": { "synced": 1, "failed": 0, "skipped": 0, "cameras": [] }
}
```

`POST /api/v1/system/ntp/sync` queries the configured NTP server, applies the
offset and performs the same camera synchronization; it returns `offsetMs`,
`applied`, `applyError` and `cameras`. Pure RTSP cameras have no control channel
and are reported as skipped. For Hikvision/Dahua channels (or ONVIF channels whose
manufacturer is Hikvision/Dahua) the vendor ISAPI/CGI API is used as a fallback.

## Brand detection

`POST /api/v1/channels/detect` with
`{ "host": "192.168.1.64", "httpPort": 80, "username": "admin",
"password": "...", "brand": "hikvision" }` tries the camera's ONVIF device
service, then Hikvision ISAPI or Dahua CGI over HTTP Digest. Supplying `brand`
tries that vendor API first. The response contains `protocol`, `name`, `model`,
`brand`, `mainUrl`, `subUrl`, `subEnabled`, `eventsEnabled` and `httpPort`.
Channels store `httpPort` (default 80) separately from the RTSP port and return
it in the channel JSON; it is used for ONVIF/brand HTTP and time sync.

## ONVIF events

ONVIF channels accept `eventsEnabled` on create/update and return it in the
channel JSON. When enabled, UvNVR resolves the camera's event service through
`GetCapabilities` (falling back to `http://host:port/onvif/events`), creates a
PullPoint subscription and polls notifications every few seconds.

Recognized topics become alerts (`type`):

| Topic fragment | Alert type |
|---|---|
| `CellMotionDetector`, `Motion` | `motion` |
| `PeopleDetector`, `HumanDetector`, `ObjectDetector` | `person` |
| `TamperDetector` | `tamper` |
| `LineDetector`, `FieldDetector` | `intrusion` |

Notifications with `IsMotion`/state values of false are ignored. Each type has a
60-second per-channel cooldown. Motion alerts (including the experimental
frame-difference detector) and the four ONVIF types above also start an event
clip when the channel has an active `event`/`both` plan. Subscriptions are
renewed before the camera's termination time, and the poller reconnects with
backoff when the camera is
unreachable.

## Channel groups

`GET /api/v1/channel-groups` returns the flat group list with channel counts:

```json
[{ "id": 1, "name": "Building A", "parentId": 0, "sort": 0, "channels": 3 }]
```

`POST` accepts `{ "name": "...", "parentId": 0, "sort": 0 }`; `PUT /{id}`
updates the name, parent or order. Cycles and self-parenting are rejected with
400. Deleting a group moves its channels to ungrouped and reparents its child
groups to its own parent. Channels accept `groupId` on create/update and return
it in the channel JSON.

## Preview layouts

Layouts are stored per user and survive browser changes:

```json
{
  "name": "Desk",
  "grid": { "grid": "4", "cols": 2, "rows": 2, "cells": [1, null, 2, null] },
  "isDefault": true
}
```

`GET /api/v1/layouts` returns the current user's layouts, default first.
`POST` creates (up to 20 per user, 64 KiB grid limit); `PUT /{id}` updates;
`DELETE /{id}` removes. Exactly one layout per user is the default.

## Startup configuration

`GET /api/v1/system/config` returns the effective startup settings without
secrets:

```json
{
  "http": { "port": 8080, "host": "0.0.0.0", "tls": { "cert": "", "key": "", "selfSigned": false } },
  "rtsp": { "port": 554 },
  "dataDir": "/var/lib/uvnvr",
  "maintenance": false,
  "ffmpeg": { "path": "" },
  "logging": { "level": "info" },
  "config": { "path": "/etc/uvnvr/uvnvr.json", "loaded": true }
}
```

Ports and the data directory are applied at startup. See
[`configuration.md`](configuration.md) for the file format and precedence.

## Runtime settings

`GET /api/v1/settings` returns the editable groups (`security`, `audit`,
`event`, `system`, `smtp`); `PUT` accepts any subset of them. Event recording
uses:

```json
{
  "event": {
    "enabled": true,
    "preSeconds": 10,
    "postSeconds": 30,
    "maxSeconds": 300
  }
}
```

`preSeconds` is 0–60, `postSeconds` and `maxSeconds` are 1–3600 and
`postSeconds` cannot exceed `maxSeconds`; invalid values return 400 and nothing
is written. When the global switch is off, alerts still create alert records but
no event clips. Per-channel event plans (`mode` `event` or `both`) decide which
channels and streams react to alerts.

## Errors

Errors use `{ "error": "stable_code", "message": "human-readable text" }`. HTTP status codes are meaningful; 429 includes `Retry-After` for login lockouts.
