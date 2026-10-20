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
| POST/PUT/DELETE | `/api/v1/channels[/{id}]` | channel.manage |
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
| GET | `/api/v1/recordings` | playback.view |
| GET | `/api/v1/recordings/{id}/download` | playback.download |
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

## Errors

Errors use `{ "error": "stable_code", "message": "human-readable text" }`. HTTP status codes are meaningful; 429 includes `Retry-After` for login lockouts.
