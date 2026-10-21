# UvNVR User Guide (0.9.0)

This guide walks through a complete installation: first-run setup, adding cameras,
previewing them, recording, playback, API/RTSP access, alerts and administration.

A Chinese version is available at [`user-guide.zh-CN.md`](user-guide.zh-CN.md).

## 1. Install and start

| Platform | Package | Default data directory |
|---|---|---|
| Linux | `.deb`, `.rpm`, `.tar.gz` | `/var/lib/uvnvr` |
| Windows | NSIS setup or portable `.zip` | `%ProgramData%\UvNVR` (under Program Files); `.\data` (portable elsewhere) |
| macOS | `.pkg`, `.dmg`, `.zip` | `/Library/Application Support/UvNVR` |

Linux packages install a systemd unit and start it automatically. Open
`http://<host>:8080` in a browser. The service listens on:

| Port | Purpose |
|---|---|
| 8080 | Web UI and REST API |
| 8443 | Optional HTTPS |
| 554 | RTSP output (falls back to 8554/8555 when the port cannot be bound) |

To run manually or with a custom data directory:

```bash
uvnvr --port 8080 --rtsp-port 554 --data-dir /var/lib/uvnvr
```

Ports and the data directory come from a JSON file and can be changed without
rebuilding or reinstalling:

- Windows installer: choose the HTTP and RTSP ports on the **Service ports**
  page. The installer writes `%ProgramData%\UvNVR\uvnvr.json` and adds a
  **UvNVR Configuration** Start-menu shortcut.
- Linux: edit `/etc/uvnvr/uvnvr.json`, then `sudo systemctl restart uvnvr`.
- macOS: edit `/Library/Application Support/UvNVR/uvnvr.json`, then restart the
  launchd job.
- Any build: pass `--config <path>` or `--port`, `--rtsp-port`, `--data-dir`.

Command-line flags override environment variables, which override the file.
`uvnvr --print-config` shows the effective settings. See
[`configuration.md`](configuration.md) for the full schema.

```json
{
  "http": { "port": 8080, "host": "0.0.0.0" },
  "rtsp": { "port": 554 },
  "dataDir": "/var/lib/uvnvr"
}
```

Useful options are `--version`, `--help`, `--print-config`, `--tls-self-signed`
and `--maintenance`. The top bar and login page switch between **Auto** (follow
the operating system), **Light** and **Dark** themes; the choice is remembered.
If the data directory cannot be created, pass a writable location explicitly:

```bat
uvnvr.exe --data-dir "%ProgramData%\UvNVR"
```

See [`deployment.md`](deployment.md) for service users, firewall and backup notes.

## 2. First-run setup

1. The first browser visit redirects to the setup page.
2. Choose the `admin` password and confirm it. The password is stored with scrypt
   and is never logged.
3. Set the time zone. It defaults to the browser's zone.
4. Sign in as `admin`.

Until setup is complete every other API returns a setup-required error. The admin
password can be changed later from the user page; the setup endpoint cannot be
replayed after it succeeds.

## 3. Add cameras

Open **Channels**. The list shows state, protocol, stream URLs, last error and the
last online/offline times.

### ONVIF discovery

1. Click **Scan ONVIF**. Enter the camera user name and password used by the probe.
2. Select a discovered device. UvNVR calls `GetDeviceInformation`, `GetProfiles` and
   `GetStreamUri`, and shows the manufacturer, model, firmware and profiles.
3. Click **Add** to create a channel from the selected profile.

0.1.0 probes `http://` ONVIF device services only. If discovery is blocked by the
network, add the channel manually with the protocol **ONVIF** and the device host.

### Manual RTSP

1. Click **Add channel**.
2. Choose a protocol:
   - **RTSP**: paste the full main stream URL, for example
     `rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101`.
   - **Hikvision**: fill host, port and credentials; the
     `Streaming/Channels/101|102` paths are generated.
   - **Dahua**: the `cam/realmonitor?channel=1&subtype=0|1` paths are generated.
   - **Generic**: a configurable URL template with the same placeholders.
3. Optionally enable the sub stream and enter its URL. Multi-cell preview should
   use the sub stream to keep CPU and bandwidth low.
4. Optionally set a watermark text and position. The watermark is drawn in the
   browser over preview and snapshots.
5. For ONVIF cameras, enable **Use camera ONVIF events**. UvNVR subscribes to the
   camera's PullPoint service and turns motion, person, tamper and line-crossing
   notifications into alerts.
