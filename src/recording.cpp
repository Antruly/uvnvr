#include "recording.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <uv.h>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace uvnvr {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string GetSetting(uvcpp::uvcpp_db_pool* db, const std::string& key,
                       const std::string& fallback) {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(key);
  if (!uvcpp::uvcpp_db_ok(db->query("SELECT value FROM settings WHERE key = ?", params, &table)) ||
      table.empty()) {
    return fallback;
  }
  return table.at(0, "value").to_text();
}

int64_t GetSettingInt(uvcpp::uvcpp_db_pool* db, const std::string& key, int64_t fallback) {
  const std::string value = GetSetting(db, key, std::to_string(fallback));
  int64_t parsed = fallback;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    return fallback;
  return parsed;
}

std::string LocalDatePath(int64_t timestamp_ms) {
  const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y/%m/%d", &tm);
  return buffer;
}

std::string DetectCodec(const uint8_t* data, size_t len) {
  for (size_t i = 0; i + 4 <= len; ++i) {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    const uint8_t nal = data[i + 3];
    const int h264_type = nal & 0x1f;
    if (h264_type == 5 || h264_type == 7)
      return "h264";
    const int h265_type = (nal >> 1) & 0x3f;
    // Only IDR/CRA types are unambiguous. H.264 P-slices such as 0x41 also
    // decode to H.265 type 32 if SPS/VPS types are accepted, which used to
    // flap the segment codec between h264 and h265.
    if (h265_type == 19 || h265_type == 20 || h265_type == 21)
      return "h265";
  }
  return {};
}

std::vector<size_t> KeyframeOffsets(const uint8_t* data, size_t len) {
  std::vector<size_t> offsets;
  for (size_t i = 0; i + 4 <= len; ++i) {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    const uint8_t nal = data[i + 3];
    const int h264_type = nal & 0x1f;
    const int h265_type = (nal >> 1) & 0x3f;
    if (h264_type == 5 || h265_type == 19 || h265_type == 20 || h265_type == 21) {
      const size_t absolute = i - 3;
      offsets.push_back((absolute / 188) * 188);
    }
  }
  return offsets;
}

struct VolumeInfo {
  int64_t id = 0;
  std::string path;
  std::string mode;
  int64_t max_usage_pct = 90;
  int64_t reserve_gb = 5;
  int64_t retention_days = 30;
};

} // namespace

struct RecordingService::Impl {
  struct Segment {
    int64_t volume_id = 1;
    std::string channel_name;
    std::string stream;
    int64_t channel_id = 0;
    int64_t start_ms = 0;
    int64_t bytes = 0;
    int64_t limit_bytes = 0;
    int64_t limit_ms = 0;
    int64_t seq = 0;
    std::string codec;
    std::string path;
    std::string part_path;
    uv_file fd = -1;
  };

  struct Recorder {
    Impl* owner = nullptr;
    int64_t channel_id = 0;
    std::string stream;
    VolumeInfo volume;
    int64_t segment_minutes = 5;
    int64_t segment_size_mb = 256;
    std::string segment_mode = "time";

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> queue;
    std::atomic<bool> stopping{false};
    std::thread thread;
    Segment segment;
    bool has_segment = false;
    std::string current_codec;
    bool event_mode = false;
    std::string record_type = "timed";
    std::atomic<int64_t> event_deadline_ms{0};
    std::atomic<int64_t> event_hard_deadline_ms{0};
    std::atomic<bool> finished{false};
    int64_t event_start_ms = 0;
    bool event_start_used = false;
    int64_t batch_bytes = 8 * 1024 * 1024;
    int64_t batch_ms = 500;
    int64_t pending_bytes = 0;
    int64_t last_flush_ms = 0;
    uv_loop_t loop{};
    bool loop_ready = false;
    bool fs_done = false;
    int fs_result = 0;
    mutable std::mutex status_mutex;
    struct StatusSnapshot {
      bool active = false;
      std::string path;
      int64_t bytes = 0;
      int64_t start_ms = 0;
      int64_t volume_id = 0;
      std::string volume_path;
    } status;

    void RefreshStatus() {
      std::lock_guard<std::mutex> lock(status_mutex);
      status.active = has_segment;
      if (has_segment) {
        status.path = segment.path;
        status.bytes = segment.bytes;
        status.start_ms = segment.start_ms;
      } else {
        status.path.clear();
        status.bytes = 0;
        status.start_ms = 0;
      }
      status.volume_id = volume.id;
      status.volume_path = volume.path;
    }

