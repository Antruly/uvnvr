# UvNVR Private Protocol (UVNVR-PP) — version 1

Status: **stable v1**, implemented by `src/proto/` and `sdk/`.

This document is the normative wire specification for the UvNVR private
protocol. It is the contract between the recorder and the C SDK
(`sdk/include/uvnvr_sdk.h`) and is intended to be implementable by any client
that can open a TCP socket or a QUIC connection. Changes that alter the byte
layout or semantics require a new `version`.

- Default port: **1515**, TCP and UDP/QUIC on the same port number.
- Payload encoding: UTF-8 JSON for control messages, binary for media.
- Byte order: **little-endian** for every integer field.
- Intellectual property: none; the protocol is part of the UvNVR project.

## 1. Design summary

| Concern | Decision |
|---|---|
| TCP framing | 32-byte fixed header carries `payload_len`; no separate length prefix. |
| QUIC framing | Same 32-byte header on a **long-lived bidirectional control stream**. |
| QUIC library | The recorder and SDK use the QUIC implementation shipped with the pinned `libuvcpp` build (ngtcp2 + TLS 1.3). The **MsQuic** backend named in the task brief is not installed on the build hosts, so the transport layer is isolated behind `proto_server.cpp` / `sdk/src/proto_client.cpp` and a MsQuic backend can be dropped in without touching the dispatcher or the C API. `UVNVR_ENABLE_QUIC=ON` enables the QUIC listener; if the linked libuvcpp was built without QUIC the server starts TCP-only and clients fall back automatically. |
| Encryption | QUIC is TLS 1.3 (self-signed certificate by default). TCP is plaintext. `ENCRYPTED` is reserved and never set: with QUIC the TLS record layer already encrypts, with TCP v1 deliberately leaves the bytes visible for wire debugging. Use TCP only on trusted networks or behind a tunnel. |
| Compression | None in v1. `COMPRESSED` is reserved and never set. |
| Integrity | CRC32 (IEEE 802.3, reflected polynomial `0xEDB88320`) over header+payload. |
| Multiplexing | One request/response stream; asynchronous media is demultiplexed by a 32-bit `stream_id` carried in the frame payload. |

### 1.1 Why one long-lived QUIC stream

A per-request stream is the natural QUIC shape, but the protocol needs three
properties that a single persistent stream gives for free:

1. **Session affinity after reconnect.** A resumed client sends its old
   `session_id` on the same stream it used before; no extra stream state has to
   be reconstructed on the server.
2. **Deterministic request ordering.** Live playback control (`LIVE_OPEN`,
   `LIVE_CLOSE`) and file transfer control (`DOWNLOAD_OPEN`) are small and
   latency-sensitive. One ordered stream means a slow request can never be
   overtaken by a later one in a way the client did not ask for.
3. **Simpler ownership.** The server maps one `ProtoConnection` to one QUIC
   connection and one control stream; adding streams would add a second table
   whose lifetime is another source of use-after-free.

The obvious cost is head-of-line blocking between a large transfer and a live
frame. The protocol bounds it: media frames are capped at `maxFrame` (4 MiB,
default 256 KiB in the SDK sender), download chunks carry explicit offsets so a
client can resume, and live frames are delivered on the same event loop rather
than waiting for the whole file. Measured on the Linux test host a 10 MiB
download is 40 chunks of 256 KiB; a live frame queued behind one chunk waits
for one chunk, not for the whole file.

## 2. Frame format

Every frame, on both transports, is:

```
 offset  size  field          notes
 ------  ----  -------------  ------------------------------------------------
 0       4     magic          0x524E5655; bytes on the wire are 'U' 'V' 'N' 'R'
 4       1     version        1
 5       1     header_len     32 (allows a future extension header)
 6       2     flags          see §2.1
 8       2     opcode         see §3
 10      2     reserved       must be 0
 12      8     session_id     0 before AUTH; temporary communication ID after
 20      4     request_id     client-chosen, unique per connection while in flight
 24      4     payload_len    bytes following the header; <= maxFrame
 28      4     crc32          see §2.2
 32      N     payload        JSON or binary depending on opcode
```

`maxFrame` is advertised in `HELLO_ACK` (default 4 MiB). A receiver MUST close
the connection on `payload_len > maxFrame`, on a bad magic, on `version != 1`,
or on a CRC mismatch. A receiver MUST NOT trust `session_id` until `AUTH_ACK`
has been received.

