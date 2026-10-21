#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr {

struct ExportJob {
  // Segment files in playback order.
  std::vector<std::string> segments;
  // Wall-clock time of the first byte of the first segment.
  int64_t first_segment_start_ms = 0;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  std::string ffmpeg_path;
  std::string work_dir;
  std::string output_path;
  uint64_t max_input_bytes = 4ULL * 1024 * 1024 * 1024;
};

// Concatenates the selected MPEG-TS segments and remuxes the requested time
// range to a fast-start MP4 with the bundled FFmpeg. Runs synchronously; call
// it from a worker thread. The caller owns `work_dir` and is responsible for
// removing it, including on failure.
bool ExportMp4(const ExportJob& job, std::string* error);

} // namespace uvnvr
