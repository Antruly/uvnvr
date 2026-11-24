# AI recognition pipeline

Phase 1 ships the framework for licence-plate and face recognition: capture
timers, bounded queues, worker pools, persistence, snapshots, REST, WebSocket
push and alerts. The recognisers themselves are pluggable; this phase provides
a deterministic stub. HyperLPR3 (plates) and InspireFace (faces) are wired in
during phase 2/3 without changing the public interfaces below.

## Architecture

```
                    one capture timer per enabled channel
 channels ──► CaptureLoop ── ffmpeg snapshot (sub, fallback main)
                    │  cv::imdecode → BGR cv::Mat
                    │  optional ROI crop per detector
          ┌─────────┴─────────┐
          ▼                   ▼
   plate queue (cap N)   face queue (cap N)     full ⇒ drop oldest, count dropped
          │                   │
   ┌──────┴──────┐     ┌──────┴──────┐
   │ plate pool  │     │ face pool   │         each thread owns one AiEngine
   │  (default 2)│     │ (default 2) │
   └──────┬──────┘     └──────┬──────┘
          │  post-process      │  quality filter / face match
          ▼                    ▼
   ai_plate_events        ai_face_events
   + full/crop JPEG       + full/crop JPEG
          │                    │
          ├─ /ws/events  {"type":"ai.plate"} / {"type":"ai.face"}
          └─ alerts row when a watchlist matches
```

The event loop never performs capture, recognition, JPEG encoding or disk I/O.
Those run on the capture/worker threads. Database access uses the existing
thread-safe `uvcpp_db_pool`.

## Thread model

| Thread | Count | Responsibility |
|---|---|---|
| capture timer | one per AI-enabled channel | sleep to `intervalMs`, call `MediaService::CaptureJpegFromStream`, decode with `cv::imdecode`, enqueue copies |
| plate worker | `ai.plateWorkers` (default 2) | owns its own `AiEngine`; detect, normalise, dedup, write DB/JPEG, broadcast |
| face worker | `ai.faceWorkers` (default 2) | owns its own `AiEngine`; detect, quality filter, face match, write DB/JPEG, broadcast |
| maintenance | 1 while AI is enabled | once per day, delete events older than `ai.snapshotRetentionDays` and their JPEGs |

Rules:

* An `AiEngine` instance is **not** thread-safe. It is created, used and
  destroyed by exactly one worker thread. No engine object is shared.
* Queues are bounded. When full, the oldest frame is discarded (real-time
  first) and `dropped` increments.
* `Stop()` is idempotent and joins every thread. `Reload()` stops and starts
  the pipeline, so per-channel switches take effect without restarting the
  service.
* With `ai.enabled=0` no AI threads are created and capture never runs.

## Settings

Global settings live in the existing `settings` table and are managed through
`GET/PUT /api/v1/ai/config` (camelCase in JSON, `ai.*` keys in SQLite).

| JSON key | Setting | Default | Range |
|---|---|---|---|
| `enabled` | `ai.enabled` | `0` | bool |
| `plateEnabled` | `ai.plate.enabled` | `1` | bool |
| `faceEnabled` | `ai.face.enabled` | `1` | bool |
| `intervalMs` | `ai.intervalMs` | `1000` | 250–10000 |
| `queueDepth` | `ai.queueDepth` | `8` | 1–64 |
| `plateWorkers` | `ai.plateWorkers` | `2` | 1–16 |
| `faceWorkers` | `ai.faceWorkers` | `2` | 1–16 |
| `minPlateConfidence` | `ai.minPlateConfidence` | `0.6` | 0–1 |
| `minFaceQuality` | `ai.minFaceQuality` | `0.3` | 0–1 |
| `snapshotRetentionDays` | `ai.snapshotRetentionDays` | `30` | 1–3650 |
| `stub` | `ai.stub` | `0` | bool |
| `plateWatchlist` | `ai.plate.watchlist` | empty | comma/space separated plates |
| `faceWatchlist` | `ai.face.watchlist` | empty | comma separated `ai_faces.id` |
| `faceMatchThreshold` | `ai.face.matchThreshold` | `0.75` | 0–1 |
| `dedupSeconds` | `ai.dedupSeconds` | `10` | 0–3600 |

