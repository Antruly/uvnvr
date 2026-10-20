#pragma once

#include <cstdint>
#include <db/uvcpp_db_pool.h>
#include <memory>
#include <string>

namespace uvnvr {

// Plan-driven MPEG-TS segment recorder. It receives the same TS bytes that the
// live hub fans out, writes keyframe-aligned files and inserts SQLite indexes.
class RecordingService {
public:
  RecordingService(uvcpp::uvcpp_db_pool* db, std::string data_dir);
  ~RecordingService();

  RecordingService(const RecordingService&) = delete;
  RecordingService& operator=(const RecordingService&) = delete;

  void Start();
  void Stop();
  void OnTs(int64_t channel_id, const std::string& stream, const uint8_t* data, size_t len);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