6. Save. The channel begins connecting immediately.

Use **Auto detect** in the channel form to probe the entered host. It tries
ONVIF first, then Hikvision ISAPI or Dahua CGI over HTTP Digest; choose the brand
in **Protocol** to try that vendor first. It fills the protocol, stream paths and
name automatically. Set **HTTP / ONVIF port** if the camera's HTTP service is not
on 80. From **Scan
ONVIF**, **Add now** creates the channel in one click. Hikvision and Dahua
manufacturers are recognized and use their generated main/sub stream templates,
so no RTSP URL is needed; other ONVIF cameras use the profile URI, and manual
RTSP remains available. Channel rows show the numeric `#id` and a snapshot
thumbnail; the camera button refreshes it and double-clicking the thumbnail
opens a large view. **Reconnect** retries immediately. **Sync** copies selected channel fields and/or recording plans to other
channels after showing a diff.

Use **Groups** to organize channels into a tree. Create a group, optionally
under a parent, and pick it in the channel form. Channels without a group appear
under **Ungrouped**. Deleting a group moves its channels to ungrouped and
reparents its child groups; the preview tree and channel list update
immediately.

To onboard many cameras at once, click **Import CSV** and select a file with
`host,username,password,brand,name,httpPort` rows. A header row is optional; it
is detected from a `host` or `ip` column, and `user`, `pass` and `http_port` are
accepted as aliases. Set `brand` to `hikvision` or `dahua` to try that vendor
first, or leave it empty to try ONVIF first. `name` overrides the detected name.
Every row uses the same detection path as **Auto detect** and is then created as
a channel; the button shows `done/total` while the import runs. A host that
already has a channel is skipped, and failures are listed with the row's host.
Import is sequential in the browser, so a long list takes one detection round
per camera.

### Channel states

`disabled → connecting → online → reconnecting → offline/error`

The ingest thread reconnects with jittered backoff (1s, 2s, 5s, 10s, then 30s).
A watchdog also detects a connection that stays open but stops delivering data.
The channel page shows the last error, so test the RTSP URL with VLC or `ffplay`
if a camera will not come online.

## 4. Live preview

Open **Preview**. It is the second page after the dashboard.

1. Choose a layout: 1, 4, 8, 16 or 32 cells, or set custom rows/columns
   (1–8 each).
2. Drag a channel from the left tree into a cell, or click a channel and then an
   empty cell. Use **Clear** to empty a cell.
3. Double-click an online picture for fullscreen. An offline cell shows the
   channel name and an offline badge; double-click it to open channel management.
4. Choose the main or sub stream for the whole layout.
5. Name and save layouts. Layouts are stored per user account on the server, so
   they follow the account across browsers; the browser keeps a local cache for
   offline use. Use the layout tabs to switch, and the **×** tab to delete.
6. **Play all** fills the active layout and starts every cell; **Stop all**
   unmounts the players without losing the layout. When there are more channels
   than cells, the page arrows show the next/previous page. **Recordings** opens
   Playback for the selected channel.

Every consumer shares one camera connection. Live clients have independent
bounded queues: a slow client drops to the next keyframe or is disconnected and
does not affect other viewers or recording.

## 5. Recording plans

Open **Plans**.

1. Select a channel and the recording stream (main or sub).
2. Pick a brush: **Timed**, **Event**, **Timed + event**, or **Erase**.
3. Drag a rectangle across the 7 × 48 grid (one cell per half hour) to paint
   many days/cells at once; the erase brush clears a rectangle. A plan painted to
   midnight is stored with an end of 24:00.
4. Save. Use **Sync** to copy the plan to other channels.

Recording is plan-driven: a channel writes only while the current weekday and
half-hour slot is painted. Without a storage volume and an enabled plan nothing
is recorded.

**Event** and **Timed + event** cells also record on demand. When motion is
detected or the camera sends an ONVIF person, tamper or line-crossing event,
UvNVR starts a clip from the buffered pre-roll and keeps recording for the
post-roll. Follow-up alerts extend the clip up to the maximum length, and the
clip is finalized after the post-roll even if the stream goes quiet. Configure
the toggle and durations under **System → Event recording** (defaults: enabled,
10 s pre-roll, 30 s post-roll, 300 s maximum), or through the `event` group in
`GET/PUT /api/v1/settings`. Event segments are indexed with `record_type=event`
and play back like scheduled segments.

