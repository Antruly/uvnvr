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
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
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
    if (h265_type == 19 || h265_type == 20 || h265_type == 32 || h265_type == 33)
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
    if (h264_type == 5 || h265_type == 19 || h265_type == 20) {
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
    std::ofstream file;
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

    Recorder(Impl* owner_in, int64_t channel, std::string stream_kind, VolumeInfo vol,
             int64_t minutes, int64_t size_mb, std::string mode)
        : owner(owner_in), channel_id(channel), stream(std::move(stream_kind)),
          volume(std::move(vol)), segment_minutes(std::max<int64_t>(1, minutes)),
          segment_size_mb(std::max<int64_t>(16, size_mb)), segment_mode(std::move(mode)) {}

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
        return;
      }
      Segment value;
      value.volume_id = volume.id;
      value.channel_id = channel_id;
      value.channel_name = std::to_string(channel_id);
      value.stream = stream;
      value.start_ms = now_ms;
      value.limit_bytes = segment_size_mb * 1024 * 1024;
      value.limit_ms = segment_minutes * 60 * 1000;
      value.seq = static_cast<int64_t>(now_ms);
      value.codec = current_codec;
      const std::string base = std::to_string(channel_id) + "_" + stream + "_" +
                               std::to_string(now_ms) + "_" + std::to_string(value.seq);
      value.path = (dir / (base + ".ts")).string();
      value.part_path = (dir / (base + ".ts.part")).string();
      value.file.open(value.part_path, std::ios::binary | std::ios::trunc);
      if (!value.file.is_open()) {
        std::fprintf(stderr, "[recording] cannot open %s\n", value.part_path.c_str());
        has_segment = false;
        return;
      }
      segment = std::move(value);
      has_segment = true;
      std::fprintf(stderr, "[recording] channel %lld %s -> %s\n",
                   static_cast<long long>(channel_id), stream.c_str(), segment.path.c_str());
      (void)start_offset;
    }

    void Finalize() {
      if (!has_segment)
        return;
      if (segment.file.is_open()) {
        segment.file.flush();
        segment.file.close();
      }
      std::error_code ec;
      std::filesystem::rename(segment.part_path, segment.path, ec);
      if (ec) {
        std::fprintf(stderr, "[recording] cannot finalize %s: %s\n", segment.part_path.c_str(),
                     ec.message().c_str());
        std::filesystem::remove(segment.part_path, ec);
        has_segment = false;
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
      params.add("timed");
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
      if (!segment.file.is_open())
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
            segment.file.write(reinterpret_cast<const char*>(chunk->data() + offset),
                               static_cast<std::streamsize>(split_at - offset));
            segment.bytes += static_cast<int64_t>(split_at - offset);
            segment.file.flush();
          }
          offset = split_at;
          Finalize();
          OpenSegment(NowMs());
          continue;
        }
        // No keyframe in this chunk: write the remainder and only force a split
        // when the segment has been waiting well past its target.
        const size_t count = chunk->size() - offset;
        segment.file.write(reinterpret_cast<const char*>(chunk->data() + offset),
                           static_cast<std::streamsize>(count));
        segment.bytes += static_cast<int64_t>(count);
        segment.file.flush();
        offset = chunk->size();
        if (due && now - segment.start_ms > segment.limit_ms + 5000) {
          Finalize();
          OpenSegment(NowMs());
        }
      }
    }

    void Run() {
      while (true) {
        std::shared_ptr<std::vector<uint8_t>> item;
        {
          std::unique_lock<std::mutex> lock(mutex);
          cv.wait(lock, [this] { return stopping.load() || !queue.empty(); });
          if (stopping.load() && queue.empty())
            break;
          item = std::move(queue.front());
          queue.pop_front();
        }
        WriteChunk(item);
      }
      Finalize();
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

  std::map<std::string, std::string> ActivePlans() {
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
            "AND mode IN ('timed','both') AND ("
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

  void SchedulerRun() {
    while (!stopping.load()) {
      VolumeInfo volume;
      const bool has_volume = ActiveVolume(&volume);
      const auto active = ActivePlans();
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

void RecordingService::OnTs(int64_t channel_id, const std::string& stream, const uint8_t* data,
                            size_t len) {
  std::shared_ptr<Impl::Recorder> recorder;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->recorders.find(Impl::Key(channel_id, stream));
    if (it == impl_->recorders.end())
      return;
    recorder = it->second;
  }
  recorder->Push(data, len);
}

} // namespace uvnvr