### 2.1 Flags

| bit | name | meaning |
|---|---|---|
| 0x0001 | `RESPONSE` | frame is the reply to `request_id` |
| 0x0002 | `ERROR` | payload is the error object (§6); implies `RESPONSE` |
| 0x0004 | `FIN` | last frame of a streamed response (`DOWNLOAD_DONE`) |
| 0x0008 | `COMPRESSED` | reserved; MUST be 0 in v1 |
| 0x0010 | `ENCRYPTED` | reserved; MUST be 0 in v1 |
| 0x0020 | `STREAM` | payload starts with a 4-byte `stream_id` (§7) |

Unknown flag bits MUST be ignored.

### 2.2 CRC32

`crc32` is the IEEE 802.3 CRC32 of the 32 header bytes **with the crc32 field
set to 0**, immediately followed by the payload. It is not keyed and is not a
security control; it detects truncation, framing desynchronisation and
transport corruption. Authentication and confidentiality come from TLS on
QUIC and from the AUTH exchange.

### 2.3 TCP framing

TCP is a byte stream: the receiver accumulates bytes, reads a header when at
least 32 bytes are buffered, rejects it if invalid, then waits until
`32 + payload_len` bytes are available. There is no second length prefix and
no delimiter. One TCP connection carries one protocol session at a time.

### 2.4 QUIC framing

QUIC runs on UDP port 1515 with ALPN **`uvnvr/1`**. TLS 1.3 is mandatory (QUIC
has no cleartext mode). After the handshake the client opens one bidirectional
stream and writes `HELLO` on it; that stream is the control stream for the
lifetime of the connection. Frames are serialised exactly as on TCP. A
`STREAM`-flagged frame is an asynchronous server-to-client event; its first
four payload bytes identify the live or download handle.

The client SHOULD NOT open additional streams in v1. A server that receives a
second stream MUST ignore it (read and discard) rather than close the
connection; this keeps the door open for a future multiplexing revision.

## 3. Opcodes

| opcode | name | direction | payload |
|---|---|---|---|
| 0x0001 | `HELLO` | C → S | `hello` object |
| 0x0002 | `HELLO_ACK` | S → C | `helloAck` object |
| 0x0003 | `AUTH` | C → S | `auth` object |
| 0x0004 | `AUTH_ACK` | S → C | `authAck` object |
| 0x0005 | `PING` | both | empty |
| 0x0006 | `PONG` | both | empty |
| 0x0008 | `LOGOUT` | C → S | empty |
| 0x0009 | `SESSION_RENEW` | C → S | empty |
| 0x0010 | `SYSTEM_INFO` | C → S | empty |
| 0x0020 | `CHANNEL_LIST` | C → S | empty; reply is a channel array |
| 0x0021 | `RECORDING_LIST` | C → S | `recordingQuery` |
| 0x0022 | `RECORDING_DAYS` | C → S | `daysQuery` |
| 0x0030 | `LIVE_OPEN` | C → S | `liveOpen` |
| 0x0031 | `LIVE_ACK` | S → C | `liveAck` |
| 0x0032 | `LIVE_FRAME` | S → C | binary, `STREAM` (§7.1) |
| 0x0033 | `LIVE_CLOSE` | C → S | `liveClose` |
| 0x0034 | `PTZ_CONTROL` | C → S | `ptzControl` |
| 0x0040 | `DOWNLOAD_OPEN` | C → S | `downloadOpen` |
| 0x0041 | `DOWNLOAD_BEGIN` | S → C | `downloadBegin` |
| 0x0042 | `DOWNLOAD_CHUNK` | S → C | binary, `STREAM` (§7.2) |
| 0x0043 | `DOWNLOAD_DONE` | S → C | `downloadDone`, `FIN` |

Replies carry the request opcode and `RESPONSE`, except `HELLO→HELLO_ACK`,
`AUTH→AUTH_ACK`, `LIVE_OPEN→LIVE_ACK` and `DOWNLOAD_OPEN→DOWNLOAD_BEGIN`,
which have their own opcodes. `request_id` is copied verbatim into the reply.
`session_id` is copied into replies once a session exists.

## 4. Handshake

### 4.1 Transport negotiation (the "UDP probe")

The client resolves `host`, then attempts QUIC **first** when
`prefer_quic` is true:

