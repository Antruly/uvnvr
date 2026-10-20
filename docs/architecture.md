# Architecture notes

This document starts as a short M0 record; it will grow with the media core.

## Process model

UvNVR is a single process with one libuv-based control plane and isolated workers:

- libuv loops: HTTP/1.1, HTTP/2, WebSocket, REST, RTSP signalling, timers.
- One FFmpeg RTSP ingest thread per enabled channel stream (main and optional sub).
- One disk writer per storage volume for recording segments.
- A small decode pool for snapshots, motion detection and optional transcoding.
- One serialized SQLite writer plus a read pool for configuration and index queries.

Blocking FFmpeg calls never run on a libuv loop. Packets and status cross the boundary through
bounded queues and `uv_async_t`.

## Media path

```
RTSP camera → FFmpeg demux (thread) → timestamp normalisation → channel TS muxer
   ├─ LiveHub (lossy, GOP cache) → WebSocket clients
   ├─ RecordingSpool (reliable, back-pressure) → segment writer → .ts + SQLite index
   └─ decoder tap → snapshot / motion
```

One camera connection is shared by every consumer. Live clients can drop to the next keyframe;
recording does not drop. Timestamps are mapped to a per-channel monotonic timeline so segment
files can be concatenated without jumps after a camera reconnect.

## Storage path

Segments are MPEG-TS, keyframe-aligned, immutable, and named by channel/time. The index is
SQLite (WAL). Writes go through a single-producer/single-consumer ring buffer and are batched
before reaching io_uring (Linux) or the libuv file pool (Windows/macOS). Playback and download
share an LRU block cache and a read-ahead layer; callers only ask for an index and a byte range.

## Decisions to revisit

- Browser playback: MSE over WebSocket for 0.1.0; WebRTC is a 0.2 candidate.
- Recording container: MPEG-TS internally; MP4 is an export/remux format.
- Transcoding is never on the recording path; it is limited to compatibility preview,
  snapshots and optional watermark burn-in.
- FFmpeg is LGPL, dynamically loaded, with an explicit CI license gate once integrated.
