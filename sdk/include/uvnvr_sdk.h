/**
 * UvNVR private-protocol C SDK (x64).
 *
 * This is the only public header. It is plain C (C89-compatible declarations,
 * C99 integer types) and is safe to include from C++ with extern "C".
 *
 * Threading model
 * ---------------
 * `uvnvr_create` starts one internal libuv event-loop thread. Every API
 * function below is blocking and may be called from any thread; calls are
 * serialised through the loop. `uvnvr_frame_cb` and `uvnvr_progress_cb` are
 * invoked on that internal loop thread, one at a time, in arrival order.
 * Do not call a blocking SDK function from a callback (it would wait for the
 * same loop); use a worker thread or a queue instead.
 *
 * `uvnvr_last_error` and `uvnvr_last_error_code` return per-thread values and
 * are valid until the next SDK call on the same thread.
 *
 * Transport selection is internal: the SDK probes QUIC on the given UDP port
 * (default 1515), falls back to TCP on the same port when QUIC times out or is
 * blocked, and reconnects transparently. Applications never see sockets.
 *
 * Memory ownership
 * ----------------
 * - `uvnvr_get_channel_list` / `uvnvr_get_recordings` return one allocation;
 *   release it with the matching `uvnvr_free_*` function.
 * - `uvnvr_get_recording_days` returns a string array; release it with
 *   `uvnvr_free_strings`.
 * - Strings inside channel/recording structs are owned by that allocation.
 * - Frames passed to `uvnvr_frame_cb` are valid only for the duration of the
 *   callback; copy them if you need to keep the data.
 */

#ifndef UVNVR_SDK_H
#define UVNVR_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(UVNVR_SDK_BUILD)
#define UVNVR_API __declspec(dllexport)
#else
#define UVNVR_API __declspec(dllimport)
#endif
#define UVNVR_CALL __cdecl
#else
#define UVNVR_API __attribute__((visibility("default")))
#define UVNVR_CALL
#endif

/** Opaque SDK handle. */
typedef struct uvnvr_sdk uvnvr_sdk_t;

/** Error codes. Values above 100 mirror the REST API's HTTP status. */
typedef enum uvnvr_error {
  UVNVR_OK = 0,
  UVNVR_ERR_INVALID_ARGUMENT = 1,
  UVNVR_ERR_NOT_CONNECTED = 2,
  UVNVR_ERR_TIMEOUT = 3,
  UVNVR_ERR_NETWORK = 4,
  UVNVR_ERR_PROTOCOL = 5,
  UVNVR_ERR_CANCELLED = 6,
  UVNVR_ERR_NO_MEMORY = 7,
  UVNVR_ERR_CLOSED = 8,
  UVNVR_ERR_AUTH = 401,
  UVNVR_ERR_FORBIDDEN = 403,
  UVNVR_ERR_NOT_FOUND = 404,
  UVNVR_ERR_CONFLICT = 409,
  UVNVR_ERR_LOCKED = 429,
  UVNVR_ERR_SERVER = 500,
  UVNVR_ERR_UNSUPPORTED = 501,
  UVNVR_ERR_UNAVAILABLE = 503
} uvnvr_error_t;

typedef enum uvnvr_transport {
  UVNVR_TRANSPORT_UNKNOWN = 0,
  UVNVR_TRANSPORT_TCP = 1,
  UVNVR_TRANSPORT_QUIC = 2
} uvnvr_transport_t;

/** Live-stream selector accepted by uvnvr_live_open. */
typedef enum uvnvr_stream {
  UVNVR_STREAM_MAIN = 0,
  UVNVR_STREAM_SUB = 1
} uvnvr_stream_t;

/**
 * AI event kinds. The values are a bitmask: pass one or both to
 * uvnvr_ai_subscribe.
 */
typedef enum uvnvr_ai_event_type {
  UVNVR_AI_EVENT_PLATE = 1,
  UVNVR_AI_EVENT_FACE = 2
} uvnvr_ai_event_type_t;

/**
 * SDK options. Always initialise with uvnvr_options_init before changing
 * fields; `struct_size` lets future versions append fields without breaking
 * existing callers. A zero value in a timeout field means "use the default".
 */
typedef struct uvnvr_options {
  uint32_t struct_size;
  uint32_t connect_timeout_ms;    /**< default 5000 */
  uint32_t request_timeout_ms;    /**< default 10000 */
  uint32_t quic_probe_timeout_ms; /**< default 400; 0 disables QUIC */
  int32_t prefer_quic;            /**< default 1 */
  int32_t verify_peer;            /**< default 0 (self-signed recorders) */
  const char* ca_file;            /**< optional CA bundle when verify_peer=1 */
  uint32_t heartbeat_interval_ms; /**< default 15000 */
  uint32_t session_renew_ms;      /**< default 600000 */
  int32_t disable_tcp;            /**< default 0; never fall back to TCP */
} uvnvr_options_t;

