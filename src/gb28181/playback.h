#pragma once

// GB/T 28181 playback session (P6.2d): read recorded TS segments, mux them to
// PS with the same pipeline as live preview, and pace the RTP stream according
// to the platform's Scale/Range control.
//
// The controller owns one reader thread per dialog. It never touches the SIP
// event loop directly: frames are handed to a `Sender` callback, and the
// service forwards them to MediaSessionManager::SendFrame on the loop thread.
// This is what makes pause/seek safe: the reader can block while a new segment
// is opened without serialising the SIP stack behind a file read.
//
// Compiled only with UVNVR_ENABLE_GB28181=ON (default off).

#include "gb28181/ts_ps.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uvnvr::gb28181 {

// One recording row. `path` may point at a finalized file or at the `.part`
// file of a segment that is still being written.
struct PlaybackSegment {
  std::int64_t channel_id = 0;
  std::string stream;
  std::string path;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::int64_t size_bytes = 0;
  std::string codec;
  std::string record_type;
};

// Returns every segment overlapping [start_ms, end_ms), ordered by start time.
// The service implementation also appends active (not yet indexed) segments.
using SegmentSource =
    std::function<std::vector<PlaybackSegment>(std::int64_t channel_id, const std::string& stream,
                                               std::int64_t start_ms, std::int64_t end_ms)>;

// Reads `size` bytes at `offset`. A short read means end of file. The callback
// keeps the playback controller independent of storage encryption: the App
// supplies a plain file reader or the SM4 range decryptor.
using SegmentReadFn =
    std::function<bool(const std::string& path, std::uint64_t offset, std::size_t size,
                       std::vector<std::uint8_t>* out, std::string* error)>;

struct PlaybackRequest {
  std::string call_id;
  std::int64_t channel_id = 0;
  std::string stream = "main";
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  double speed = 1.0;
};

struct PlaybackStatus {
  std::string call_id;
  std::int64_t channel_id = 0;
  std::string stream;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::int64_t position_ms = 0;
  double speed = 1.0;
  bool paused = false;
  bool active = false;
  bool finished = false;
  std::uint64_t frames_sent = 0;
  std::uint64_t keyframes_sent = 0;
  std::uint64_t bytes_sent = 0;
};

struct PlaybackControllerConfig {
  // Allowed forward rates. Values outside the range are rejected so a platform
  // typo cannot turn into a burst of buffered media.
  double min_speed = 0.25;
  double max_speed = 16.0;
  // Media sent before real-time pacing starts.
  std::int64_t startup_burst_ms = 500;
  // Maximum bytes scanned forward while looking for a key frame on a seek.
  std::int64_t keyframe_scan_bytes = 8 * 1024 * 1024;
};

class PlaybackController {
public:
  using Sender =
      std::function<bool(const std::string& call_id, const std::uint8_t* ps, std::size_t size,
                         std::uint32_t timestamp_90k, bool keyframe, std::string* error)>;
  using FinishedFn = std::function<void(const std::string& call_id, const std::string& reason)>;

  PlaybackController(SegmentSource source, SegmentReadFn reader, Sender sender,
                     PlaybackControllerConfig config = {});
  ~PlaybackController();

  PlaybackController(const PlaybackController&) = delete;
  PlaybackController& operator=(const PlaybackController&) = delete;

  // Starts a reader thread. Returns false for an empty range or a duplicate
  // Call-ID. A missing recording is reported asynchronously through Finished().
  bool Start(const PlaybackRequest& request, std::string* error = nullptr);

  void set_finished_callback(FinishedFn callback);

  // Scale > 0 changes the forward rate; 0 pauses. Negative (reverse) playback
  // is rejected. These calls are safe from the SIP loop thread.
  bool SetSpeed(const std::string& call_id, double speed, std::string* error = nullptr);
  bool Pause(const std::string& call_id, bool paused, std::string* error = nullptr);
  // Jumps to `position_ms`; the reader restarts at the nearest key frame. A
  // paused session stays paused after the seek.
  bool Seek(const std::string& call_id, std::int64_t position_ms, std::string* error = nullptr);

  // Stops and joins the reader. Safe to call from the loop thread; the reader
  // polls its stop flag every 10 ms.
  void Stop(const std::string& call_id);
  void StopAll();

  bool Snapshot(const std::string& call_id, PlaybackStatus* status) const;
  std::vector<std::string> call_ids() const;
  std::size_t size() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Finds a key-frame-aligned TS byte offset near `approximate`. Exposed for the
// tests and for diagnostics; returns 0 when no suitable packet is found.
std::int64_t FindTsKeyframeOffset(const SegmentReadFn& reader, const std::string& path,
                                  std::int64_t approximate, std::int64_t file_size,
                                  std::int64_t scan_limit);

} // namespace uvnvr::gb28181