1. Open UDP, send a QUIC Initial carrying ALPN `uvnvr/1`.
2. Wait up to `quic_probe_timeout_ms` (default **400 ms**, configurable via
   `uvnvr_options_t`; 0 disables QUIC) for the handshake to reach
   `ESTABLISHED`.
3. On success, send `HELLO` on the control stream. `HELLO_ACK.transports`
   confirms both transports on the same port.
4. On timeout or failure, close the QUIC endpoint and connect TCP with the
   remaining budget. TCP is always attempted when QUIC fails; a server with
   `privateTcpEnabled=false` answers the TCP connect with a reset, which the
   client reports as `UVNVR_ERR_NETWORK`.

There is deliberately **no separate plaintext UDP discovery datagram**. The
UDP port is owned by the QUIC endpoint, and an application-level probe
multiplexed with QUIC packets would require packet injection the QUIC library
does not expose. Using the QUIC Initial itself as the probe has the same
observable semantics (UDP reachability, server capability, timeout, fallback)
with one fewer protocol. The capability response is `HELLO_ACK`; there is no
unauthenticated capability oracle for a server that has QUIC disabled.

The chosen transport can be read back with `uvnvr_transport()`.

### 4.2 HELLO / HELLO_ACK

`HELLO` (JSON):

```json
{
  "protocol": 1,
  "client": "uvnvr-sdk/0.1.0",
  "transports": ["quic", "tcp"],
  "features": ["live", "download", "days"],
  "resumeSessionId": 0
}
```

`HELLO_ACK` (JSON, `opcode=0x0002`, `RESPONSE`):

```json
{
  "protocol": 1,
  "serverVersion": "0.19.0",
  "maxFrame": 4194304,
  "transports": ["tcp", "quic"],
  "alpn": "uvnvr/1",
  "compression": "none",
  "encryption": { "quic": "tls1.3", "tcp": "none" },
  "heartbeatMs": 15000,
  "sessionTtl": 3600
}
```

A server without QUIC reports `transports: ["tcp"]`; a client that reached it
over QUIC should not be able to observe that combination.

### 4.3 AUTH / AUTH_ACK

`AUTH` (JSON):

```json
{
  "username": "admin",
  "password": "…",
  "clientName": "control-room-1",
  "resumeSessionId": 0
}
```

The password is sent as supplied by the user. Over QUIC it is protected by
TLS 1.3. Over TCP it is **plaintext**; see §1. Future revisions may replace
this with a PAKE, but v1 deliberately reuses the recorder's existing
scrypt-hashed password store and audit path, so a successful AUTH performs the
same verification and lockout accounting as `POST /api/v1/auth/login`.

`AUTH_ACK` (JSON, `opcode=0x0004`, `RESPONSE`):

```json
{
  "sessionId": 7419022771366541000,
  "expiresIn": 3600,
  "permissions": ["channel.view","preview.view","playback.view","playback.download","system.view"],
  "user": { "id": 1, "username": "admin", "role": "admin" }
}
```

`sessionId` is the **temporary communication ID**. It is a 64-bit value from
a CSPRNG, never reused, and is required in the header of every request after
AUTH. It is not the web session cookie and cannot be used against the REST
API. On failure the server replies `AUTH` + `RESPONSE|ERROR` with the REST
status (401 `invalid_credentials`, 403 `account_disabled`, 429 `locked`).

### 4.4 Resume and re-authentication

A client that loses its transport may reconnect and:

1. send `HELLO` with `resumeSessionId` set, then
2. send a normal request carrying the old `session_id` in the header.

The server accepts requests without a second AUTH while the session exists,
has not expired and has not been logged out. On `401 session_expired` the
client MUST send `AUTH` again. Reconnect does not change `session_id`.

## 5. Keepalive, expiry and reconnect

| Parameter | Default | Behaviour |
|---|---|---|
| Heartbeat interval | 15 s | client sends `PING`; server answers `PONG` |
| Server idle timeout | 45 s | no frame from the peer ⇒ server closes the connection |
| Session TTL | 3600 s | advertised in `AUTH_ACK.expiresIn` |
| Renewal | 600 s | client sends `SESSION_RENEW`; server extends by one TTL and replies with `expiresIn` |
| QUIC probe | 400 ms | see §4.1 |
| Connect timeout | 5000 ms | `uvnvr_options_t.connect_timeout_ms` |
| Request timeout | 10000 ms | non-streaming calls; downloads use a larger idle timeout |