    Recorder(Impl* owner_in, int64_t channel, std::string stream_kind, VolumeInfo vol,
             int64_t minutes, int64_t size_mb, std::string mode,
             std::string record_type_in = "timed")
        : owner(owner_in), channel_id(channel), stream(std::move(stream_kind)),
          volume(std::move(vol)), segment_minutes(std::max<int64_t>(1, minutes)),
          segment_size_mb(std::max<int64_t>(16, size_mb)), segment_mode(std::move(mode)),
          event_mode(record_type_in == "event"), record_type(std::move(record_type_in)) {
      batch_bytes =
          std::clamp<int64_t>(GetSettingInt(owner->db, "recording.batch_mb", 8) * 1024 * 1024,
                              64 * 1024, 256 * 1024 * 1024);
      batch_ms = std::clamp<int64_t>(GetSettingInt(owner->db, "recording.batch_ms", 500), 10, 5000);
      last_flush_ms = NowMs();
      RefreshStatus();
    }

    static void OnFs(uv_fs_t* req) {
      auto* self = static_cast<Recorder*>(req->data);
      self->fs_result = static_cast<int>(req->result);
      self->fs_done = true;
    }

    // Starts one uv_fs request and pumps the recorder's private loop until the
    // completion callback runs. libuv hands these requests to io_uring when the
    // kernel supports it and otherwise uses its threadpool.
    int CompleteFs(uv_fs_t* req, int submitted) {
      if (submitted < 0) {
        uv_fs_req_cleanup(req);
        return submitted;
      }
      while (!fs_done)
        uv_run(&loop, UV_RUN_ONCE);
      const int result = fs_result;
      uv_fs_req_cleanup(req);
      return result;
    }

    uv_file OpenFile(const std::string& path) {
      if (!loop_ready)
        return -1;
      uv_fs_t req;
      req.data = this;
      fs_done = false;
      fs_result = 0;
      const int rc =
          uv_fs_open(&loop, &req, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644, OnFs);
      return CompleteFs(&req, rc);
    }

    bool WriteFile(const uint8_t* data, size_t length) {
      size_t written = 0;
      while (written < length) {
        uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(const_cast<uint8_t*>(data + written)),
                                      static_cast<unsigned int>(length - written));
        uv_fs_t req;
        req.data = this;
        fs_done = false;
        fs_result = 0;
        const int rc = uv_fs_write(&loop, &req, segment.fd, &buffer, 1, -1, OnFs);
        const int result = CompleteFs(&req, rc);
        if (result <= 0)
          return false;
        written += static_cast<size_t>(result);
      }
      return true;
    }

    bool SyncFile() {
      if (!loop_ready || segment.fd < 0)
        return true;
      uv_fs_t req;
      req.data = this;
      fs_done = false;
      fs_result = 0;
      const int rc = uv_fs_fsync(&loop, &req, segment.fd, OnFs);
      return CompleteFs(&req, rc) == 0;
    }

    bool CloseFile(uv_file fd) {
      if (fd < 0)
        return true;
      if (!loop_ready)
        return false;
      uv_fs_t req;
      req.data = this;
      fs_done = false;
      fs_result = 0;
      const int rc = uv_fs_close(&loop, &req, fd, OnFs);
      return CompleteFs(&req, rc) == 0;
    }

    bool RenameFile(const std::string& from, const std::string& to) {
      if (!loop_ready)
        return false;
      uv_fs_t req;
      req.data = this;
      fs_done = false;
      fs_result = 0;
      const int rc = uv_fs_rename(&loop, &req, from.c_str(), to.c_str(), OnFs);
      return CompleteFs(&req, rc) == 0;
    }

    bool RemoveFile(const std::string& path) {
      if (!loop_ready)
        return false;
      uv_fs_t req;
      req.data = this;
      fs_done = false;
      fs_result = 0;
      const int rc = uv_fs_unlink(&loop, &req, path.c_str(), OnFs);
      return CompleteFs(&req, rc) == 0;
    }

    void MaybeFlush(bool force) {
      if (segment.fd < 0)
        return;
      const int64_t now = NowMs();
      if (!force && pending_bytes < batch_bytes && now - last_flush_ms < batch_ms)
        return;
      SyncFile();
      pending_bytes = 0;
      last_flush_ms = now;
    }