/** Channel metadata returned by uvnvr_get_channel_list. */
typedef struct uvnvr_channel {
  uint32_t struct_size;
  int64_t id;
  int64_t group_id;
  int32_t port;
  int32_t enabled;
  int32_t sub_enabled;
  int32_t online;      /**< mainState == "online" */
  int32_t has_keyframe;
  const char* name;
  const char* protocol;
  const char* host;
  const char* main_state;
  const char* main_error;
  const char* sub_state;
  const char* sub_error;
} uvnvr_channel_t;

/** Recording segment metadata returned by uvnvr_get_recordings. */
typedef struct uvnvr_recording {
  uint32_t struct_size;
  int64_t id;
  int64_t channel_id;
  int64_t start_ms;
  int64_t end_ms;
  int64_t duration_ms;
  int64_t size_bytes;
  int32_t active;
  const char* stream;
  const char* codec;
  const char* record_type;
  const char* path;
} uvnvr_recording_t;

/** One MPEG-TS frame delivered by a live subscription. */
typedef struct uvnvr_frame {
  uint32_t struct_size;
  int32_t codec;  /**< 0 unknown, 1 H.264, 2 H.265 */
  uint32_t flags; /**< bit 0 = keyframe */
  int64_t pts_ms;
  const uint8_t* data;
  size_t size;
} uvnvr_frame_t;

/** Live frame callback. Runs on the SDK's internal loop thread. */
typedef void(UVNVR_CALL* uvnvr_frame_cb)(uvnvr_sdk_t* sdk, int handle,
                                         const uvnvr_frame_t* frame, void* user);

/** Detection box in source-frame pixels, matching the REST/WS `box` object. */
typedef struct uvnvr_ai_box {
  int32_t x;
  int32_t y;
  int32_t w;
  int32_t h;
} uvnvr_ai_box_t;

/**
 * One AI recognition event delivered to uvnvr_ai_event_cb.
 *
 * Field names and meaning mirror the REST event item
 * (`GET /api/v1/ai/events`) and the /ws/events realtime message:
 *   - `id`           event id, usable with the REST snapshot endpoint;
 *   - `type`         UVNVR_AI_EVENT_PLATE or UVNVR_AI_EVENT_FACE;
 *   - `channel_id`   REST `channelId`;
 *   - `ts`           epoch seconds;
 *   - `confidence`   detection confidence;
 *   - `quality`      face quality (0 for plate events);
 *   - `plate`/`plate_norm`/`color`  plate events only, empty otherwise;
 *   - `matched_face_id`, `matched_score`, `matched_name`  face library hit;
 *                    `matched_face_id == 0` means no match;
 *   - `image_url`/`crop_url` relative snapshot URLs, empty if not saved;
 *   - `alert`        watchlist hit flag (WS `alert`).
 *
 * The pointers are owned by the SDK and are valid only for the duration of
 * the callback. Copy anything you need to keep.
 */
typedef struct uvnvr_ai_event {
  uint32_t struct_size;
  int64_t id;
  int32_t type; /**< uvnvr_ai_event_type_t */
  int64_t channel_id;
  int64_t ts;
  int32_t alert;
  double confidence;
  double quality;
  const char* channel_name;
  const char* plate;
  const char* plate_norm;
  const char* color;
  uvnvr_ai_box_t box;
  const char* engine;
  int64_t matched_face_id;
  double matched_score;
  const char* matched_name;
  const char* image_url;
  const char* crop_url;
} uvnvr_ai_event_t;

/** AI event callback. Runs on the SDK's internal loop thread. */
typedef void(UVNVR_CALL* uvnvr_ai_event_cb)(uvnvr_sdk_t* sdk, int handle,
                                            const uvnvr_ai_event_t* event,
                                            void* user);

/** Download progress callback. Runs on the SDK's internal loop thread. */
typedef void(UVNVR_CALL* uvnvr_progress_cb)(uvnvr_sdk_t* sdk, int64_t received,
                                            int64_t total, void* user);

/** Library version, e.g. "UvNVR SDK 0.1.0 (protocol 1)". */
UVNVR_API const char* UVNVR_CALL uvnvr_version(void);

/** Fills `options` with defaults; sets `struct_size`. */
UVNVR_API void UVNVR_CALL uvnvr_options_init(uvnvr_options_t* options);

/** Creates the SDK and starts its loop thread. `options` may be NULL. */
UVNVR_API uvnvr_sdk_t* UVNVR_CALL uvnvr_create(const uvnvr_options_t* options);

/** Stops the loop thread and releases every SDK allocation. */
UVNVR_API void UVNVR_CALL uvnvr_destroy(uvnvr_sdk_t* sdk);

/**
 * Connects to a recorder. Picks QUIC or TCP automatically. Returns UVNVR_OK or
 * an error; on failure use uvnvr_last_error.
 */
UVNVR_API int UVNVR_CALL uvnvr_connect(uvnvr_sdk_t* sdk, const char* host,
                                       uint16_t port, int timeout_ms);