`PING`/`PONG` and `SESSION_RENEW` are exempt from the AUTH requirement only
for `PING`; `SESSION_RENEW` requires a valid session. A `PONG` is sent even if
the session has expired, because it is a transport liveness check.

When the SDK detects a closed transport it marks the connection down, fails
in-flight requests with `UVNVR_ERR_NETWORK`, and attempts one reconnect on the
next API call using the same host/port, the same transport preference, and the
stored `session_id`. Live handles are not resurrected automatically: their
`LIVE_FRAME` callbacks stop, and the application must call `uvnvr_live_open`
again. Downloads that fail mid-transfer return an error; the file is left with
the bytes received so far and the caller may retry with a new request.

## 6. Error model

An error is a normal frame with `flags = RESPONSE | ERROR`. The payload is:

```json
{
  "status": 404,
  "code": "not_found",
  "message": "channel not found",
  "details": {}
}
```

`status` uses HTTP semantics, and `code` is the same machine-readable string
the REST API returns for the equivalent condition, so a client can share
error handling between REST and the private protocol.

| status | SDK enum | typical codes |
|---|---|---|
| 400 | `UVNVR_ERR_INVALID_ARGUMENT` | `invalid_json`, `invalid_range`, `invalid_stream` |
| 401 | `UVNVR_ERR_AUTH` | `invalid_credentials`, `session_expired`, `missing_session` |
| 403 | `UVNVR_ERR_FORBIDDEN` | `account_disabled`, `forbidden` |
| 404 | `UVNVR_ERR_NOT_FOUND` | `not_found`, `no_recordings` |
| 409 | `UVNVR_ERR_CONFLICT` | `channel_limit` |
| 429 | `UVNVR_ERR_LOCKED` | `locked` |
| 500 | `UVNVR_ERR_SERVER` | `database_error`, `session_failed` |
| 501 | `UVNVR_ERR_UNSUPPORTED` | `unsupported_opcode` |
| 503 | `UVNVR_ERR_UNAVAILABLE` | `stream_unavailable`, `ffmpeg_unavailable` |

Transport-level failures (DNS, TCP reset, QUIC timeout, CRC, framing) are
mapped by the SDK to `UVNVR_ERR_NETWORK`, `UVNVR_ERR_TIMEOUT` or
`UVNVR_ERR_PROTOCOL`; `uvnvr_last_error()` carries a human-readable reason.

## 7. Streaming payloads

### 7.1 LIVE_FRAME

`LIVE_OPEN`:

```json
{ "channelId": 1, "stream": "main", "streamId": 17 }
```

`streamId` is client-chosen, non-zero, unique per connection. The server
replies `LIVE_ACK` `{"channelId":1,"stream":"main","streamId":17}` and then
emits frames with `opcode=0x0032`, `flags=STREAM`:

```
 0   u32  stream_id
 4   u8   codec      0 unknown (v1 always sends 0: the recorder fans out MPEG-TS
                     without decoding it, so it does not classify the codec here)
 5   u8   frame_flags reserved, 0 in v1 (the raw fan-out does not classify frames)
 6   u16  reserved
 8   u64  pts_ms     server send time, Unix epoch milliseconds
 16  ...  MPEG-TS bytes (verbatim fan-out of the recorder's Stream)
```

The `pts_ms` field is filled by the server at fan-out; clients should treat
`codec` and `frame_flags` as hints that may gain meaning in a later revision.

Frames are the same MPEG-TS bytes the recorder uses for live preview and
recording; the private protocol adds no second camera connection. The stream
ends with `LIVE_CLOSE` `{"streamId":17}` (client) or when the connection
closes. Subscriptions are reference-counted per (channel, stream) by the
existing `MediaService` fan-out.

### 7.2 DOWNLOAD_OPEN / DOWNLOAD_CHUNK

`DOWNLOAD_OPEN`:

```json
{ "recordingId": 42, "downloadId": 7 }
```

`DOWNLOAD_BEGIN`:

```json
{ "recordingId": 42, "downloadId": 7, "sizeBytes": 10485760, "codec": "h264" }
```

`DOWNLOAD_CHUNK` (`flags=STREAM`):

```
 0   u32  download_id
 4   u32  reserved
 8   u64  offset        byte offset of this chunk in the file
 16  ...  bytes
```

