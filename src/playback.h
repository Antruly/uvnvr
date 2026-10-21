#pragma once

#include "recording.h"
#include "storage_cache.h"

#include <cstdint>
#include <functional>
#include <db/uvcpp_db_pool.h>
#include <memory>
#include <string>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>

namespace uvnvr {

// Streams recorded MPEG-TS segments over an established WebSocket. The caller
// owns the ticket and passes a validated channel/time range.
class PlaybackService {
public:
  PlaybackService(uvcpp::uvcpp_web_app* web, uvcpp::uvcpp_db_pool* db, size_t cache_bytes,
                  size_t readahead_bytes);
  ~PlaybackService();

  PlaybackService(const PlaybackService&) = delete;
  PlaybackService& operator=(const PlaybackService&) = delete;

  // `speed` is a positive playback rate (1.0 = real time). The stream is paced
  // by the segment's average bitrate so the client does not buffer the whole
  // requested range at once.
  void Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id, const std::string& stream,
            int64_t start_ms, int64_t end_ms, double speed);

  // Playback follows segments that are still being written by asking this
  // provider for the active recorders.
  using ActiveFn = std::function<std::vector<RecordingService::ActiveSegment>()>;
  void SetActiveProvider(ActiveFn provider);

  BlockCache::Stats CacheStats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