    // Event clips start with the buffered pre-roll and stop at the deadline.
    void BeginEvent(int64_t start_ms, int64_t deadline_ms, int64_t hard_deadline_ms) {
      event_start_ms = start_ms;
      event_deadline_ms = deadline_ms;
      event_hard_deadline_ms = hard_deadline_ms;
    }

    // A follow-up alert extends the current clip without exceeding its cap.
    void ExtendEvent(int64_t deadline_ms) {
      const int64_t hard = event_hard_deadline_ms.load();
      int64_t current = event_deadline_ms.load();
      const int64_t target = std::min(deadline_ms, hard);
      while (target > current && !event_deadline_ms.compare_exchange_weak(current, target)) {
      }
    }

    bool EventExpired(int64_t now_ms) const {
      return event_mode && now_ms >= event_deadline_ms.load();
    }

    ~Recorder() {
      Stop();
    }

    void Start() {
      stopping = false;
      thread = std::thread([this] { Run(); });
    }

    void Stop() {
      stopping = true;
      cv.notify_all();
      if (thread.joinable())
        thread.join();
    }

    void Push(const uint8_t* data, size_t len) {
      if (len == 0)
        return;
      auto copy = std::make_shared<std::vector<uint8_t>>(data, data + len);
      {
        std::lock_guard<std::mutex> lock(mutex);
        // A slow disk must not grow memory without bound. 256 MB of queued TS
        // is already far beyond any healthy write path.
        if (queue.size() > 4096) {
          queue.pop_front();
        }
        queue.push_back(std::move(copy));
      }
      cv.notify_one();
    }