Recording defaults can be changed in **System** settings:

- `recording.segment_minutes` (default 5): time-based slicing.
- `recording.segment_size_mb` (default 256): size-based slicing.
- Slices end at the first keyframe after the threshold.

Segments are MPEG-TS, immutable and indexed in SQLite. A crash during a segment
leaves a `.part` file; the recorder repairs or reindexes it at startup.

## 6. Storage

Open **Storage** to manage recording volumes.

- Add a path that exists and is writable. Each volume can use:
  - **Loop**: delete the oldest segments when the volume reaches its limit.
  - **Alert**: stop recording and raise an alert at the limit.
- Configure the maximum usage percentage (default 90), reserved space and
  retention days (default 30; **0 means keep forever**). Expired segments are
  deleted automatically.
- Volumes can be disabled without deleting existing files.

The **Active recordings** panel below the volume list refreshes every five
seconds. It shows the volume new segments are written to and, for each plan
recorder, whether it is recording, whether the clip is **Scheduled** or
**Event**, the current file, bytes written and the segment start time. Idle
recorders show **Idle** with no current file. This
panel reflects the in-memory recorder state, so it confirms that data is
actually being written without scanning the disk.

Manual deletion of video files is not available outside maintenance mode.
Automatic retention and loop cleanup are policy operations and are audited.

## 7. Playback and download

Open **Playback**.

1. Choose the channel, stream and date.
2. The timeline shows recorded intervals; click a segment or a time to play.
3. Playback streams MPEG-TS over a WebSocket and crosses segment boundaries
   without reloading the player.
4. Seeking starts a new ticket at the target time, so only the required segment
   and a small read-ahead window are read from disk. Click a recorded interval
   on the timeline to jump to that time.
5. The player toolbar has proportional skip buttons and **0.25×–32×** speed
   control. The timeline zooms from 24 hours down to 5 minutes; the skip step is
   the visible window divided by 24 (24h → 1h, 1h → 2.5m). **Reverse** steps
   backward at the selected rate. The server paces the stream to the selected rate;
   fast-forward is forward playback at a higher rate, and rewind is repeated
   backward seeks (browsers cannot decode a reversed GOP).
6. **Download** fetches a single MPEG-TS segment with HTTP Range support, so
   interrupted transfers can resume. **Export MP4** remuxes the selected segment
   to a fast-start MP4; the **From/To** fields download an MP4 for an arbitrary
   time range. No transcoding is performed. A codec switch starts a new segment;
   playback stops at that boundary, so seek to the next segment to change codec.
7. Playback and downloads share a 64 KiB-block LRU cache (128 MB by default) and
   preload the next segment while the current one plays. Cache hits, misses and
   memory use are shown in **System settings**; tune them with
   `storage.cacheMb` and `storage.readaheadMb`.

## 8. Snapshots, API and RTSP output

### Snapshots

`GET /api/v1/channels/{id}/snapshot` returns JPEG. Optional query parameters set
width, height and quality. Decoding uses the hardware path when available and
falls back to CPU.

### REST API

1. Create a token in **System → API Token**. The token is shown once; only its
   hash is stored.
2. Send `Authorization: Bearer <token>`.
3. Choose **API scopes** when creating a token: preview, snapshots, channel
   query/add/delete/manage, playback/download, alerts, plans, storage, time,
   system, RTSP, audit and users. Group checkboxes and the **All scopes** box
   select children in one click; an empty selection inherits the account role.
   Scopes are intersected with the role.
4. Endpoints are under `/api/v1`; see [`api.md`](api.md) for the full list.

API authentication failures count toward the independent API lockout policy.

### RTSP output

```
rtsp://user:pass@host:554/live/{channel}/{main|sub}
rtsp://user:pass@host:554/live/{channel}        # main stream
```

RTSP uses Basic authentication and RTP/MP2T (payload type 33) over TCP
interleaved or UDP unicast. The `rtsp.access` permission is required. If the sub
stream is disabled, requesting it returns an explicit error.

### Live WebSocket

`POST /api/v1/streams/ticket` returns a short-lived single-use ticket and a
`/ws/live?ticket=...` path. Keeping credentials out of the URL avoids leaking
camera passwords into browser history and proxy logs.

## 9. Alerts and email

Open **Alerts**. The list can be filtered by level and acknowledgement state.
Each alert shows its snapshot when available; acknowledging is audited.