/** Authenticates and obtains a temporary communication ID. */
UVNVR_API int UVNVR_CALL uvnvr_login(uvnvr_sdk_t* sdk, const char* user,
                                     const char* pass);

/** Invalidates the communication ID. The transport is kept open. */
UVNVR_API void UVNVR_CALL uvnvr_logout(uvnvr_sdk_t* sdk);

/** Returns the channels. Caller frees with uvnvr_free_channels. */
UVNVR_API int UVNVR_CALL uvnvr_get_channel_list(uvnvr_sdk_t* sdk,
                                                uvnvr_channel_t** out,
                                                size_t* count);

UVNVR_API void UVNVR_CALL uvnvr_free_channels(uvnvr_channel_t* channels,
                                              size_t count);

/**
 * Returns local calendar dates ("YYYY-MM-DD") with at least one segment for
 * `channel_id` between `from` and `to` (inclusive). Frees with
 * uvnvr_free_strings.
 */
UVNVR_API int UVNVR_CALL uvnvr_get_recording_days(uvnvr_sdk_t* sdk,
                                                  int channel_id,
                                                  const char* from,
                                                  const char* to, char*** days,
                                                  size_t* count);

UVNVR_API void UVNVR_CALL uvnvr_free_strings(char** strings, size_t count);

/**
 * Returns recording segments. `stream` may be NULL ("main" or "sub");
 * `from_ms`/`to_ms` are Unix epoch milliseconds and 0 means unbounded.
 * Frees with uvnvr_free_recordings.
 */
UVNVR_API int UVNVR_CALL uvnvr_get_recordings(
    uvnvr_sdk_t* sdk, int channel_id, const char* stream, int64_t from_ms,
    int64_t to_ms, uvnvr_recording_t** out, size_t* count);

UVNVR_API void UVNVR_CALL uvnvr_free_recordings(uvnvr_recording_t* recordings,
                                                size_t count);

/**
 * Opens a live subscription. Returns a handle >= 0, or a negative
 * uvnvr_error_t. Frames are delivered as MPEG-TS on the loop thread.
 */
UVNVR_API int UVNVR_CALL uvnvr_live_open(uvnvr_sdk_t* sdk, int channel_id,
                                         int stream, uvnvr_frame_cb cb,
                                         void* user);

/** Closes a live subscription. Returns UVNVR_OK or UVNVR_ERR_INVALID_ARGUMENT. */
UVNVR_API int UVNVR_CALL uvnvr_live_close(uvnvr_sdk_t* sdk, int handle);

/**
 * Subscribes to AI recognition events. Returns a handle >= 0, or a negative
 * uvnvr_error_t. `types` is a bitmask of uvnvr_ai_event_type_t; `channel_id`
 * <= 0 receives every channel. Events are pushed as the recorder recognises
 * them (the same events /ws/events broadcasts) and delivered on the loop
 * thread. A subscription does not survive a reconnect; subscribe again.
 *
 * Requires the `alert.view` permission. Returns UVNVR_ERR_UNAVAILABLE when
 * AI support is not compiled into the recorder or not running.
 */
UVNVR_API int UVNVR_CALL uvnvr_ai_subscribe(uvnvr_sdk_t* sdk, int channel_id,
                                            unsigned int types,
                                            uvnvr_ai_event_cb cb, void* user);

/** Cancels an AI subscription. Returns UVNVR_OK or UVNVR_ERR_INVALID_ARGUMENT. */
UVNVR_API int UVNVR_CALL uvnvr_ai_unsubscribe(uvnvr_sdk_t* sdk, int handle);

/**
 * Downloads one finalized recording to `path`, synchronously. `progress` may
 * be NULL. Returns UVNVR_OK only after the file is complete.
 */
UVNVR_API int UVNVR_CALL uvnvr_download_recording(uvnvr_sdk_t* sdk,
                                                  int64_t recording_id,
                                                  const char* path,
                                                  uvnvr_progress_cb progress,
                                                  void* user);

/** Human-readable error from the last call on this thread (never NULL). */
UVNVR_API const char* UVNVR_CALL uvnvr_last_error(uvnvr_sdk_t* sdk);

/** Numeric error from the last call on this thread. */
UVNVR_API int UVNVR_CALL uvnvr_last_error_code(uvnvr_sdk_t* sdk);

/** Transport currently in use (UVNVR_TRANSPORT_UNKNOWN before connecting). */
UVNVR_API uvnvr_transport_t UVNVR_CALL uvnvr_transport(uvnvr_sdk_t* sdk);

/** "quic", "tcp" or "unknown". */
UVNVR_API const char* UVNVR_CALL uvnvr_transport_name(uvnvr_sdk_t* sdk);

/** Aborts in-flight requests and downloads with UVNVR_ERR_CANCELLED. */
UVNVR_API void UVNVR_CALL uvnvr_cancel(uvnvr_sdk_t* sdk);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UVNVR_SDK_H */