    void OpenSegment(int64_t now_ms, int64_t start_offset = 0) {
      const std::filesystem::path dir = std::filesystem::path(volume.path) / "uvnvr" /
                                        std::to_string(channel_id) / LocalDatePath(now_ms);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::fprintf(stderr, "[recording] cannot create %s: %s\n", dir.string().c_str(),
                     ec.message().c_str());
        has_segment = false;
        RefreshStatus();
        return;
      }
      Segment value;
      value.volume_id = volume.id;
      value.channel_id = channel_id;
      value.channel_name = std::to_string(channel_id);
      value.stream = stream;
      // The first event segment begins with the pre-roll, so its index row
      // starts before the alert and matches the first buffered frame.
      value.start_ms = event_mode && !event_start_used && event_start_ms > 0
                           ? std::min<int64_t>(now_ms, event_start_ms)
                           : now_ms;
      value.limit_bytes = segment_size_mb * 1024 * 1024;
      value.limit_ms = segment_minutes * 60 * 1000;
      value.seq = static_cast<int64_t>(now_ms);
      value.codec = current_codec;
      const std::string base = std::to_string(channel_id) + "_" + stream +
                               (event_mode ? "_event" : "") + "_" + std::to_string(now_ms) + "_" +
                               std::to_string(value.seq);
      value.path = (dir / (base + ".ts")).string();
      value.part_path = (dir / (base + ".ts.part")).string();
      value.fd = OpenFile(value.part_path);
      if (value.fd < 0) {
        std::fprintf(stderr, "[recording] cannot open %s\n", value.part_path.c_str());
        has_segment = false;
        RefreshStatus();
        return;
      }
      segment = std::move(value);
      has_segment = true;
      event_start_used = true;
      pending_bytes = 0;
      last_flush_ms = NowMs();
      RefreshStatus();
      std::fprintf(stderr, "[recording] channel %lld %s -> %s\n",
                   static_cast<long long>(channel_id), stream.c_str(), segment.path.c_str());
      (void)start_offset;
    }

    void Finalize() {
      if (!has_segment)
        return;
      if (segment.fd >= 0) {
        SyncFile();
        CloseFile(segment.fd);
        segment.fd = -1;
      }
      // An event that produced no media (for example while the camera was
      // offline) should not leave an empty index row or file behind.
      if (event_mode && segment.bytes == 0) {
        RemoveFile(segment.part_path);
        has_segment = false;
        segment = Segment{};
        RefreshStatus();
        return;
      }
      if (!RenameFile(segment.part_path, segment.path)) {
        std::fprintf(stderr, "[recording] cannot finalize %s\n", segment.part_path.c_str());
        RemoveFile(segment.part_path);
        has_segment = false;
        segment = Segment{};
        RefreshStatus();
        return;
      }
      std::fprintf(stderr, "[recording] finalized %s (%lld bytes)\n", segment.path.c_str(),
                   static_cast<long long>(segment.bytes));
      const int64_t end_ms = NowMs();
      uvcpp::uvcpp_db_params params;
      params.add(segment.channel_id);
      params.add(segment.stream);
      params.add(segment.volume_id);
      params.add(segment.path);
      params.add(segment.start_ms);
      params.add(end_ms);
      params.add(std::max<int64_t>(0, end_ms - segment.start_ms));
      params.add(segment.bytes);
      params.add(segment.codec);
      params.add(record_type);
      params.add(end_ms / 1000);
      std::string error;
      if (!uvcpp::uvcpp_db_ok(owner->db->insert(
              "INSERT INTO recordings(channel_id, stream, volume_id, path, start_ms, end_ms, "
              "duration_ms, size_bytes, codec, record_type, created_at) "
              "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
              params, nullptr))) {
        std::fprintf(stderr, "recording index insert failed: %s\n",
                     owner->db->last_error().c_str());
      }
      has_segment = false;
      segment = Segment{};
      RefreshStatus();
    }

    void WriteChunk(const std::shared_ptr<std::vector<uint8_t>>& chunk) {
      const std::string detected = DetectCodec(chunk->data(), chunk->size());
      if (!detected.empty()) {
        if (!current_codec.empty() && detected != current_codec) {
          // A camera that switches H.264/H.265 must start a new file; the two
          // codecs cannot share an index row or a player session.
          if (has_segment)
            Finalize();
          current_codec = detected;
          OpenSegment(NowMs());
        } else if (current_codec.empty()) {
          current_codec = detected;
          if (has_segment)
            segment.codec = current_codec;
        }
      }
      if (!has_segment)
        OpenSegment(NowMs());
      if (segment.fd < 0)
        return;
      const auto keyframes = KeyframeOffsets(chunk->data(), chunk->size());
      size_t offset = 0;
      while (offset < chunk->size()) {
        if (!has_segment)
          OpenSegment(NowMs());
        const int64_t now = NowMs();
        const bool time_due = segment_mode == "time" && now - segment.start_ms >= segment.limit_ms;
        const bool size_due = segment_mode == "size" && segment.bytes >= segment.limit_bytes;
        const bool due = time_due || size_due;
        size_t split_at = chunk->size();
        bool found_keyframe = false;
        if (due) {
          for (const size_t keyframe : keyframes) {
            if (keyframe > offset) {
              split_at = keyframe;
              found_keyframe = true;
              break;
            }
          }
        }
        if (due && found_keyframe) {
          if (split_at > offset) {
            const size_t count = split_at - offset;
            if (!WriteFile(chunk->data() + offset, count))
              std::fprintf(stderr, "[recording] write failed for %s\n", segment.path.c_str());
            segment.bytes += static_cast<int64_t>(count);
            pending_bytes += static_cast<int64_t>(count);
            MaybeFlush(true);
          }
          offset = split_at;
          Finalize();
          OpenSegment(NowMs());
          continue;
        }
        // No keyframe in this chunk: write the remainder and only force a split
        // when the segment has been waiting well past its target.
        const size_t count = chunk->size() - offset;
        if (!WriteFile(chunk->data() + offset, count))
          std::fprintf(stderr, "[recording] write failed for %s\n", segment.path.c_str());
        segment.bytes += static_cast<int64_t>(count);
        pending_bytes += static_cast<int64_t>(count);
        MaybeFlush(false);
        offset = chunk->size();
        if (due && now - segment.start_ms > segment.limit_ms + 5000) {
          Finalize();
          OpenSegment(NowMs());
        }
      }
      RefreshStatus();
    }

    void Run() {
      if (uv_loop_init(&loop) == 0) {
        loop_ready = true;
      } else {
        std::fprintf(stderr, "[recording] cannot initialize the file I/O loop\n");
      }
      while (true) {
        std::shared_ptr<std::vector<uint8_t>> item;
        {
          std::unique_lock<std::mutex> lock(mutex);
          if (event_mode) {
            // Event clips must finalize on time even when the camera is quiet.
            cv.wait_for(lock, std::chrono::milliseconds(250),
                        [this] { return stopping.load() || !queue.empty(); });
          } else {
            cv.wait(lock, [this] { return stopping.load() || !queue.empty(); });
          }
          if (stopping.load() && queue.empty())
            break;
          if (!queue.empty()) {
            item = std::move(queue.front());
            queue.pop_front();
          }
        }
        if (!item) {
          if (event_mode && EventExpired(NowMs()))
            break;
          continue;
        }
        WriteChunk(item);
        if (event_mode && EventExpired(NowMs()))
          break;
      }
      finished = true;
      Finalize();
      if (loop_ready) {
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
        loop_ready = false;
      }
    }
  };

  Impl(uvcpp::uvcpp_db_pool* database, std::string data)
      : db(database), data_dir(std::move(data)) {}

  ~Impl() {
    Stop();
  }

  uvcpp::uvcpp_db_pool* db;
  std::string data_dir;
  std::atomic<bool> stopping{false};
  std::thread scheduler;
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Recorder>> recorders;
  int64_t last_cleanup_ms = 0;

  // Event clips are independent of the timed recorder map: a channel can have
  // a timed recording and an alert-triggered clip at the same time.
  std::mutex event_mutex;
  std::map<std::string, std::shared_ptr<Recorder>> event_recorders;
  std::map<std::string, std::string> event_streams; // key -> stream for active event plans
  struct BufferedChunk {
    int64_t at_ms = 0;
    std::shared_ptr<std::vector<uint8_t>> data;
  };
  std::map<std::string, std::deque<BufferedChunk>> preroll;
  int64_t preroll_window_ms = 10 * 1000;

  static std::string Key(int64_t channel, const std::string& stream) {
    return std::to_string(channel) + ":" + stream;
  }

  bool ActiveVolume(VolumeInfo* volume) {
    std::error_code absolute_ec;
    const std::string default_path =
        std::filesystem::absolute(std::filesystem::path(data_dir) / "recordings", absolute_ec)
            .string();
    uvcpp::uvcpp_db_table table;
    uvcpp::uvcpp_db_params params;
    params.add(default_path);
    // The auto-created volume under the data directory is a fallback. A volume
    // the user configured explicitly wins, and among those the newest wins.
    if (!uvcpp::uvcpp_db_ok(
            db->query("SELECT id, path, mode, max_usage_pct, reserve_gb, retention_days "
                      "FROM storage_volumes WHERE enabled = 1 "
                      "ORDER BY CASE WHEN path = ? THEN 1 ELSE 0 END, id DESC LIMIT 1",
                      params, &table)) ||
        table.empty()) {
      return false;
    }
    volume->id = table.at(0, "id").to_int64();
    volume->path = table.at(0, "path").to_text();
    volume->mode = table.at(0, "mode").to_text();
    volume->max_usage_pct = table.at(0, "max_usage_pct").to_int64();
    volume->reserve_gb = table.at(0, "reserve_gb").to_int64();
    volume->retention_days = table.at(0, "retention_days").to_int64();
    return !volume->path.empty();
  }

  void EnsureRecorder(int64_t channel_id, const std::string& stream, const VolumeInfo& volume) {
    const std::string key = Key(channel_id, stream);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = recorders.find(key);
    if (it != recorders.end())
      return;
    const int64_t minutes = GetSettingInt(db, "recording.segment_minutes", 5);
    const int64_t size_mb = GetSettingInt(db, "recording.segment_size_mb", 256);
    const std::string mode = GetSetting(db, "recording.segment_mode", "time");
    auto recorder =
        std::make_shared<Recorder>(this, channel_id, stream, volume, minutes, size_mb, mode);
    recorder->Start();
    recorders[key] = recorder;
  }

  void StopRecorder(const std::string& key) {
    std::shared_ptr<Recorder> recorder;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = recorders.find(key);
      if (it == recorders.end())
        return;
      recorder = it->second;
      recorders.erase(it);
    }
    recorder->Stop();
  }

  std::map<std::string, std::string> ActivePlanStreams(const std::string& modes) {
    std::map<std::string, std::string> result;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const int weekday = tm.tm_wday;
    const int minute = tm.tm_hour * 60 + tm.tm_min;
    uvcpp::uvcpp_db_params params;
    params.add(weekday);
    params.add(minute);
    params.add(minute);
    params.add(minute);
    params.add(minute);
    uvcpp::uvcpp_db_table table;
    if (!uvcpp::uvcpp_db_ok(db->query(
            "SELECT channel_id, stream FROM record_plans WHERE enabled = 1 AND weekday = ? "
            "AND mode IN (" +
                modes +
                ") AND ("
                "(start_min <= end_min AND ? >= start_min AND ? < end_min) OR "
                "(start_min > end_min AND (? >= start_min OR ? < end_min)))",
            params, &table))) {
      return result;
    }
    for (size_t i = 0; i < table.rows().size(); ++i) {
      const int64_t channel = table.at(i, "channel_id").to_int64();
      if (channel <= 0)
        continue;
      result[Key(channel, table.at(i, "stream").to_text())] = table.at(i, "stream").to_text();
    }
    return result;
  }

  std::map<std::string, std::string> ActivePlans() {
    return ActivePlanStreams("'timed','both'");
  }

  std::map<std::string, std::string> ActiveEventPlans() {
    return ActivePlanStreams("'event','both'");
  }

  // Buffers the most recent TS bytes for event pre-roll. Caller holds
  // event_mutex. A long gap means the stream reconnected, so old bytes must
  // not be stitched to the new connection.
  void BufferPrerollLocked(const std::string& key, const uint8_t* data, size_t len) {
    auto& buffer = preroll[key];
    const int64_t now = NowMs();
    if (preroll_window_ms <= 0) {
      buffer.clear();
      return;
    }
    if (!buffer.empty() && now - buffer.back().at_ms > 2000)
      buffer.clear();
    auto chunk = std::make_shared<std::vector<uint8_t>>(data, data + len);
    buffer.push_back(BufferedChunk{now, std::move(chunk)});
    const int64_t cutoff = now - preroll_window_ms;
    while (!buffer.empty() && buffer.front().at_ms < cutoff)
      buffer.pop_front();
  }

  void OnEvent(int64_t channel_id) {
    if (channel_id <= 0 || GetSettingInt(db, "event.enabled", 1) == 0)
      return;
    const int64_t now = NowMs();
    const int64_t post_ms =
        std::clamp<int64_t>(GetSettingInt(db, "event.post_seconds", 30), 1, 3600) * 1000;
    const int64_t max_ms = std::max(
        post_ms, std::clamp<int64_t>(GetSettingInt(db, "event.max_seconds", 300), 1, 3600) * 1000);
    // Query the plans directly: a plan created a moment before the alert must
    // still start a clip, even if the scheduler has not refreshed its snapshot.
    std::vector<std::string> streams;
    const std::string prefix = std::to_string(channel_id) + ":";
    for (const auto& [key, stream] : ActiveEventPlans()) {
      if (key.compare(0, prefix.size(), prefix) == 0)
        streams.push_back(stream);
    }
    if (streams.empty())
      return;
    VolumeInfo volume;
    if (!ActiveVolume(&volume))
      return;
    const int64_t size_mb = GetSettingInt(db, "recording.segment_size_mb", 256);
    // One event produces one clip unless it reaches the configured maximum.
    const int64_t minutes = std::max<int64_t>(1, (max_ms + 59999) / 60000);
    for (const auto& stream : streams) {
      const std::string key = Key(channel_id, stream);
      std::lock_guard<std::mutex> lock(event_mutex);
      auto existing = event_recorders.find(key);
      if (existing != event_recorders.end()) {
        if (!existing->second->finished.load()) {
          existing->second->ExtendEvent(now + post_ms);
          continue;
        }
        // The clip ended but the scheduler has not pruned it yet. Start a new
        // clip instead of extending a recorder whose thread already exited.
        event_recorders.erase(existing);
      }
      auto recorder = std::make_shared<Recorder>(this, channel_id, stream, volume, minutes, size_mb,
                                                 "time", "event");
      int64_t start_ms = now;
      const auto buffered = preroll.find(key);
      if (buffered != preroll.end() && !buffered->second.empty()) {
        start_ms = buffered->second.front().at_ms;
        for (const auto& chunk : buffered->second)
          recorder->Push(chunk.data->data(), chunk.data->size());
      }
      recorder->BeginEvent(start_ms, now + post_ms, now + max_ms);
      // Insert before starting so live chunks cannot outrun the pre-roll.
      event_recorders[key] = recorder;
      recorder->Start();
    }
  }

  void Cleanup() {
    const int64_t now = NowMs();
    if (now - last_cleanup_ms < 60 * 1000)
      return;
    last_cleanup_ms = now;
    uvcpp::uvcpp_db_table volumes;
    if (!uvcpp::uvcpp_db_ok(
            db->query("SELECT id, path, mode, max_usage_pct, reserve_gb, retention_days "
                      "FROM storage_volumes WHERE enabled = 1",
                      {}, &volumes))) {
      return;
    }
    for (size_t i = 0; i < volumes.rows().size(); ++i) {
      const int64_t id = volumes.at(i, "id").to_int64();
      const std::string path = volumes.at(i, "path").to_text();
      const std::string mode = volumes.at(i, "mode").to_text();
      if (path.empty())
        continue;

      // Retention applies to every volume; 0 means unlimited.
      const int64_t retention_days = volumes.at(i, "retention_days").to_int64();
      if (retention_days > 0) {
        const int64_t cutoff = now - retention_days * 24LL * 60 * 60 * 1000;
        while (true) {
          uvcpp::uvcpp_db_table expired;
          uvcpp::uvcpp_db_params params;
          params.add(id);
          params.add(cutoff);
          if (!uvcpp::uvcpp_db_ok(
                  db->query("SELECT id, path FROM recordings WHERE volume_id = ? AND end_ms < ? "
                            "ORDER BY start_ms ASC LIMIT 100",
                            params, &expired)) ||
              expired.rows().empty()) {
            break;
          }
          for (size_t row = 0; row < expired.rows().size(); ++row) {
            const int64_t recording_id = expired.at(row, "id").to_int64();
            const std::string file = expired.at(row, "path").to_text();
            std::error_code ec;
            std::filesystem::remove(file, ec);
            uvcpp::uvcpp_db_params del;
            del.add(recording_id);
            db->execute("DELETE FROM recordings WHERE id = ?", del, nullptr);
          }
          if (expired.rows().size() < 100)
            break;
        }
      }

      if (mode != "loop")
        continue;
      const int64_t max_pct = volumes.at(i, "max_usage_pct").to_int64();
#ifdef _WIN32
      ULARGE_INTEGER free_bytes{};
      ULARGE_INTEGER total_bytes{};
      if (!GetDiskFreeSpaceExA(path.c_str(), &free_bytes, &total_bytes, nullptr))
        continue;
      const uint64_t total = total_bytes.QuadPart;
      const uint64_t free = free_bytes.QuadPart;
#else
      struct statvfs stat {};
      if (statvfs(path.c_str(), &stat) != 0)
        continue;
      const uint64_t total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
      const uint64_t free = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
#endif
      if (total == 0)
        continue;
      const double used_pct = 100.0 * (1.0 - static_cast<double>(free) / total);
      if (used_pct < static_cast<double>(max_pct))
        continue;
      uvcpp::uvcpp_db_table rows;
      uvcpp::uvcpp_db_params params;
      params.add(id);
      if (!uvcpp::uvcpp_db_ok(db->query(
              "SELECT id, path FROM recordings WHERE volume_id = ? ORDER BY start_ms ASC LIMIT 20",
              params, &rows))) {
        continue;
      }
      for (size_t row = 0; row < rows.rows().size(); ++row) {
        const int64_t recording_id = rows.at(row, "id").to_int64();
        const std::string file = rows.at(row, "path").to_text();
        std::error_code ec;
        std::filesystem::remove(file, ec);
        uvcpp::uvcpp_db_params del;
        del.add(recording_id);
        db->execute("DELETE FROM recordings WHERE id = ?", del, nullptr);
      }
    }
  }

  std::string StatusJson() {
    VolumeInfo volume;
    const bool has_volume = ActiveVolume(&volume);
    nlohmann::json items = nlohmann::json::array();
    const auto append = [&items](const std::map<std::string, std::shared_ptr<Recorder>>& map) {
      for (const auto& [key, recorder] : map) {
        Recorder::StatusSnapshot snapshot;
        {
          std::lock_guard<std::mutex> status_lock(recorder->status_mutex);
          snapshot = recorder->status;
        }
        items.push_back(nlohmann::json{
            {"channelId", recorder->channel_id},
            {"stream", recorder->stream},
            {"recordType", recorder->record_type},
            {"active", snapshot.active},
            {"path", snapshot.path},
            {"bytes", snapshot.bytes},
            {"startMs", snapshot.start_ms},
            {"volumeId", snapshot.volume_id},
            {"volumePath", snapshot.volume_path},
            {"untilMs", recorder->event_mode ? recorder->event_deadline_ms.load() : 0}});
      }
    };
    {
      std::lock_guard<std::mutex> lock(mutex);
      append(recorders);
    }
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      append(event_recorders);
    }
    nlohmann::json result{{"recordings", std::move(items)},
                          {"volume", has_volume ? nlohmann::json{{"id", volume.id},
                                                                 {"path", volume.path},
                                                                 {"mode", volume.mode}}
                                                : nlohmann::json::object()}};
    return result.dump();
  }

  void SchedulerRun() {
    while (!stopping.load()) {
      VolumeInfo volume;
      const bool has_volume = ActiveVolume(&volume);
      const auto active = ActivePlans();
      const auto event_active = GetSettingInt(db, "event.enabled", 1) != 0
                                    ? ActiveEventPlans()
                                    : std::map<std::string, std::string>{};
      const int64_t preroll_seconds =
          std::clamp<int64_t>(GetSettingInt(db, "event.pre_seconds", 10), 0, 60);
      std::vector<std::shared_ptr<Recorder>> expired_events;
      {
        std::lock_guard<std::mutex> lock(event_mutex);
        event_streams = event_active;
        preroll_window_ms = preroll_seconds * 1000;
        for (auto it = preroll.begin(); it != preroll.end();) {
          if (event_active.find(it->first) == event_active.end())
            it = preroll.erase(it);
          else
            ++it;
        }
        for (auto it = event_recorders.begin(); it != event_recorders.end();) {
          if (it->second->finished.load()) {
            expired_events.push_back(it->second);
            it = event_recorders.erase(it);
          } else {
            ++it;
          }
        }
      }
      for (auto& expired : expired_events)
        expired->Stop();
      if (has_volume) {
        for (const auto& [key, stream] : active) {
          const size_t colon = key.find(':');
          if (colon == std::string::npos)
            continue;
          int64_t channel = 0;
          const std::string id = key.substr(0, colon);
          std::from_chars(id.data(), id.data() + id.size(), channel);
          if (channel > 0)
            EnsureRecorder(channel, stream, volume);
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = recorders.begin(); it != recorders.end();) {
          if (active.find(it->first) == active.end()) {
            auto recorder = it->second;
            it = recorders.erase(it);
            recorder->Stop();
          } else {
            ++it;
          }
        }
      }
      Cleanup();
      for (int i = 0; i < 50 && !stopping.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  void Start() {
    stopping = false;
    scheduler = std::thread([this] { SchedulerRun(); });
  }

  void Stop() {
    stopping = true;
    if (scheduler.joinable())
      scheduler.join();
    std::map<std::string, std::shared_ptr<Recorder>> copy;
    {
      std::lock_guard<std::mutex> lock(mutex);
      copy.swap(recorders);
    }
    for (auto& [key, recorder] : copy)
      recorder->Stop();
    std::map<std::string, std::shared_ptr<Recorder>> event_copy;
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      event_copy.swap(event_recorders);
    }
    for (auto& [key, recorder] : event_copy)
      recorder->Stop();
  }
};

RecordingService::RecordingService(uvcpp::uvcpp_db_pool* db, std::string data_dir)
    : impl_(std::make_unique<Impl>(db, std::move(data_dir))) {}

RecordingService::~RecordingService() = default;

void RecordingService::Start() {
  impl_->Start();
}

void RecordingService::Stop() {
  impl_->Stop();
}

void RecordingService::OnEvent(int64_t channel_id) {
  impl_->OnEvent(channel_id);
}

std::string RecordingService::StatusJson() {
  return impl_->StatusJson();
}

void RecordingService::OnTs(int64_t channel_id, const std::string& stream, const uint8_t* data,
                            size_t len) {
  const std::string key = Impl::Key(channel_id, stream);
  std::shared_ptr<Impl::Recorder> recorder;
  std::shared_ptr<Impl::Recorder> event_recorder;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->recorders.find(key);
    if (it != impl_->recorders.end())
      recorder = it->second;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->event_mutex);
    if (impl_->event_streams.find(key) != impl_->event_streams.end())
      impl_->BufferPrerollLocked(key, data, len);
    const auto it = impl_->event_recorders.find(key);
    if (it != impl_->event_recorders.end())
      event_recorder = it->second;
  }
  if (recorder)
    recorder->Push(data, len);
  if (event_recorder)
    event_recorder->Push(data, len);
}

} // namespace uvnvr