Offsets are absolute, so a client can detect a gap or resume. After the last
chunk the server sends `DOWNLOAD_DONE` with `flags = RESPONSE | FIN`:

```json
{ "downloadId": 7, "sizeBytes": 10485760, "chunks": 41 }
```

Only finalized segments (`id > 0`, `active=false`) can be downloaded, matching
the REST endpoint. `404 not_found` is returned for a missing row or file.

### 7.3 PTZ_CONTROL

`PTZ_CONTROL` (JSON, `opcode=0x0034`) drives the same ONVIF PTZ backend as the
GB/T 28181 `DeviceControl` path. It requires `channel.manage`:

```json
{
  "channelId": 1,
  "action": "right",
  "speed": 255,
  "preset": 0
}
```

`action` is one of `stop`, `right`, `left`, `up`, `down`, `up-right`,
`up-left`, `down-right`, `down-left`, `zoom-in`, `zoom-out`, `set-preset`,
`goto-preset`, `remove-preset`. `speed` is the 0..255 wire scale used by
GB/T 28181; preset actions require a positive `preset`.

The server queues the network call on a worker and replies after the camera
answers: `200 {"channelId":1,"action":"right","ok":true}`, or `502
ptz_failed` with the ONVIF error. Channels without an ONVIF endpoint fail
with that error; no session or stream state is touched.

## 8. Relationship to REST API v1

The private protocol is a second entry point to the same services, not a
reimplementation. The dispatcher in `src/app.cpp` calls the same
`MediaService`, `RecordingService` and database queries as the REST handlers:

| private op | REST equivalent | permission |
|---|---|---|
| `SYSTEM_INFO` | `GET /api/v1/system/info` (public subset) | none |
| `CHANNEL_LIST` | `GET /api/v1/channels` | `channel.view` |
| `RECORDING_DAYS` | `GET /api/v1/recordings/days` | `playback.view` |
| `RECORDING_LIST` | `GET /api/v1/recordings` | `playback.view` |
| `LIVE_OPEN` | `POST /api/v1/streams/ticket` + `/ws/live` | `preview.view` |
| `DOWNLOAD_OPEN` | `GET /api/v1/recordings/{id}/download` | `playback.download` |
| `PTZ_CONTROL` | no REST equivalent (shared ONVIF PTZ backend) | `channel.manage` |

Permissions are evaluated with the same `PermissionImplies` table as REST, so
`preview`-scoped accounts behave identically on both entry points.

## 9. Configuration

`uvnvr.json`:

```json
{
  "private": {
    "port": 1515,
    "tcp": true,
    "quic": true
  }
}
```

Flat aliases `privatePort`, `privateTcpEnabled`, `privateQuicEnabled` are also
accepted. Environment overrides: `UVNVR_PRIVATE_PORT`, `UVNVR_PRIVATE_TCP`,
`UVNVR_PRIVATE_QUIC`. `privatePort` defaults to 1515 and must not equal the
HTTP or RTSP port on the TCP side. When QUIC is enabled and no
`tls.cert`/`tls.key` is configured the recorder generates an in-memory
self-signed certificate for the QUIC listener; clients verify nothing by
default (TOFU), which is the documented SDK behaviour.

Set `UVNVR_PROTO_TRACE=1` to print accepted connections, decoded opcodes and
sent frames to stderr. Tracing is off by default and does not alter the wire
protocol.

## 10. Test vectors

A minimal `HELLO` on a fresh connection (41-byte payload, CRC `0xe906a25d`):

```
55 56 4e 52  01 20 00 00  01 00 00 00  00 00 00 00
00 00 00 00  29 00 00 00  5d a2 06 e9  7b 22 70 72
6f 74 6f 63  6f 6c 22 3a  31 2c 22 63  6c 69 65 6e
74 22 3a 22  75 76 6e 76  72 2d 73 64  6b 2f 30 2e
31 2e 30 22  7d
```

- magic bytes: `55 56 4e 52` (`UVNR`, the little-endian form of `0x524E5655`)
- version `01`, header_len `20` (32)
- flags `0000`, opcode `0100` (0x0001), reserved `0000`
- session `0`, request_id `0`, payload_len `0x29` (41)
- CRC `5d a2 06 e9` (little-endian `0xe906a25d`) covers the 32 header bytes
  with the CRC field zeroed plus the payload.

Clients should treat the CRC field as opaque and recompute rather than
compare against a hard-coded value.