Per-channel overrides live in `ai_channel_settings`:

| Column | Meaning |
|---|---|
| `plate_enabled` / `face_enabled` | this channel's switches (default off) |
| `interval_ms` | `0` inherits `ai.intervalMs`; otherwise 250–10000 |
| `plate_roi` / `face_roi` | `x,y,w,h` in percent of the frame; empty = full frame |

Both global switches and the channel switch must be on for capture to start.
The global config is read at `Start`/`Reload`; changing one channel restarts the
pipeline, so the new state is effective immediately.

## API

All routes require a session, an API token or an event ticket as usual; the
permission column is the UvNVR permission checked after authentication.

| Method | Path | Permission | Purpose |
|---|---|---|---|
| GET | `/api/v1/ai/config` | `alert.view` | global config + `running` |
| PUT | `/api/v1/ai/config` | `channel.manage` | validate and persist, hot reload |
| GET | `/api/v1/channels/:id/ai` | `alert.view` | channel switches and effective state |
| PUT | `/api/v1/channels/:id/ai` | `channel.manage` | `{plateEnabled,faceEnabled,intervalMs}` (partial), hot reload |
| PUT | `/api/v1/ai/channels` | `channel.manage` | `{channelIds:[],plateEnabled,faceEnabled}` |
| GET | `/api/v1/ai/events` | `alert.view` | `type=plate\|face&channelId=&from=&to=&plate=&limit=&offset=` |
| GET | `/api/v1/ai/events/:id` | `alert.view` | one event (`type` query) |
| GET | `/api/v1/ai/events/:id/snapshot` | `alert.view` | `type=plate\|face&kind=full\|crop`, `image/jpeg` |
| GET | `/api/v1/ai/plates` | `alert.view` | trajectory for one plate, up to 200 rows |
| GET | `/api/v1/ai/stats` | `alert.view` | workers, processed, dropped, avgMs, fps, channels |
| POST | `/api/v1/ai/test` | `channel.manage` | grab one live frame and run the configured engines |
| GET | `/api/v1/ai/faces` | `alert.view` | local face library |
| POST | `/api/v1/ai/faces` | `channel.manage` | `{name,note?,feature?:number[],imagePath?}` |
| PUT | `/api/v1/ai/faces/:id` | `channel.manage` | partial update |
| DELETE | `/api/v1/ai/faces/:id` | `channel.manage` | delete |

`from`/`to` are epoch **seconds**. Event `ts` is also seconds. `limit` defaults
to 50 and is capped at 500.

### WebSocket messages

`/ws/events` (ticket from `POST /api/v1/events/ticket`, unchanged) carries two
new message types:

```json
{"type":"ai.plate","ts":1795515031,"channel":{"id":1,"name":"LabCam"},
 "plate":"京A12345","confidence":0.93,"box":{"x":128,"y":240,"w":160,"h":48},
 "color":"蓝","engine":"stub",
 "imageUrl":"/api/v1/ai/events/4/snapshot?type=plate&kind=full",
 "cropUrl":"/api/v1/ai/events/4/snapshot?type=plate&kind=crop","alert":true}
```

```json
{"type":"ai.face","ts":1795515031,"channel":{"id":1,"name":"LabCam"},
 "quality":0.81,"confidence":0.9,"box":{"x":213,"y":120,"w":106,"h":120},
 "engine":"stub","matched":{"id":2,"name":"张三","score":0.87},
 "imageUrl":"/api/v1/ai/events/9/snapshot?type=face&kind=full",
 "cropUrl":"/api/v1/ai/events/9/snapshot?type=face&kind=crop","alert":true}
```

`matched` is `null` when no face-library entry reaches
`ai.face.matchThreshold`. Pushes go through the existing asynchronous
`EventHub`; a slow client cannot block recognition.

## Persistence

