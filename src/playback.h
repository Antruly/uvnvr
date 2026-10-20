#pragma once

#include <cstdint>
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
  PlaybackService(uvcpp::uvcpp_web_app* web, uvcpp::uvcpp_db_pool* db);
  ~PlaybackService();

  PlaybackService(const PlaybackService&) = delete;
  PlaybackService& operator=(const PlaybackService&) = delete;

  void Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id, const std::string& stream,
            int64_t start_ms, int64_t end_ms);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
