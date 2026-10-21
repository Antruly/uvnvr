#pragma once

#include <cstdint>
#include <db/uvcpp_db_pool.h>
#include <memory>
#include <string>
#include <vector>

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

  // Starts or extends an event clip for every stream the channel's active
  // event plan covers. Called for motion/person/tamper/intrusion alerts.
  void OnEvent(int64_t channel_id);

  // Segment currently being written. Playback uses part_path to follow a live
  // recording before it is finalized and indexed.
  struct ActiveSegment {
    int64_t channel_id = 0;
    std::string stream;
    std::string part_path;
    std::string final_path;
    int64_t start_ms = 0;
    int64_t bytes = 0;
    std::string codec;
    std::string record_type;
    int64_t volume_id = 0;
  };
  std::vector<ActiveSegment> ActiveSegments();

  // Active recorder/segment snapshots for the system UI.
  std::string StatusJson();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