Alert sources:

- Motion detection on a low-resolution sampled frame. Sensitivity, region and
  cooldown are configurable. This is an experimental frame-difference detector,
  not AI.
- ONVIF camera events (motion, person, tamper, line crossing) when enabled on the
  channel. Each type has a 60-second per-channel cooldown.
- Camera offline and recovery events.
- Storage nearly full, disk errors and service errors.

Configure SMTP under **System → Email alerts**. The default preset is QQ Mail
(`smtp.qq.com:465`); use the account's authorization code, not the login
password. **Test** sends a message immediately. TLS certificate verification is
kept enabled.

## 10. Users, roles and security

Open **Users**.

- Users belong to a role. Built-in roles are admin, operator and viewer; custom
  roles select individual Web and API permissions.
- Web permissions cover dashboard, preview, playback, download, channels, plans,
  storage, alerts, users, logs, system and maintenance.
- API permissions cover `api.access`, `api.snapshot`, `api.stream`,
  `api.download`, `api.alerts` and `rtsp.access`.
- Disabling a user invalidates their sessions at the next request.

Lockout defaults, all configurable in **System**:

| Policy | Default |
|---|---|
| Web login failures before lockout | 10 |
| Web lockout duration | 10 minutes |
| API failures before lockout | 100 |
| API lockout duration | 10 minutes |

Lockouts and unlocks are audited. Camera and SMTP passwords are encrypted with
AES-256-GCM using `<data-dir>/secret.key` (mode 0600). Back up that file with the
database; without it the stored credentials cannot be recovered.

### Maintenance mode

Start the service with `--maintenance` to expose destructive operations: manual
video/log deletion, serial number changes, version changes and full-disk
formatting. These actions require an administrator, a second confirmation and
are always audited. Outside maintenance mode they return forbidden.

## 11. System settings

**System** contains:

- Security: lockout thresholds, audit retention (default 180 days), time zone,
  UI language and GPU enable/disable.
- Time: current server time, NTP enable/server/offset, immediate NTP sync, and
  "use browser time" calibration. The page clock updates every second. Both
  sync actions also set the clock on ONVIF-capable cameras (Hikvision/Dahua via
  ONVIF) and report how many cameras were synchronized; pure RTSP cameras are
  skipped.
- Performance: detected tier (low/medium/high), GPU devices and active FFmpeg
  mode. Low-tier systems default to the sub stream off; the tier can be
  overridden.
- Email: SMTP host, port, user, password, sender and test send.
- API tokens: create, list and revoke.
- Logging: application log location and level; audit logs are stored in SQLite.

The dashboard shows CPU, memory, disk, channel online counts, recording state,
sessions, alerts and bitrate trends. It is the default landing page.

## 12. Audit log

Open **Logs**. Filter by category (auth, channel, record, storage, API, system,
alert, permission, maintenance), level, result, actor, source IP, keyword and
time range. Results are paginated and can be exported as CSV.

Audit entries expire after the configured retention period. Manual deletion is
reserved for maintenance mode.

## 13. Help and about

- **Help** contains the searchable in-app manual and troubleshooting guide.
- **About** shows the version, git commit, serial number, build information,
  dependencies, licences and the project URL.

## 14. Troubleshooting

| Symptom | Check |
|---|---|
| Camera stays offline | Test the RTSP URL in VLC/ffplay; verify credentials, port and ONVIF user rights; read the channel last error. |
| Preview is black | Use an H.264 sub stream; H.265 MSE playback depends on the browser and hardware. Check that the stream is enabled. |
| No recording | The plan must cover the current weekday/time, the volume must be enabled and writable, and the channel must be online. |
| Cannot bind port 554 | Grant `CAP_NET_BIND_SERVICE` or run as root on Linux; otherwise use the fallback port printed in the log and update client URLs. |
| Email test fails | QQ Mail requires an authorization code and STARTTLS/SMTPS; check host, port and TLS. |
| Login locked | Wait for the configured lockout window. The remaining time is shown in the error and the event is audited. |
| Disk fills up | Lower retention days, enable loop mode, or add a volume. Free space must exceed the reserve threshold. |
| H.265 RTSP client has no video | 0.1.0 outputs RTP/MP2T; use a client that accepts payload type 33, or the browser preview for H.264. |

More answers are in [`faq.md`](faq.md). For security-sensitive reports follow
[`../SECURITY.md`](../SECURITY.md).