Tables created by the normal migration:

* `ai_plate_events` — plate, normalised plate, confidence, colour, box,
  image/crop path, engine, `ts`, `created_at`.
* `ai_face_events` — quality, detection confidence, box, image/crop path,
  raw float32 feature blob, engine, matched face id/score.
* `ai_faces` — local face library (`name`, `note`, `feature_blob`,
  `image_path`). The feature blob is a little-endian float32 vector.
* `ai_channel_settings` — per-channel switches/interval/ROI.

Snapshots are written to:

```
<data_dir>/ai/<yyyyMMdd>/<channel_id>/<capture-ts-ms>_plate_full.jpg
<data_dir>/ai/<yyyyMMdd>/<channel_id>/<capture-ts-ms>_plate_crop.jpg
<data_dir>/ai/<yyyyMMdd>/<channel_id>/<capture-ts-ms>_face_full.jpg
<data_dir>/ai/<yyyyMMdd>/<channel_id>/<capture-ts-ms>_face_crop.jpg
```

JPEG quality is 85. The crop expands the detected box by half of its longest
side on each edge (at least four times the box area) and clamps to the frame.
The date directory uses local time. Retention runs once per day and removes
both the rows and the files.

### Plate normalisation

`src/ai/plate_post.cpp` only accepts mainland China blue/green plates:
province character + issuing-authority letter + 5 or 6 serial characters. It
folds full-width ASCII, removes spaces/`·`/`-`, upper-cases letters, maps `I→1`
and `O→0` **in the serial segment only** (the authority letter keeps `O`, e.g.
`京O`). Duplicate hits with the same `plate_norm` on the same channel inside
`ai.dedupSeconds` are dropped before anything is written.

## Alerts

* `ai.plate.watchlist` contains comma/space-separated plates. A hit produces
  `alert=true` in the WebSocket message and inserts an `alerts` row with
  `type='ai_plate'`, the channel id, the normalised plate in the message and
  the full snapshot path.
* `ai.face.watchlist` contains `ai_faces.id` values. If the best cosine match
  is at least `ai.face.matchThreshold` and its id is on the list, the event gets
  `alert=true` and an `alerts` row with `type='ai_face'`.

Existing alert acknowledgement (`POST /api/v1/alerts/:id/ack`) works unchanged.

## Stub and build options

* `UVNVR_AI_STUB=1` (or `ai.stub=1`) makes the stub return a fixed
  `京A12345` plate (confidence 0.93) and one face (quality 0.81, 128-float
  feature). Without it the stub returns empty results.
* `UVNVR_AI_STUB_DELAY_MS=<n>` adds artificial per-frame inference time. It is
  only for exercising queue overflow and `dropped` accounting.
* `UVNVR_ENABLE_AI` (CMake, default ON) requires OpenCV `core`, `imgproc` and
  `imgcodecs`. When OpenCV is not found the option is forced off, the module is
  not compiled, `UVNVR_ENABLE_AI=0` is defined and the rest of the service is
  unchanged. Linux uses the system OpenCV; a Windows MinGW build looks for
  `~/.local/opencv4-mingw`.
* Unit test: configure with `-DUVNVR_BUILD_TESTS=ON`, then `ctest` runs
  `ai_plate_post` (no OpenCV needed).

## Phase 2/3 integration points

1. Implement HyperLPR3/InspireFace behind `uvnvr::AiEngine` in
   `src/ai/ai_engine.cpp` (`CreatePlateEngine` / `CreateFaceEngine`).
2. Keep `Name()` stable (`"hyperlpr3"`, `"inspireface"`); it is stored in the
   `engine` column and shown by `POST /api/v1/ai/test`.
3. Return normalised boxes in the **input** image coordinate space; the
   pipeline adds the ROI offset and writes snapshots/crops itself.
4. Face `feature` should be a unit-length float vector; the local face library
   already compares it with cosine similarity and stores it as float32 BLOB.
5. Set `AiConfig::model_dir` from a future setting; per-worker construction
   guarantees no engine instance is shared across threads.
