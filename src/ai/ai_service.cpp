#include "ai_service.h"

#include "ai_common.h"
#include "ai_engine.h"
#include "ai_rules.h"
#include "ai_event_json.h"
#include "media.h"
#include "plate_post.h"
#include "snapshot_quota.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_value.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

namespace uvnvr {
namespace {

// Visible in `ps -L` / /proc/<pid>/task/*/comm, which makes it obvious that no
// AI threads exist while the feature is disabled.
void NameCurrentThread(const char* name) {
#ifdef __linux__
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

using json = nlohmann::json;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::tm LocalTime(int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm value{};
#ifdef _WIN32
  localtime_s(&value, &seconds);
#else
  localtime_r(&seconds, &value);
#endif
  return value;
}

std::string DayStamp(int64_t epoch_ms) {
  const std::tm value = LocalTime(epoch_ms);
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d", &value);
  return buffer;
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

std::vector<std::string> SplitList(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  for (const char c : text) {
    if (c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty())
    out.push_back(current);
  return out;
}

bool ParseBoolText(const std::string& text, bool fallback) {
  const std::string value = Trim(text);
  if (value == "1" || value == "true" || value == "on" || value == "yes")
    return true;
  if (value == "0" || value == "false" || value == "off" || value == "no")
    return false;
  return fallback;
}

int64_t ParseIntText(const std::string& text, int64_t fallback) {
  if (text.empty())
    return fallback;
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0')
    return fallback;
  return static_cast<int64_t>(value);
}

double ParseDoubleText(const std::string& text, double fallback) {
  if (text.empty())
    return fallback;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0')
    return fallback;
  return value;
}

const char* kPlateTable = "ai_plate_events";
const char* kFaceTable = "ai_face_events";

std::string NormalizeType(const std::string& type) {
  return type == "face" ? "face" : (type == "plate" ? "plate" : "");
}

std::string SnapshotUrl(const std::string& type, int64_t id, const std::string& kind) {
  return "/api/v1/ai/events/" + std::to_string(id) + "/snapshot?type=" + type + "&kind=" + kind;
}

std::string SqlColumn(const uvcpp::uvcpp_db_table& table, size_t row, const char* column) {
  return table.rows()[row][column].to_text();
}

int64_t SqlInt(const uvcpp::uvcpp_db_table& table, size_t row, const char* column) {
  return table.rows()[row][column].to_int64();
}

double SqlDouble(const uvcpp::uvcpp_db_table& table, size_t row, const char* column) {
  return table.rows()[row][column].to_double();
}

std::vector<float> BlobToFloats(const std::string& bytes) {
  std::vector<float> out;
  if (bytes.size() < sizeof(float))
    return out;
  out.resize(bytes.size() / sizeof(float));
  std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
  return out;
}

std::string FloatsToBlob(const std::vector<float>& values) {
  std::string bytes(values.size() * sizeof(float), '\0');
  if (!values.empty())
    std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

std::vector<float> JsonToFloats(const json& value) {
  std::vector<float> out;
  if (!value.is_array())
    return out;
  out.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_number())
      return {};
    out.push_back(static_cast<float>(item.get<double>()));
  }
  return out;
}

std::set<int64_t> IdWatchlist(const std::string& text) {
  std::set<int64_t> out;
  for (const auto& item : SplitList(text)) {
    const int64_t id = ParseIntText(item, 0);
    if (id > 0)
      out.insert(id);
  }
  return out;
}

using SnapshotSealFn =
    std::function<bool(const std::string&, const std::string&, std::string*, std::string*)>;

bool WriteJpeg(const cv::Mat& image, const std::string& path, const SnapshotSealFn& seal) {
  if (image.empty())
    return false;
  std::vector<unsigned char> encoded;
  const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
  if (!cv::imencode(".jpg", image, encoded, params) || encoded.empty())
    return false;
  std::string stored(encoded.begin(), encoded.end());
  if (seal) {
    const std::string plaintext = stored;
    std::string error;
    if (!seal(path, plaintext, &stored, &error)) {
      std::fprintf(stderr, "ai: cannot seal snapshot %s: %s\n", path.c_str(), error.c_str());
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary);
  if (!out)
    return false;
  out.write(stored.data(), static_cast<std::streamsize>(stored.size()));
  return out.good();
}

int64_t FileSize(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error)
    return 0;
  return static_cast<int64_t>(size);
}

// Parses "x,y,w,h" percentages. Returns the full frame for empty/invalid input.
cv::Rect PercentRect(const cv::Mat& image, const std::string& roi) {
  const cv::Rect full(0, 0, image.cols, image.rows);
  if (roi.empty())
    return full;
  int values[4] = {0, 0, 0, 0};
  size_t index = 0;
  std::string current;
  for (const char c : roi) {
    if (c == ',') {
      if (index >= 4)
        return full;
      values[index++] = static_cast<int>(ParseIntText(Trim(current), 0));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (index != 3)
    return full;
  values[index] = static_cast<int>(ParseIntText(Trim(current), 0));
  const int x = values[0];
  const int y = values[1];
  const int w = values[2];
  const int h = values[3];
  if (w <= 0 || h <= 0)
    return full;
  const int left = std::max(0, std::min(image.cols, image.cols * x / 100));
  const int top = std::max(0, std::min(image.rows, image.rows * y / 100));
  const int right = std::max(0, std::min(image.cols, image.cols * (x + w) / 100));
  const int bottom = std::max(0, std::min(image.rows, image.rows * (y + h) / 100));
  if (right - left < 8 || bottom - top < 8)
    return full;
  return cv::Rect(left, top, right - left, bottom - top);
}

cv::Rect ClampBox(const cv::Mat& image, const cv::Rect& box) {
  const int x = std::max(0, std::min(image.cols, box.x));
  const int y = std::max(0, std::min(image.rows, box.y));
  const int right = std::max(x, std::min(image.cols, box.x + std::max(0, box.width)));
  const int bottom = std::max(y, std::min(image.rows, box.y + std::max(0, box.height)));
  return cv::Rect(x, y, right - x, bottom - y);
}

// Expands a detection by half of its longest side on every edge, giving a
// crop of at least four times the box area, then clamps to the frame.
cv::Rect ExpandedBox(const cv::Mat& image, const cv::Rect& box) {
  if (box.width <= 0 || box.height <= 0)
    return cv::Rect();
  const int padding = std::max(4, std::max(box.width, box.height) / 2);
  return ClampBox(image, cv::Rect(box.x - padding, box.y - padding, box.width + 2 * padding,
                                  box.height + 2 * padding));
}

// Intersection over union of two detection boxes; 0 for empty or disjoint
// boxes. Used by the face dedup window.
double BoxIou(const cv::Rect& a, const cv::Rect& b) {
  if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0)
    return 0.0;
  const int left = std::max(a.x, b.x);
  const int top = std::max(a.y, b.y);
  const int right = std::min(a.x + a.width, b.x + b.width);
  const int bottom = std::min(a.y + a.height, b.y + b.height);
  if (right <= left || bottom <= top)
    return 0.0;
  const double intersection =
      static_cast<double>(right - left) * static_cast<double>(bottom - top);
  const double area_a = static_cast<double>(a.width) * static_cast<double>(a.height);
  const double area_b = static_cast<double>(b.width) * static_cast<double>(b.height);
  return intersection / (area_a + area_b - intersection);
}

struct Frame {
  cv::Mat bgr;
  int64_t channel_id = 0;
  std::string channel_name;
  int64_t ts_ms = 0;
  AiRuntimeConfig config;
  std::string roi;
};

class FrameQueue {
public:
  explicit FrameQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  void Push(Frame frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_)
        return;
      while (frames_.size() >= capacity_) {
        frames_.pop_front();
        dropped_.fetch_add(1);
      }
      frames_.push_back(std::move(frame));
    }
    ready_.notify_one();
  }

  bool Pop(Frame* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return stop_ || !frames_.empty(); });
    if (frames_.empty())
      return false;
    *out = std::move(frames_.front());
    frames_.pop_front();
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      frames_.clear();
    }
    ready_.notify_all();
  }

  uint64_t dropped() const {
    return dropped_.load();
  }

private:
  size_t capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Frame> frames_;
  bool stop_ = false;
  std::atomic<uint64_t> dropped_{0};
};

using FaceRecord = FaceLibraryEntry;

struct CaptureTask {
  int64_t channel_id = 0;
  std::string channel_name;
  std::string stream;
  bool plate = false;
  bool face = false;
  int interval_ms = 1000;
  std::string plate_roi;
  std::string face_roi;
};

} // namespace

struct AiService::Impl {
  explicit Impl(Options options) : opts(std::move(options)) {}
  ~Impl() {
    Stop();
  }

  Options opts;

  mutable std::mutex lifecycle_mutex;
  mutable std::mutex mutex; // settings_, channel_settings_, channels_
  AiSettings settings;
  std::map<int64_t, AiChannelSettings> channel_settings;
  std::map<int64_t, AiChannelInfo> channels;
  bool running = false;
  int active_plate_workers = 0;
  int active_face_workers = 0;

  std::atomic<bool> stop{false};
  std::vector<std::thread> capture_threads;
  std::vector<std::thread> plate_threads;
  std::vector<std::thread> face_threads;
  std::thread maintenance_thread;
  std::shared_ptr<FrameQueue> plate_queue;
  std::shared_ptr<FrameQueue> face_queue;

  std::atomic<uint64_t> plate_processed{0};
  std::atomic<uint64_t> face_processed{0};
  std::atomic<uint64_t> plate_failed{0};
  std::atomic<uint64_t> face_failed{0};
  std::atomic<uint64_t> plate_total_ms{0};
  std::atomic<uint64_t> face_total_ms{0};

  std::mutex dedup_mutex;
  std::map<std::pair<int64_t, std::string>, int64_t> plate_dedup;

  // Face dedup keeps the boxes that produced events, per channel. A still face
  // matches one of them while its box overlaps and the window has not elapsed.
  struct FaceDedupEntry {
    cv::Rect box;
    int64_t ts_seconds = 0;
  };
  std::mutex face_dedup_mutex;
  std::map<int64_t, std::vector<FaceDedupEntry>> face_dedup;
  std::atomic<uint64_t> face_deduped{0};

  // Quota gate shared by the plate and face workers.
  SnapshotQuota snapshot_quota;
  std::mutex snapshot_log_mutex;
  int64_t snapshot_log_last_ms = 0;

  mutable std::mutex faces_mutex;
  std::vector<FaceRecord> faces;

  mutable std::mutex stats_mutex;
  int64_t stats_last_ms = 0;
  uint64_t stats_last_plate = 0;
  uint64_t stats_last_face = 0;
  double stats_plate_fps = 0.0;
  double stats_face_fps = 0.0;

  // ---- settings helpers -------------------------------------------------

  std::string Setting(const std::string& key, const std::string& fallback) {
    if (opts.db == nullptr)
      return fallback;
    uvcpp::uvcpp_db_params params;
    params.add(key);
    uvcpp::uvcpp_db_table table;
    if (!uvcpp::uvcpp_db_ok(
            opts.db->query("SELECT value FROM settings WHERE key = ?", params, &table)) ||
        table.empty()) {
      return fallback;
    }
    return SqlColumn(table, 0, "value");
  }

  int64_t SettingInt(const std::string& key, int64_t fallback) {
    return ParseIntText(Setting(key, std::to_string(fallback)), fallback);
  }

  double SettingDouble(const std::string& key, double fallback) {
    return ParseDoubleText(Setting(key, std::to_string(fallback)), fallback);
  }

  bool SettingBool(const std::string& key, bool fallback) {
    return ParseBoolText(Setting(key, fallback ? "1" : "0"), fallback);
  }

  bool WriteSetting(const std::string& key, const std::string& value, std::string* error) {
    if (opts.db == nullptr) {
      if (error != nullptr)
        *error = "database is not available";
      return false;
    }
    uvcpp::uvcpp_db_params params;
    params.add(key);
    params.add(value);
    params.add(NowSeconds());
    const auto status = opts.db->execute(
        "INSERT INTO settings(key, value, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
        params);
    if (!uvcpp::uvcpp_db_ok(status)) {
      if (error != nullptr)
        *error = opts.db->last_error();
      return false;
    }
    return true;
  }

  // ---- loading ----------------------------------------------------------

  std::string MetaValue(const std::string& key) {
    if (opts.db == nullptr)
      return "";
    uvcpp::uvcpp_db_params params;
    params.add(key);
    uvcpp::uvcpp_db_table table;
    if (!uvcpp::uvcpp_db_ok(
            opts.db->query("SELECT value FROM system_meta WHERE key = ?", params, &table)) ||
        table.empty()) {
      return "";
    }
    return SqlColumn(table, 0, "value");
  }

  void WriteMetaValue(const std::string& key, const std::string& value) {
    if (opts.db == nullptr)
      return;
    uvcpp::uvcpp_db_params params;
    params.add(key);
    params.add(value);
    if (!uvcpp::uvcpp_db_ok(opts.db->execute(
            "INSERT INTO system_meta(key, value) VALUES(?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            params))) {
      std::fprintf(stderr, "ai: cannot persist %s: %s\n", key.c_str(),
                   opts.db->last_error().c_str());
    }
  }

  // Binds the quota gate to the current settings. The daily counter is seeded
  // from system_meta once per process so a restart resumes where it left off.
  void ConfigureSnapshotQuotaLocked() {
    SnapshotQuotaLimits limits;
    limits.max_per_day = settings.snapshot_max_per_day;
    limits.max_total_mb = settings.snapshot_max_total_mb;
    limits.min_free_mb = settings.snapshot_min_free_mb;
    const std::string root = (std::filesystem::path(opts.data_dir) / "ai").string();
    snapshot_quota.SetPersist([this](int64_t day, int64_t count) {
      WriteMetaValue("ai.snapshot.day", std::to_string(day));
      WriteMetaValue("ai.snapshot.count", std::to_string(count));
    });
    snapshot_quota.Configure(root, limits, NowMs());
    snapshot_quota.Seed(ParseIntText(MetaValue("ai.snapshot.day"), 0),
                        ParseIntText(MetaValue("ai.snapshot.count"), 0), NowMs());
  }

  void LoadSettingsLocked() {
    settings.enabled = SettingBool("ai.enabled", false);
    settings.plate_enabled = SettingBool("ai.plate.enabled", true);
    settings.face_enabled = SettingBool("ai.face.enabled", true);
    settings.interval_ms = static_cast<int>(
        std::max<int64_t>(250, std::min<int64_t>(10000, SettingInt("ai.intervalMs", 1000))));
    settings.queue_depth = static_cast<int>(
        std::max<int64_t>(1, std::min<int64_t>(64, SettingInt("ai.queueDepth", 8))));
    settings.plate_workers = static_cast<int>(
        std::max<int64_t>(1, std::min<int64_t>(16, SettingInt("ai.plateWorkers", 2))));
    settings.face_workers = static_cast<int>(
        std::max<int64_t>(1, std::min<int64_t>(16, SettingInt("ai.faceWorkers", 2))));
    settings.min_plate_confidence =
        std::max(0.0, std::min(1.0, SettingDouble("ai.minPlateConfidence", 0.6)));
    settings.min_face_quality =
        std::max(0.0, std::min(1.0, SettingDouble("ai.minFaceQuality", 0.3)));
    settings.face_match_threshold =
        std::max(0.0, std::min(1.0, SettingDouble("ai.face.matchThreshold", 0.75)));
    settings.snapshot_retention_days = static_cast<int>(
        std::max<int64_t>(1, std::min<int64_t>(3650, SettingInt("ai.snapshotRetentionDays", 30))));
    settings.snapshot_encrypt = SettingBool("ai.snapshot.encrypt", false);
    settings.snapshot_max_per_day = std::max<int64_t>(
        0, std::min<int64_t>(1000000, SettingInt("ai.snapshotMaxPerDay", 20000)));
    settings.snapshot_max_total_mb = std::max<int64_t>(
        0, std::min<int64_t>(10000000, SettingInt("ai.snapshotMaxTotalMb", 2048)));
    settings.snapshot_min_free_mb = std::max<int64_t>(
        0, std::min<int64_t>(10000000, SettingInt("ai.snapshotMinFreeMb", 2048)));
    settings.dedup_seconds = static_cast<int>(
        std::max<int64_t>(0, std::min<int64_t>(3600, SettingInt("ai.dedupSeconds", 10))));
    settings.face_dedup_seconds = static_cast<int>(
        std::max<int64_t>(0, std::min<int64_t>(3600, SettingInt("ai.faceDedupSeconds", 10))));
    settings.stub = SettingBool("ai.stub", false);
    std::string face_model_dir = Setting("ai.face.modelDir", "");
    if (face_model_dir.empty()) {
      face_model_dir =
          (std::filesystem::path(opts.data_dir) / "models" / "inspireface").string();
    } else if (std::filesystem::path(face_model_dir).is_relative()) {
      face_model_dir = (std::filesystem::path(opts.data_dir) / face_model_dir).string();
    }
    settings.face_model_dir = std::move(face_model_dir);
    settings.face_pixel_level = static_cast<int>(
        std::max<int64_t>(-1, std::min<int64_t>(1280, SettingInt("ai.face.pixelLevel", -1))));
    settings.plate_watchlist = Setting("ai.plate.watchlist", "");
    settings.plate_blacklist = Setting("ai.plate.blacklist", "");
    if (settings.plate_blacklist.empty()) {
      // Compatibility: before black/white lists existed the single watchlist
      // was the alert list. Keep those deployments alerting after upgrade.
      settings.plate_blacklist = settings.plate_watchlist;
    }
    settings.plate_whitelist = Setting("ai.plate.whitelist", "");
    settings.face_watchlist = Setting("ai.face.watchlist", "");
  }

  void LoadChannelsLocked() {
    channels.clear();
    channel_settings.clear();
    if (opts.db == nullptr)
      return;
    uvcpp::uvcpp_db_table table;
    if (uvcpp::uvcpp_db_ok(opts.db->query(
            "SELECT id, name, enabled, sub_enabled FROM channels ORDER BY id", {}, &table))) {
      for (size_t row = 0; row < table.rows().size(); ++row) {
        AiChannelInfo info;
        info.id = SqlInt(table, row, "id");
        info.name = SqlColumn(table, row, "name");
        info.enabled = SqlInt(table, row, "enabled") != 0;
        info.sub_enabled = SqlInt(table, row, "sub_enabled") != 0;
        channels[info.id] = std::move(info);
      }
    }
    uvcpp::uvcpp_db_table settings_table;
    if (uvcpp::uvcpp_db_ok(opts.db->query(
            "SELECT channel_id, plate_enabled, face_enabled, interval_ms, plate_roi, face_roi "
            "FROM ai_channel_settings",
            {}, &settings_table))) {
      for (size_t row = 0; row < settings_table.rows().size(); ++row) {
        AiChannelSettings cs;
        cs.channel_id = SqlInt(settings_table, row, "channel_id");
        cs.plate_enabled = SqlInt(settings_table, row, "plate_enabled") != 0;
        cs.face_enabled = SqlInt(settings_table, row, "face_enabled") != 0;
        cs.interval_ms = static_cast<int>(SqlInt(settings_table, row, "interval_ms"));
        cs.plate_roi = SqlColumn(settings_table, row, "plate_roi");
        cs.face_roi = SqlColumn(settings_table, row, "face_roi");
        channel_settings[cs.channel_id] = std::move(cs);
      }
    }
  }

  void LoadFaces() {
    std::vector<FaceRecord> loaded;
    if (opts.db != nullptr) {
      uvcpp::uvcpp_db_table table;
      if (uvcpp::uvcpp_db_ok(opts.db->query(
              "SELECT id, name, feature_blob FROM ai_faces WHERE feature_blob IS NOT NULL "
              "ORDER BY id",
              {}, &table))) {
        for (size_t row = 0; row < table.rows().size(); ++row) {
          FaceRecord record;
          record.id = SqlInt(table, row, "id");
          record.name = SqlColumn(table, row, "name");
          record.feature = BlobToFloats(table.rows()[row]["feature_blob"].bytes());
          if (!record.feature.empty())
            loaded.push_back(std::move(record));
        }
      }
    }
    std::lock_guard<std::mutex> lock(faces_mutex);
    faces = std::move(loaded);
  }

  AiRuntimeConfig RuntimeLocked() const {
    AiRuntimeConfig config;
    config.stub = settings.stub;
    config.min_plate_confidence = settings.min_plate_confidence;
    config.min_face_quality = settings.min_face_quality;
    config.face_match_threshold = settings.face_match_threshold;
    config.dedup_seconds = settings.dedup_seconds;
    config.face_dedup_seconds = settings.face_dedup_seconds;
    config.face_model_dir = settings.face_model_dir;
    config.face_pixel_level = settings.face_pixel_level;
    config.plate_watchlist = settings.plate_watchlist;
    config.plate_blacklist = settings.plate_blacklist;
    config.plate_whitelist = settings.plate_whitelist;
    config.face_watchlist = settings.face_watchlist;
    return config;
  }

  // ---- lifecycle --------------------------------------------------------

  void StartLocked() {
    std::vector<CaptureTask> tasks;
    AiRuntimeConfig runtime;
    int plate_workers = 0;
    int face_workers = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (running)
        return;
      LoadSettingsLocked();
      LoadChannelsLocked();
      LoadFaces();
      ConfigureSnapshotQuotaLocked();
      stop.store(false);
      // Counters stay cumulative across Reload() so callers can compare
      // before/after values; they start at zero for the service lifetime.
      stats_last_ms = 0;
      stats_last_plate = 0;
      stats_last_face = 0;
      stats_plate_fps = 0.0;
      stats_face_fps = 0.0;
      plate_queue.reset();
      face_queue.reset();
      active_plate_workers = 0;
      active_face_workers = 0;
      if (!settings.enabled)
        return;
      runtime = RuntimeLocked();
      for (const auto& [id, channel] : channels) {
        if (!channel.enabled)
          continue;
        const auto it = channel_settings.find(id);
        const bool plate =
            settings.plate_enabled && it != channel_settings.end() && it->second.plate_enabled;
        const bool face =
            settings.face_enabled && it != channel_settings.end() && it->second.face_enabled;
        if (!plate && !face)
          continue;
        CaptureTask task;
        task.channel_id = id;
        task.channel_name = channel.name;
        task.stream = channel.sub_enabled ? "sub" : "main";
        task.plate = plate;
        task.face = face;
        task.interval_ms =
            it->second.interval_ms > 0 ? it->second.interval_ms : settings.interval_ms;
        task.plate_roi = it->second.plate_roi;
        task.face_roi = it->second.face_roi;
        tasks.push_back(std::move(task));
      }
      if (tasks.empty())
        return;
      bool any_plate = false;
      bool any_face = false;
      for (const auto& task : tasks) {
        any_plate = any_plate || task.plate;
        any_face = any_face || task.face;
      }
      running = true;
      if (any_plate) {
        plate_queue = std::make_shared<FrameQueue>(static_cast<size_t>(settings.queue_depth));
        plate_workers = settings.plate_workers;
        active_plate_workers = plate_workers;
      }
      if (any_face) {
        face_queue = std::make_shared<FrameQueue>(static_cast<size_t>(settings.queue_depth));
        face_workers = settings.face_workers;
        active_face_workers = face_workers;
      }
    }

    const std::shared_ptr<FrameQueue> plate = plate_queue;
    const std::shared_ptr<FrameQueue> face = face_queue;
    for (int i = 0; i < plate_workers; ++i) {
      plate_threads.emplace_back([this, plate, runtime] { PlateWorker(plate, runtime); });
    }
    for (int i = 0; i < face_workers; ++i) {
      face_threads.emplace_back([this, face, runtime] { FaceWorker(face, runtime); });
    }
    for (const auto& task : tasks) {
      capture_threads.emplace_back(
          [this, task, runtime, plate, face] { CaptureLoop(task, runtime, plate, face); });
    }
    if (settings.snapshot_retention_days > 0) {
      maintenance_thread = std::thread([this] { MaintenanceLoop(); });
    }

    std::printf("ai: pipeline started (%zu capture timer(s), %d plate worker(s), %d face "
                "worker(s), stub=%s)\n",
                tasks.size(), plate_workers, face_workers, runtime.stub ? "on" : "off");
    std::fflush(stdout);
  }

  void StopLocked() {
    std::vector<std::thread> captures;
    std::vector<std::thread> plates;
    std::vector<std::thread> face_workers;
    std::thread maintenance;
    std::shared_ptr<FrameQueue> plate;
    std::shared_ptr<FrameQueue> face;
    bool was_running = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      was_running = running;
      running = false;
      active_plate_workers = 0;
      active_face_workers = 0;
      stop.store(true);
      captures.swap(capture_threads);
      plates.swap(plate_threads);
      face_workers.swap(face_threads);
      maintenance = std::move(maintenance_thread);
      plate = plate_queue;
      face = face_queue;
    }
    if (plate)
      plate->Stop();
    if (face)
      face->Stop();
    for (auto& thread : captures) {
      if (thread.joinable())
        thread.join();
    }
    for (auto& thread : plates) {
      if (thread.joinable())
        thread.join();
    }
    for (auto& thread : face_workers) {
      if (thread.joinable())
        thread.join();
    }
    if (maintenance.joinable())
      maintenance.join();
    // Workers are joined, so the day counter can be flushed safely.
    snapshot_quota.Flush();
    if (was_running) {
      std::printf("ai: pipeline stopped\n");
      std::fflush(stdout);
    }
  }

  void Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
    StartLocked();
  }

  void Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
    StopLocked();
  }

  void Reload() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex);
    StopLocked();
    StartLocked();
  }

  // ---- capture / workers ------------------------------------------------

  bool CaptureFrame(int64_t channel_id, const std::string& primary, cv::Mat* out,
                    std::string* error) {
    out->release();
    if (opts.media == nullptr) {
      if (error != nullptr)
        *error = "media service is not available";
      return false;
    }
    std::string jpeg;
    std::string capture_error;
    bool ok = opts.media->CaptureJpegFromStream(channel_id, primary, &jpeg, &capture_error);
    if (!ok && primary != "main") {
      jpeg.clear();
      capture_error.clear();
      ok = opts.media->CaptureJpegFromStream(channel_id, "main", &jpeg, &capture_error);
    }
    if (!ok || jpeg.empty()) {
      if (error != nullptr)
        *error = capture_error.empty() ? "capture failed" : capture_error;
      return false;
    }
    const std::vector<unsigned char> bytes(jpeg.begin(), jpeg.end());
    *out = cv::imdecode(bytes, cv::IMREAD_COLOR);
    if (out->empty()) {
      if (error != nullptr)
        *error = "captured JPEG could not be decoded";
      return false;
    }
    return true;
  }

  void CaptureLoop(const CaptureTask& task, AiRuntimeConfig runtime,
                   std::shared_ptr<FrameQueue> plate, std::shared_ptr<FrameQueue> face) {
    NameCurrentThread("ai-capture");
    int consecutive_failures = 0;
    while (!stop.load()) {
      const int64_t started = NowMs();
      cv::Mat image;
      std::string error;
      if (CaptureFrame(task.channel_id, task.stream, &image, &error)) {
        consecutive_failures = 0;
        Frame frame;
        frame.bgr = image;
        frame.channel_id = task.channel_id;
        frame.channel_name = task.channel_name;
        frame.ts_ms = NowMs();
        frame.config = runtime;
        if (task.plate && plate) {
          Frame copy = frame;
          copy.roi = task.plate_roi;
          plate->Push(std::move(copy));
        }
        if (task.face && face) {
          Frame copy = frame;
          copy.roi = task.face_roi;
          face->Push(std::move(copy));
        }
      } else if (++consecutive_failures == 1 || consecutive_failures % 60 == 0) {
        std::fprintf(stderr, "ai: channel %lld capture failed: %s\n",
                     static_cast<long long>(task.channel_id), error.c_str());
      }
      const int64_t elapsed = NowMs() - started;
      int64_t wait = static_cast<int64_t>(task.interval_ms) - elapsed;
      if (wait <= 0)
        wait = 10;
      for (int64_t slept = 0; slept < wait && !stop.load(); slept += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(50, wait - slept)));
    }
  }

  void PlateWorker(std::shared_ptr<FrameQueue> queue, AiRuntimeConfig runtime) {
    NameCurrentThread("ai-plate");
    AiConfig engine_config;
    engine_config.stub = runtime.stub;
    std::string error;
    std::unique_ptr<AiEngine> engine = CreatePlateEngine(engine_config, &error);
    if (!engine) {
      std::fprintf(stderr, "ai: cannot create the plate engine: %s\n", error.c_str());
    }
    Frame frame;
    while (queue->Pop(&frame)) {
      if (stop.load())
        break;
      const int64_t started = NowMs();
      if (engine) {
        int offset_x = 0;
        int offset_y = 0;
        const cv::Rect roi = PercentRect(frame.bgr, frame.roi);
        offset_x = roi.x;
        offset_y = roi.y;
        const cv::Mat input = frame.bgr(roi);
        std::vector<AiPlateResult> results;
        std::string detect_error;
        if (engine->DetectPlates(input, &results, &detect_error)) {
          for (const auto& result : results)
            HandlePlate(frame, result, offset_x, offset_y, runtime, engine->Name());
        } else {
          plate_failed.fetch_add(1);
          std::fprintf(stderr, "ai: plate detection failed: %s\n", detect_error.c_str());
        }
      }
      plate_processed.fetch_add(1);
      plate_total_ms.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, NowMs() - started)));
    }
  }

  void FaceWorker(std::shared_ptr<FrameQueue> queue, AiRuntimeConfig runtime) {
    NameCurrentThread("ai-face");
    AiConfig engine_config;
    engine_config.stub = runtime.stub;
    engine_config.model_dir = runtime.face_model_dir;
    engine_config.face_pixel_level = runtime.face_pixel_level;
    std::string error;
    std::unique_ptr<AiEngine> engine = CreateFaceEngine(engine_config, &error);
    if (!engine) {
      std::fprintf(stderr, "ai: cannot create the face engine: %s\n", error.c_str());
    }
    Frame frame;
    while (queue->Pop(&frame)) {
      if (stop.load())
        break;
      const int64_t started = NowMs();
      if (engine) {
        const cv::Rect roi = PercentRect(frame.bgr, frame.roi);
        const cv::Mat input = frame.bgr(roi);
        std::vector<AiFaceResult> results;
        std::string detect_error;
        if (engine->DetectFaces(input, &results, &detect_error)) {
          for (const auto& result : results)
            HandleFace(frame, result, roi.x, roi.y, runtime, engine->Name());
        } else {
          face_failed.fetch_add(1);
          std::fprintf(stderr, "ai: face detection failed: %s\n", detect_error.c_str());
        }
      }
      face_processed.fetch_add(1);
      face_total_ms.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, NowMs() - started)));
    }
  }

  // ---- event handling ---------------------------------------------------

  std::string ImageDirectory(int64_t ts_ms, int64_t channel_id) {
    std::filesystem::path directory =
        std::filesystem::path(opts.data_dir) / "ai" / DayStamp(ts_ms) / std::to_string(channel_id);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      std::fprintf(stderr, "ai: cannot create %s: %s\n", directory.string().c_str(),
                   error.message().c_str());
      return "";
    }
    return directory.string();
  }

  std::string UniqueBase(const std::string& directory, int64_t ts_ms, const std::string& suffix) {
    std::string base = std::to_string(ts_ms);
    int attempt = 0;
    while (attempt < 100) {
      const std::string candidate = base + suffix;
      const std::filesystem::path full =
          std::filesystem::path(directory) / (candidate + "_full.jpg");
      const std::filesystem::path crop =
          std::filesystem::path(directory) / (candidate + "_crop.jpg");
      std::error_code error;
      const bool exists =
          std::filesystem::exists(full, error) || std::filesystem::exists(crop, error);
      if (!exists)
        return candidate;
      ++attempt;
      base = std::to_string(ts_ms + attempt);
    }
    return std::to_string(ts_ms) + suffix;
  }

  void WarnSnapshotBlocked(SnapshotBlockReason reason, int64_t now_ms) {
    {
      std::lock_guard<std::mutex> lock(snapshot_log_mutex);
      if (snapshot_log_last_ms != 0 && now_ms - snapshot_log_last_ms < 60000)
        return;
      snapshot_log_last_ms = now_ms;
    }
    const int64_t free_bytes = snapshot_quota.FreeBytes();
    const std::string free_text =
        free_bytes < 0 ? "free space unknown"
                       : std::to_string(free_bytes / (1024 * 1024)) + " MB free";
    std::fprintf(stderr,
                 "ai: warning: snapshot write suppressed by %s (today %lld image(s), %lld MB in "
                 "the snapshot directory, %s); events are kept without images\n",
                 SnapshotBlockReasonName(reason),
                 static_cast<long long>(snapshot_quota.Today()),
                 static_cast<long long>(snapshot_quota.DirectoryBytes() / (1024 * 1024)),
                 free_text.c_str());
    std::fflush(stderr);
  }

  void SaveImages(const Frame& frame, const cv::Rect& box, const std::string& suffix,
                  const std::string& directory, std::string* full_path, std::string* crop_path) {
    full_path->clear();
    crop_path->clear();
    if (directory.empty())
      return;
    const int64_t now = NowMs();
    const std::string base = UniqueBase(directory, frame.ts_ms, suffix);
    const std::filesystem::path full = std::filesystem::path(directory) / (base + "_full.jpg");
    const std::filesystem::path crop = std::filesystem::path(directory) / (base + "_crop.jpg");

    SnapshotBlockReason blocked = SnapshotBlockReason::kNone;
    if (!snapshot_quota.Acquire(now, &blocked)) {
      WarnSnapshotBlocked(blocked, now);
      return;
    }
    if (WriteJpeg(frame.bgr, full.string(), opts.seal_snapshot)) {
      *full_path = full.string();
      snapshot_quota.Commit(FileSize(full.string()), now);
    } else {
      snapshot_quota.Abort();
    }

    const cv::Rect expanded = ExpandedBox(frame.bgr, box);
    if (expanded.width > 0 && expanded.height > 0) {
      if (!snapshot_quota.Acquire(now, &blocked)) {
        WarnSnapshotBlocked(blocked, now);
        return;
      }
      const cv::Mat cropped = frame.bgr(expanded);
      if (WriteJpeg(cropped, crop.string(), opts.seal_snapshot)) {
        *crop_path = crop.string();
        snapshot_quota.Commit(FileSize(crop.string()), now);
      } else {
        snapshot_quota.Abort();
      }
    }
  }

  bool InsertPlateEvent(int64_t channel_id, int64_t ts, const AiPlateResult& result,
                        const std::string& normalized, const std::string& engine,
                        int64_t* event_id) {
    if (opts.db == nullptr)
      return false;
    uvcpp::uvcpp_db_params params;
    params.add(channel_id);
    params.add(ts);
    params.add(result.text);
    params.add(normalized);
    params.add(static_cast<double>(result.confidence));
    params.add(result.color);
    params.add(result.x);
    params.add(result.y);
    params.add(result.w);
    params.add(result.h);
    params.add(std::string());
    params.add(std::string());
    params.add(engine);
    params.add(ts);
    uvcpp::uvcpp_db_value new_id;
    const auto status = opts.db->insert(
        "INSERT INTO ai_plate_events(channel_id, ts, plate, plate_norm, confidence, color, "
        "x, y, w, h, image_path, crop_path, engine, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, &new_id);
    if (!uvcpp::uvcpp_db_ok(status)) {
      std::fprintf(stderr, "ai: plate event insert failed: %s\n", opts.db->last_error().c_str());
      return false;
    }
    *event_id = new_id.to_int64();
    return *event_id > 0;
  }

  void UpdatePlatePaths(int64_t event_id, const std::string& full, const std::string& crop) {
    if (opts.db == nullptr || (full.empty() && crop.empty()))
      return;
    uvcpp::uvcpp_db_params params;
    params.add(full);
    params.add(crop);
    params.add(event_id);
    if (!uvcpp::uvcpp_db_ok(opts.db->execute(
            "UPDATE ai_plate_events SET image_path = ?, crop_path = ? WHERE id = ?", params))) {
      std::fprintf(stderr, "ai: plate image path update failed: %s\n",
                   opts.db->last_error().c_str());
    }
  }

  // Uses the capture timestamp rather than processing time: with a backlog a
  // frame can reach a worker long after it was grabbed, and the dedup window is
  // about how far apart the frames are, not when the model got to them.
  bool PlateIsDuplicate(int64_t channel_id, const std::string& normalized, int dedup_seconds,
                        int64_t frame_ts) {
    if (dedup_seconds <= 0)
      return false;
    std::lock_guard<std::mutex> lock(dedup_mutex);
    const auto key = std::make_pair(channel_id, normalized);
    const auto it = plate_dedup.find(key);
    if (it != plate_dedup.end() && std::llabs(frame_ts - it->second) < dedup_seconds)
      return true;
    plate_dedup[key] = frame_ts;
    // Bound the map so a busy camera cannot grow it without limit.
    if (plate_dedup.size() > 4096) {
      const int64_t now = NowSeconds();
      for (auto entry = plate_dedup.begin(); entry != plate_dedup.end();) {
        if (now - entry->second > static_cast<int64_t>(dedup_seconds) * 4)
          entry = plate_dedup.erase(entry);
        else
          ++entry;
      }
    }
    return false;
  }

  // A still face keeps producing detections: the same channel plus an
  // overlapping box (IoU > 0.5, centres within half a box width) inside the
  // window is the same face, not a new arrival. Suppressed detections do not
  // refresh the window, so an immobile face still yields one event per
  // ai.faceDedupSeconds instead of one per capture interval.
  bool FaceIsDuplicate(int64_t channel_id, const cv::Rect& box, int dedup_seconds,
                       int64_t frame_ts) {
    if (dedup_seconds <= 0)
      return false;
    std::lock_guard<std::mutex> lock(face_dedup_mutex);
    std::vector<FaceDedupEntry>& entries = face_dedup[channel_id];
    const int64_t window = dedup_seconds;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const FaceDedupEntry& entry) {
                                   return std::llabs(frame_ts - entry.ts_seconds) >= window;
                                 }),
                  entries.end());
    for (const FaceDedupEntry& entry : entries) {
      if (BoxIou(box, entry.box) <= 0.5)
        continue;
      const double center_x = box.x + box.width / 2.0;
      const double center_y = box.y + box.height / 2.0;
      const double other_x = entry.box.x + entry.box.width / 2.0;
      const double other_y = entry.box.y + entry.box.height / 2.0;
      const double distance = std::hypot(center_x - other_x, center_y - other_y);
      const double reach = std::max(box.width, entry.box.width) * 0.5;
      if (distance < reach)
        return true;
    }
    entries.push_back(FaceDedupEntry{box, frame_ts});
    // Bound the per-channel table; a busy frame with dozens of faces must not
    // grow it without limit.
    if (entries.size() > 256)
      entries.erase(entries.begin(), entries.end() - 256);
    return false;
  }

  void HandlePlate(const Frame& frame, const AiPlateResult& raw, int offset_x, int offset_y,
                   const AiRuntimeConfig& runtime, const char* engine_name) {
    if (raw.confidence < runtime.min_plate_confidence)
      return;
    const std::string normalized = NormalizePlate(raw.text);
    if (normalized.empty() || !IsValidPlate(normalized))
      return;
    if (PlateIsDuplicate(frame.channel_id, normalized, runtime.dedup_seconds, frame.ts_ms / 1000))
      return;

    AiPlateResult result = raw;
    result.text = normalized;
    result.x = raw.x + offset_x;
    result.y = raw.y + offset_y;
    result.w = raw.w;
    result.h = raw.h;
    const cv::Rect box = ClampBox(frame.bgr, cv::Rect(result.x, result.y, result.w, result.h));
    result.x = box.x;
    result.y = box.y;
    result.w = box.width;
    result.h = box.height;

    int64_t event_id = 0;
    const int64_t ts = frame.ts_ms / 1000;
    if (!InsertPlateEvent(frame.channel_id, ts, result, normalized, engine_name, &event_id))
      return;

    std::string full_path;
    std::string crop_path;
    SaveImages(frame, box, "_plate", ImageDirectory(frame.ts_ms, frame.channel_id), &full_path,
               &crop_path);
    UpdatePlatePaths(event_id, full_path, crop_path);

    const PlateListVerdict verdict =
        EvaluatePlateLists(normalized, runtime.plate_blacklist, runtime.plate_whitelist);
    const bool alert = verdict.ShouldAlert();

    json message{{"type", "ai.plate"},
                 {"ts", ts},
                 {"channel", json{{"id", frame.channel_id}, {"name", frame.channel_name}}},
                 {"plate", normalized},
                 {"confidence", result.confidence},
                 {"box", json{{"x", result.x}, {"y", result.y}, {"w", result.w}, {"h", result.h}}},
                 {"color", result.color},
                 {"engine", engine_name},
                 {"alert", alert},
                 {"list", verdict.MatchLabel()}};
    message["imageUrl"] = full_path.empty() ? "" : SnapshotUrl("plate", event_id, "full");
    message["cropUrl"] = crop_path.empty() ? "" : SnapshotUrl("plate", event_id, "crop");

    if (opts.event_sink) {
      AiEventFields fields;
      fields.id = event_id;
      fields.face = false;
      fields.channel_id = frame.channel_id;
      fields.channel_name = frame.channel_name;
      fields.ts = ts;
      fields.confidence = result.confidence;
      fields.plate = normalized;
      fields.plate_norm = normalized;
      fields.color = result.color;
      fields.x = result.x;
      fields.y = result.y;
      fields.w = result.w;
      fields.h = result.h;
      fields.engine = engine_name;
      fields.alert = alert;
      fields.image_url = message["imageUrl"].get<std::string>();
      fields.crop_url = message["cropUrl"].get<std::string>();
      opts.event_sink(AiEventJson(fields));
    }
    if (opts.broadcast)
      opts.broadcast(message);
    if (alert && opts.raise_alert) {
      opts.raise_alert(frame.channel_id, "ai_plate", "warning",
                       "AI 车牌黑名单预警: " + normalized, full_path, "plate", event_id);
    }
  }

  FaceMatchResult MatchFace(const std::vector<float>& feature, double threshold) {
    std::lock_guard<std::mutex> lock(faces_mutex);
    return MatchFaceFeature(faces, feature, threshold);
  }

  bool InsertFaceEvent(int64_t channel_id, int64_t ts, const AiFaceResult& result,
                       const std::string& engine, const FaceMatchResult& match, int64_t* event_id) {
    if (opts.db == nullptr)
      return false;
    uvcpp::uvcpp_db_params params;
    params.add(channel_id);
    params.add(ts);
    params.add(static_cast<double>(result.quality));
    params.add(static_cast<double>(result.detConfidence));
    params.add(result.x);
    params.add(result.y);
    params.add(result.w);
    params.add(result.h);
    params.add(std::string());
    params.add(std::string());
    params.add(uvcpp::uvcpp_db_value::blob(FloatsToBlob(result.feature)));
    params.add(engine);
    if (match.found)
      params.add(match.id);
    else
      params.add_null();
    if (match.found)
      params.add(match.score);
    else
      params.add_null();
    params.add(ts);
    uvcpp::uvcpp_db_value new_id;
    const auto status = opts.db->insert(
        "INSERT INTO ai_face_events(channel_id, ts, quality, confidence, x, y, w, h, image_path, "
        "crop_path, feature_blob, engine, matched_face_id, matched_score, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, &new_id);
    if (!uvcpp::uvcpp_db_ok(status)) {
      std::fprintf(stderr, "ai: face event insert failed: %s\n", opts.db->last_error().c_str());
      return false;
    }
    *event_id = new_id.to_int64();
    return *event_id > 0;
  }

  void UpdateFacePaths(int64_t event_id, const std::string& full, const std::string& crop) {
    if (opts.db == nullptr || (full.empty() && crop.empty()))
      return;
    uvcpp::uvcpp_db_params params;
    params.add(full);
    params.add(crop);
    params.add(event_id);
    if (!uvcpp::uvcpp_db_ok(opts.db->execute(
            "UPDATE ai_face_events SET image_path = ?, crop_path = ? WHERE id = ?", params))) {
      std::fprintf(stderr, "ai: face image path update failed: %s\n",
                   opts.db->last_error().c_str());
    }
  }

  void HandleFace(const Frame& frame, const AiFaceResult& raw, int offset_x, int offset_y,
                  const AiRuntimeConfig& runtime, const char* engine_name) {
    if (raw.quality < runtime.min_face_quality)
      return;
    AiFaceResult result = raw;
    result.x = raw.x + offset_x;
    result.y = raw.y + offset_y;
    const cv::Rect box = ClampBox(frame.bgr, cv::Rect(result.x, result.y, result.w, result.h));
    result.x = box.x;
    result.y = box.y;
    result.w = box.width;
    result.h = box.height;

    const int64_t ts = frame.ts_ms / 1000;
    if (FaceIsDuplicate(frame.channel_id, box, runtime.face_dedup_seconds, ts)) {
      face_deduped.fetch_add(1);
      return;
    }

    const FaceMatchResult match = MatchFace(result.feature, runtime.face_match_threshold);
    const bool alert = match.found && IdWatchlist(runtime.face_watchlist).count(match.id) > 0;

    int64_t event_id = 0;
    if (!InsertFaceEvent(frame.channel_id, ts, result, engine_name, match, &event_id))
      return;

    std::string full_path;
    std::string crop_path;
    SaveImages(frame, box, "_face", ImageDirectory(frame.ts_ms, frame.channel_id), &full_path,
               &crop_path);
    UpdateFacePaths(event_id, full_path, crop_path);

    json message{{"type", "ai.face"},
                 {"ts", ts},
                 {"channel", json{{"id", frame.channel_id}, {"name", frame.channel_name}}},
                 {"quality", result.quality},
                 {"confidence", result.detConfidence},
                 {"box", json{{"x", result.x}, {"y", result.y}, {"w", result.w}, {"h", result.h}}},
                 {"engine", engine_name},
                 {"alert", alert},
                 {"matchThreshold", runtime.face_match_threshold}};
    if (match.found) {
      message["matched"] = json{{"id", match.id}, {"name", match.name}, {"score", match.score}};
    } else {
      message["matched"] = nullptr;
    }
    message["imageUrl"] = full_path.empty() ? "" : SnapshotUrl("face", event_id, "full");
    message["cropUrl"] = crop_path.empty() ? "" : SnapshotUrl("face", event_id, "crop");

    if (opts.event_sink) {
      AiEventFields fields;
      fields.id = event_id;
      fields.face = true;
      fields.channel_id = frame.channel_id;
      fields.channel_name = frame.channel_name;
      fields.ts = ts;
      fields.confidence = result.detConfidence;
      fields.quality = result.quality;
      fields.x = result.x;
      fields.y = result.y;
      fields.w = result.w;
      fields.h = result.h;
      fields.engine = engine_name;
      fields.alert = alert;
      fields.matched = match.found;
      fields.matched_face_id = match.id;
      fields.matched_score = match.score;
      fields.matched_name = match.name;
      fields.image_url = message["imageUrl"].get<std::string>();
      fields.crop_url = message["cropUrl"].get<std::string>();
      opts.event_sink(AiEventJson(fields));
    }
    if (opts.broadcast)
      opts.broadcast(message);
    if (alert && opts.raise_alert) {
      opts.raise_alert(frame.channel_id, "ai_face", "warning", "AI 人脸预警: " + match.name,
                       full_path, "face", event_id);
    }
  }

  // ---- retention --------------------------------------------------------

  void RunRetention() {
    int retention_days = 30;
    {
      std::lock_guard<std::mutex> lock(mutex);
      retention_days = settings.snapshot_retention_days;
    }
    if (retention_days <= 0 || opts.db == nullptr)
      return;
    const int64_t cutoff = NowSeconds() - static_cast<int64_t>(retention_days) * 86400;
    for (const char* table : {kPlateTable, kFaceTable}) {
      uvcpp::uvcpp_db_params select_params;
      select_params.add(cutoff);
      uvcpp::uvcpp_db_table table_rows;
      const std::string select_sql =
          std::string("SELECT id, image_path, crop_path FROM ") + table + " WHERE ts < ?";
      if (!uvcpp::uvcpp_db_ok(opts.db->query(select_sql, select_params, &table_rows)))
        continue;
      for (size_t row = 0; row < table_rows.rows().size(); ++row) {
        for (const char* column : {"image_path", "crop_path"}) {
          const std::string path = SqlColumn(table_rows, row, column);
          if (path.empty())
            continue;
          std::error_code error;
          std::filesystem::remove(path, error);
        }
      }
      uvcpp::uvcpp_db_params delete_params;
      delete_params.add(cutoff);
      const std::string delete_sql = std::string("DELETE FROM ") + table + " WHERE ts < ?";
      if (!uvcpp::uvcpp_db_ok(opts.db->execute(delete_sql, delete_params))) {
        std::fprintf(stderr, "ai: retention delete failed for %s: %s\n", table,
                     opts.db->last_error().c_str());
      }
    }
    PruneEmptySnapshotDirectories();
  }

  // Retention removes files but leaves the <day>/<channel> directories behind.
  // Walk them bottom-up and reclaim the ones that are now empty.
  void PruneEmptySnapshotDirectories() {
    const std::filesystem::path root = std::filesystem::path(opts.data_dir) / "ai";
    std::error_code error;
    if (!std::filesystem::is_directory(root, error))
      return;
    std::vector<std::filesystem::path> directories;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
      std::error_code entry_error;
      if (it->is_directory(entry_error))
        directories.push_back(it->path());
      it.increment(error);
    }
    // Deepest first: a day directory can only become empty after its channel
    // directories have been removed.
    std::sort(directories.begin(), directories.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                return a.native().size() > b.native().size();
              });
    for (const auto& directory : directories) {
      std::error_code remove_error;
      if (!std::filesystem::is_empty(directory, remove_error) || remove_error)
        continue;
      std::filesystem::remove(directory, remove_error);
    }
    std::error_code root_error;
    if (std::filesystem::is_empty(root, root_error) && !root_error)
      std::filesystem::remove(root, root_error);
  }

  void MaintenanceLoop() {
    NameCurrentThread("ai-maint");
    // Clean once at startup (on this thread, so startup is not blocked), then
    // once per day.
    RunRetention();
    int64_t last_run = NowSeconds();
    while (!stop.load()) {
      for (int i = 0; i < 600 && !stop.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (stop.load())
        break;
      const int64_t now = NowSeconds();
      if (now - last_run >= 86400) {
        RunRetention();
        last_run = now;
      }
    }
  }

  // ---- query helpers ----------------------------------------------------

  json PlateItem(const uvcpp::uvcpp_db_table& table, size_t row) const {
    const int64_t id = SqlInt(table, row, "id");
    const std::string image_path = SqlColumn(table, row, "image_path");
    const std::string crop_path = SqlColumn(table, row, "crop_path");
    json item{{"id", id},
              {"type", "plate"},
              {"channelId", SqlInt(table, row, "channel_id")},
              {"channelName", SqlColumn(table, row, "channel_name")},
              {"ts", SqlInt(table, row, "ts")},
              {"plate", SqlColumn(table, row, "plate")},
              {"plateNorm", SqlColumn(table, row, "plate_norm")},
              {"confidence", SqlDouble(table, row, "confidence")},
              {"color", SqlColumn(table, row, "color")},
              {"box", json{{"x", SqlInt(table, row, "x")},
                           {"y", SqlInt(table, row, "y")},
                           {"w", SqlInt(table, row, "w")},
                           {"h", SqlInt(table, row, "h")}}},
              {"engine", SqlColumn(table, row, "engine")},
              {"createdAt", SqlInt(table, row, "created_at")}};
    item["imageUrl"] = image_path.empty() ? "" : SnapshotUrl("plate", id, "full");
    item["cropUrl"] = crop_path.empty() ? "" : SnapshotUrl("plate", id, "crop");
    return item;
  }

  json FaceItem(const uvcpp::uvcpp_db_table& table, size_t row) const {
    const int64_t id = SqlInt(table, row, "id");
    const std::string image_path = SqlColumn(table, row, "image_path");
    const std::string crop_path = SqlColumn(table, row, "crop_path");
    const int64_t matched_id = SqlInt(table, row, "matched_face_id");
    json item{{"id", id},
              {"type", "face"},
              {"channelId", SqlInt(table, row, "channel_id")},
              {"channelName", SqlColumn(table, row, "channel_name")},
              {"ts", SqlInt(table, row, "ts")},
              {"quality", SqlDouble(table, row, "quality")},
              {"confidence", SqlDouble(table, row, "confidence")},
              {"box", json{{"x", SqlInt(table, row, "x")},
                           {"y", SqlInt(table, row, "y")},
                           {"w", SqlInt(table, row, "w")},
                           {"h", SqlInt(table, row, "h")}}},
              {"engine", SqlColumn(table, row, "engine")},
              {"matchedFaceId", matched_id == 0 ? json(nullptr) : json(matched_id)},
              {"matchedScore", SqlDouble(table, row, "matched_score")},
              {"createdAt", SqlInt(table, row, "created_at")}};
    item["imageUrl"] = image_path.empty() ? "" : SnapshotUrl("face", id, "full");
    item["cropUrl"] = crop_path.empty() ? "" : SnapshotUrl("face", id, "crop");
    return item;
  }

  std::string EventSelectSql(const std::string& type) const {
    if (type == "plate") {
      return "SELECT e.*, c.name AS channel_name FROM ai_plate_events e "
             "LEFT JOIN channels c ON c.id = e.channel_id";
    }
    return "SELECT e.*, c.name AS channel_name FROM ai_face_events e "
           "LEFT JOIN channels c ON c.id = e.channel_id";
  }

  bool QueryEventsImpl(const std::string& type, int64_t channel_id, int64_t from, int64_t to,
                       const std::string& plate, int limit, int offset, json* out,
                       std::string* error) const {
    if (opts.db == nullptr) {
      if (error != nullptr)
        *error = "database is not available";
      return false;
    }
    std::string where = " WHERE 1=1";
    std::vector<uvcpp::uvcpp_db_value> values;
    if (channel_id > 0) {
      where += " AND e.channel_id = ?";
      values.push_back(uvcpp::uvcpp_db_value(channel_id));
    }
    if (from > 0) {
      where += " AND e.ts >= ?";
      values.push_back(uvcpp::uvcpp_db_value(from));
    }
    if (to > 0) {
      where += " AND e.ts <= ?";
      values.push_back(uvcpp::uvcpp_db_value(to));
    }
    if (type == "plate" && !plate.empty()) {
      const std::string normalized = NormalizePlate(plate);
      if (!normalized.empty()) {
        where += " AND (e.plate_norm = ? OR e.plate LIKE ?)";
        values.push_back(uvcpp::uvcpp_db_value(normalized));
        values.push_back(uvcpp::uvcpp_db_value("%" + plate + "%"));
      } else {
        where += " AND e.plate LIKE ?";
        values.push_back(uvcpp::uvcpp_db_value("%" + plate + "%"));
      }
    }

    const std::string base = EventSelectSql(type);
    uvcpp::uvcpp_db_params count_params;
    for (const auto& value : values)
      count_params.add(value);
    uvcpp::uvcpp_db_table count_table;
    const std::string count_sql = type == "plate"
                                      ? "SELECT COUNT(*) AS n FROM ai_plate_events e" + where
                                      : "SELECT COUNT(*) AS n FROM ai_face_events e" + where;
    if (!uvcpp::uvcpp_db_ok(opts.db->query(count_sql, count_params, &count_table))) {
      if (error != nullptr)
        *error = opts.db->last_error();
      return false;
    }
    const int64_t total = count_table.empty() ? 0 : SqlInt(count_table, 0, "n");

    const int safe_limit = std::max(1, std::min(limit, 500));
    const int safe_offset = std::max(0, offset);
    uvcpp::uvcpp_db_params params;
    for (const auto& value : values)
      params.add(value);
    params.add(safe_limit);
    params.add(safe_offset);
    uvcpp::uvcpp_db_table table;
    if (!uvcpp::uvcpp_db_ok(opts.db->query(
            base + where + " ORDER BY e.ts DESC, e.id DESC LIMIT ? OFFSET ?", params, &table))) {
      if (error != nullptr)
        *error = opts.db->last_error();
      return false;
    }
    json items = json::array();
    for (size_t row = 0; row < table.rows().size(); ++row)
      items.push_back(type == "plate" ? PlateItem(table, row) : FaceItem(table, row));
    *out = json{{"total", total}, {"items", items}};
    return true;
  }

  bool GetEventImpl(const std::string& type, int64_t id, json* out, std::string* error) const {
    if (opts.db == nullptr) {
      if (error != nullptr)
        *error = "database is not available";
      return false;
    }
    uvcpp::uvcpp_db_params params;
    params.add(id);
    uvcpp::uvcpp_db_table table;
    if (!uvcpp::uvcpp_db_ok(
            opts.db->query(EventSelectSql(type) + " WHERE e.id = ?", params, &table))) {
      if (error != nullptr)
        *error = opts.db->last_error();
      return false;
    }
    if (table.empty()) {
      if (error != nullptr)
        *error = "event not found";
      return false;
    }
    *out = type == "plate" ? PlateItem(table, 0) : FaceItem(table, 0);
    return true;
  }

  bool ReadSnapshotImpl(const std::string& type, int64_t id, const std::string& kind,
                        std::string* bytes, std::string* error) const {
    if (opts.db == nullptr) {
      if (error != nullptr)
        *error = "database is not available";
      return false;
    }
    uvcpp::uvcpp_db_params params;
    params.add(id);
    uvcpp::uvcpp_db_table table;
    const std::string sql = type == "plate"
                                ? "SELECT image_path, crop_path FROM ai_plate_events WHERE id = ?"
                                : "SELECT image_path, crop_path FROM ai_face_events WHERE id = ?";
    if (!uvcpp::uvcpp_db_ok(opts.db->query(sql, params, &table))) {
      if (error != nullptr)
        *error = opts.db->last_error();
      return false;
    }
    if (table.empty()) {
      if (error != nullptr)
        *error = "event not found";
      return false;
    }
    const std::string path =
        kind == "crop" ? SqlColumn(table, 0, "crop_path") : SqlColumn(table, 0, "image_path");
    if (path.empty()) {
      if (error != nullptr)
        *error = "snapshot is not available for this event";
      return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      if (error != nullptr)
        *error = "snapshot file is missing";
      return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *bytes = buffer.str();
    if (bytes->empty()) {
      if (error != nullptr)
        *error = "snapshot file is empty";
      return false;
    }
    if (opts.open_snapshot) {
      std::string plaintext;
      std::string open_error;
      if (!opts.open_snapshot(path, *bytes, &plaintext, &open_error)) {
        if (error != nullptr)
          *error = open_error.empty() ? "snapshot decryption failed" : open_error;
        return false;
      }
      *bytes = std::move(plaintext);
    }
    return true;
  }

  bool ChannelSettingsJsonLocked(int64_t channel_id, json* out, std::string* error) const {
    const auto channel = channels.find(channel_id);
    if (channel == channels.end()) {
      if (error != nullptr)
        *error = "channel not found";
      return false;
    }
    AiChannelSettings settings_value;
    const auto it = channel_settings.find(channel_id);
    if (it != channel_settings.end())
      settings_value = it->second;
    settings_value.channel_id = channel_id;
    const bool global_plate = settings.enabled && settings.plate_enabled;
    const bool global_face = settings.enabled && settings.face_enabled;
    const bool effective_plate =
        global_plate && channel->second.enabled && settings_value.plate_enabled;
    const bool effective_face =
        global_face && channel->second.enabled && settings_value.face_enabled;
    *out = json{{"channelId", channel_id},
                {"channelName", channel->second.name},
                {"plateEnabled", settings_value.plate_enabled},
                {"faceEnabled", settings_value.face_enabled},
                {"intervalMs", settings_value.interval_ms},
                {"plateRoi", settings_value.plate_roi},
                {"faceRoi", settings_value.face_roi},
                {"effectivePlate", effective_plate},
                {"effectiveFace", effective_face},
                {"effectiveIntervalMs", settings_value.interval_ms > 0 ? settings_value.interval_ms
                                                                       : settings.interval_ms}};
    return true;
  }
};

// =======================================================================
// AiService forwarding
// =======================================================================

AiService::AiService(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
AiService::~AiService() = default;

void AiService::Start() {
  impl_->Start();
}
void AiService::Stop() {
  impl_->Stop();
}
void AiService::Reload() {
  impl_->Reload();
}

json AiService::ConfigJson() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const AiSettings& s = impl_->settings;
  return json{{"enabled", s.enabled},
              {"plateEnabled", s.plate_enabled},
              {"faceEnabled", s.face_enabled},
              {"intervalMs", s.interval_ms},
              {"queueDepth", s.queue_depth},
              {"plateWorkers", s.plate_workers},
              {"faceWorkers", s.face_workers},
              {"minPlateConfidence", s.min_plate_confidence},
              {"minFaceQuality", s.min_face_quality},
              {"snapshotRetentionDays", s.snapshot_retention_days},
              {"snapshotEncrypt", s.snapshot_encrypt},
              {"snapshotMaxPerDay", s.snapshot_max_per_day},
              {"snapshotMaxTotalMb", s.snapshot_max_total_mb},
              {"snapshotMinFreeMb", s.snapshot_min_free_mb},
              {"stub", s.stub},
              {"faceModelDir", s.face_model_dir},
              {"facePixelLevel", s.face_pixel_level},
              {"plateWatchlist", s.plate_watchlist},
              {"plateBlacklist", s.plate_blacklist},
              {"plateWhitelist", s.plate_whitelist},
              {"faceWatchlist", s.face_watchlist},
              {"faceMatchThreshold", s.face_match_threshold},
              {"dedupSeconds", s.dedup_seconds},
              {"faceDedupSeconds", s.face_dedup_seconds},
              {"running", impl_->running}};
}

bool AiService::UpdateConfig(const json& body, json* out, std::string* error) {
  if (!body.is_object()) {
    if (error != nullptr)
      *error = "request body must be a JSON object";
    return false;
  }
  struct Update {
    std::string key;
    std::string value;
  };
  std::vector<Update> updates;
  auto fail = [error](const std::string& message) {
    if (error != nullptr)
      *error = message;
    return false;
  };
  auto read_bool = [&](const char* name, bool* value) -> bool {
    const auto it = body.find(name);
    if (it == body.end())
      return true;
    if (!it->is_boolean())
      return fail(std::string(name) + " must be a boolean");
    *value = it->get<bool>();
    return true;
  };
  auto read_int = [&](const char* name, int64_t low, int64_t high, int64_t* value) -> bool {
    const auto it = body.find(name);
    if (it == body.end())
      return true;
    if (!it->is_number_integer())
      return fail(std::string(name) + " must be an integer");
    const int64_t parsed = it->get<int64_t>();
    if (parsed < low || parsed > high)
      return fail(std::string(name) + " must be between " + std::to_string(low) + " and " +
                  std::to_string(high));
    *value = parsed;
    return true;
  };
  auto read_double = [&](const char* name, double* value) -> bool {
    const auto it = body.find(name);
    if (it == body.end())
      return true;
    if (!it->is_number())
      return fail(std::string(name) + " must be a number");
    const double parsed = it->get<double>();
    if (parsed < 0.0 || parsed > 1.0)
      return fail(std::string(name) + " must be between 0 and 1");
    *value = parsed;
    return true;
  };
  auto read_text = [&](const char* name, std::string* value) -> bool {
    const auto it = body.find(name);
    if (it == body.end())
      return true;
    if (!it->is_string())
      return fail(std::string(name) + " must be a string");
    *value = it->get<std::string>();
    return true;
  };

  bool enabled = true;
  bool plate = true;
  bool face = true;
  bool stub = false;
  bool snapshot_encrypt = false;
  int64_t interval = 1000;
  int64_t queue_depth = 8;
  int64_t plate_workers = 2;
  int64_t face_workers = 2;
  int64_t retention = 30;
  int64_t dedup = 10;
  int64_t face_dedup = 10;
  int64_t snapshot_max_per_day = 20000;
  int64_t snapshot_max_total_mb = 2048;
  int64_t snapshot_min_free_mb = 2048;
  double min_plate = 0.6;
  double min_face = 0.3;
  double match_threshold = 0.75;
  int64_t face_pixel_level = -1;
  std::string face_model_dir;
  std::string plate_watchlist;
  std::string plate_blacklist;
  std::string plate_whitelist;
  std::string face_watchlist;
  if (!read_bool("enabled", &enabled) || !read_bool("plateEnabled", &plate) ||
      !read_bool("faceEnabled", &face) || !read_bool("stub", &stub) ||
      !read_bool("snapshotEncrypt", &snapshot_encrypt) ||
      !read_int("intervalMs", 250, 10000, &interval) ||
      !read_int("queueDepth", 1, 64, &queue_depth) ||
      !read_int("plateWorkers", 1, 16, &plate_workers) ||
      !read_int("faceWorkers", 1, 16, &face_workers) ||
      !read_int("snapshotRetentionDays", 1, 3650, &retention) ||
      !read_int("dedupSeconds", 0, 3600, &dedup) ||
      !read_int("faceDedupSeconds", 0, 3600, &face_dedup) ||
      !read_int("snapshotMaxPerDay", 0, 1000000, &snapshot_max_per_day) ||
      !read_int("snapshotMaxTotalMb", 0, 10000000, &snapshot_max_total_mb) ||
      !read_int("snapshotMinFreeMb", 0, 10000000, &snapshot_min_free_mb) ||
      !read_int("facePixelLevel", -1, 1280, &face_pixel_level) ||
      !read_double("minPlateConfidence", &min_plate) || !read_double("minFaceQuality", &min_face) ||
      !read_double("faceMatchThreshold", &match_threshold) ||
      !read_text("faceModelDir", &face_model_dir) ||
      !read_text("plateWatchlist", &plate_watchlist) ||
      !read_text("plateBlacklist", &plate_blacklist) ||
      !read_text("plateWhitelist", &plate_whitelist) ||
      !read_text("faceWatchlist", &face_watchlist)) {
    return false;
  }

  auto add_double = [&](const char* key, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    updates.push_back({key, buffer});
  };
  if (body.contains("enabled"))
    updates.push_back({"ai.enabled", enabled ? "1" : "0"});
  if (body.contains("plateEnabled"))
    updates.push_back({"ai.plate.enabled", plate ? "1" : "0"});
  if (body.contains("faceEnabled"))
    updates.push_back({"ai.face.enabled", face ? "1" : "0"});
  if (body.contains("intervalMs"))
    updates.push_back({"ai.intervalMs", std::to_string(interval)});
  if (body.contains("queueDepth"))
    updates.push_back({"ai.queueDepth", std::to_string(queue_depth)});
  if (body.contains("plateWorkers"))
    updates.push_back({"ai.plateWorkers", std::to_string(plate_workers)});
  if (body.contains("faceWorkers"))
    updates.push_back({"ai.faceWorkers", std::to_string(face_workers)});
  if (body.contains("minPlateConfidence"))
    add_double("ai.minPlateConfidence", min_plate);
  if (body.contains("minFaceQuality"))
    add_double("ai.minFaceQuality", min_face);
  if (body.contains("snapshotRetentionDays"))
    updates.push_back({"ai.snapshotRetentionDays", std::to_string(retention)});
  if (body.contains("stub"))
    updates.push_back({"ai.stub", stub ? "1" : "0"});
  if (body.contains("snapshotEncrypt"))
    updates.push_back({"ai.snapshot.encrypt", snapshot_encrypt ? "1" : "0"});
  if (body.contains("faceModelDir"))
    updates.push_back({"ai.face.modelDir", face_model_dir});
  if (body.contains("facePixelLevel"))
    updates.push_back({"ai.face.pixelLevel", std::to_string(face_pixel_level)});
  if (body.contains("plateWatchlist"))
    updates.push_back({"ai.plate.watchlist", plate_watchlist});
  if (body.contains("plateBlacklist"))
    updates.push_back({"ai.plate.blacklist", plate_blacklist});
  if (body.contains("plateWhitelist"))
    updates.push_back({"ai.plate.whitelist", plate_whitelist});
  if (body.contains("faceWatchlist"))
    updates.push_back({"ai.face.watchlist", face_watchlist});
  if (body.contains("faceMatchThreshold"))
    add_double("ai.face.matchThreshold", match_threshold);
  if (body.contains("dedupSeconds"))
    updates.push_back({"ai.dedupSeconds", std::to_string(dedup)});
  if (body.contains("faceDedupSeconds"))
    updates.push_back({"ai.faceDedupSeconds", std::to_string(face_dedup)});
  if (body.contains("snapshotMaxPerDay"))
    updates.push_back({"ai.snapshotMaxPerDay", std::to_string(snapshot_max_per_day)});
  if (body.contains("snapshotMaxTotalMb"))
    updates.push_back({"ai.snapshotMaxTotalMb", std::to_string(snapshot_max_total_mb)});
  if (body.contains("snapshotMinFreeMb"))
    updates.push_back({"ai.snapshotMinFreeMb", std::to_string(snapshot_min_free_mb)});

  for (const auto& update : updates) {
    if (!impl_->WriteSetting(update.key, update.value, error))
      return false;
  }
  impl_->Reload();
  if (out != nullptr)
    *out = ConfigJson();
  return true;
}

bool AiService::GetChannelSettings(int64_t channel_id, json* out, std::string* error) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->ChannelSettingsJsonLocked(channel_id, out, error);
}

bool AiService::UpdateChannelSettings(int64_t channel_id, const json& body, std::string* error) {
  if (!body.is_object()) {
    if (error != nullptr)
      *error = "request body must be a JSON object";
    return false;
  }
  AiChannelSettings current;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->channels.find(channel_id) == impl_->channels.end()) {
      if (error != nullptr)
        *error = "channel not found";
      return false;
    }
    const auto it = impl_->channel_settings.find(channel_id);
    if (it != impl_->channel_settings.end())
      current = it->second;
    current.channel_id = channel_id;
  }
  if (body.contains("plateEnabled")) {
    if (!body["plateEnabled"].is_boolean()) {
      if (error != nullptr)
        *error = "plateEnabled must be a boolean";
      return false;
    }
    current.plate_enabled = body["plateEnabled"].get<bool>();
  }
  if (body.contains("faceEnabled")) {
    if (!body["faceEnabled"].is_boolean()) {
      if (error != nullptr)
        *error = "faceEnabled must be a boolean";
      return false;
    }
    current.face_enabled = body["faceEnabled"].get<bool>();
  }
  if (body.contains("intervalMs")) {
    if (!body["intervalMs"].is_number_integer()) {
      if (error != nullptr)
        *error = "intervalMs must be an integer";
      return false;
    }
    const int64_t interval = body["intervalMs"].get<int64_t>();
    if (interval != 0 && (interval < 250 || interval > 10000)) {
      if (error != nullptr)
        *error = "intervalMs must be 0 (inherit) or between 250 and 10000";
      return false;
    }
    current.interval_ms = static_cast<int>(interval);
  }
  if (body.contains("plateRoi")) {
    if (!body["plateRoi"].is_string()) {
      if (error != nullptr)
        *error = "plateRoi must be a string";
      return false;
    }
    current.plate_roi = body["plateRoi"].get<std::string>();
  }
  if (body.contains("faceRoi")) {
    if (!body["faceRoi"].is_string()) {
      if (error != nullptr)
        *error = "faceRoi must be a string";
      return false;
    }
    current.face_roi = body["faceRoi"].get<std::string>();
  }
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return false;
  }
  uvcpp::uvcpp_db_params params;
  params.add(current.channel_id);
  params.add(current.plate_enabled ? 1 : 0);
  params.add(current.face_enabled ? 1 : 0);
  params.add(current.interval_ms);
  params.add(current.plate_roi);
  params.add(current.face_roi);
  params.add(NowSeconds());
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->execute(
          "INSERT INTO ai_channel_settings(channel_id, plate_enabled, face_enabled, "
          "interval_ms, plate_roi, face_roi, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?) "
          "ON CONFLICT(channel_id) DO UPDATE SET plate_enabled = excluded.plate_enabled, "
          "face_enabled = excluded.face_enabled, interval_ms = excluded.interval_ms, "
          "plate_roi = excluded.plate_roi, face_roi = excluded.face_roi, "
          "updated_at = excluded.updated_at",
          params))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return false;
  }
  impl_->Reload();
  return true;
}

bool AiService::UpdateChannels(const std::vector<int64_t>& channel_ids, bool plate_enabled,
                               bool face_enabled, std::string* error) {
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return false;
  }
  for (const int64_t channel_id : channel_ids) {
    if (channel_id <= 0) {
      if (error != nullptr)
        *error = "channelIds must contain positive integers";
      return false;
    }
    AiChannelSettings current;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->channels.find(channel_id) == impl_->channels.end()) {
        if (error != nullptr)
          *error = "channel " + std::to_string(channel_id) + " not found";
        return false;
      }
      const auto it = impl_->channel_settings.find(channel_id);
      if (it != impl_->channel_settings.end())
        current = it->second;
    }
    current.channel_id = channel_id;
    current.plate_enabled = plate_enabled;
    current.face_enabled = face_enabled;
    uvcpp::uvcpp_db_params params;
    params.add(current.channel_id);
    params.add(current.plate_enabled ? 1 : 0);
    params.add(current.face_enabled ? 1 : 0);
    params.add(current.interval_ms);
    params.add(current.plate_roi);
    params.add(current.face_roi);
    params.add(NowSeconds());
    if (!uvcpp::uvcpp_db_ok(impl_->opts.db->execute(
            "INSERT INTO ai_channel_settings(channel_id, plate_enabled, face_enabled, "
            "interval_ms, plate_roi, face_roi, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(channel_id) DO UPDATE SET plate_enabled = excluded.plate_enabled, "
            "face_enabled = excluded.face_enabled, updated_at = excluded.updated_at",
            params))) {
      if (error != nullptr)
        *error = impl_->opts.db->last_error();
      return false;
    }
  }
  impl_->Reload();
  return true;
}

json AiService::Stats() {
  json channel_items = json::array();
  AiSettings snapshot;
  std::map<int64_t, AiChannelSettings> channel_settings;
  std::map<int64_t, AiChannelInfo> channels;
  std::shared_ptr<FrameQueue> plate_queue;
  std::shared_ptr<FrameQueue> face_queue;
  bool running = false;
  int active_plate_workers = 0;
  int active_face_workers = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    snapshot = impl_->settings;
    channel_settings = impl_->channel_settings;
    channels = impl_->channels;
    plate_queue = impl_->plate_queue;
    face_queue = impl_->face_queue;
    running = impl_->running;
    active_plate_workers = impl_->active_plate_workers;
    active_face_workers = impl_->active_face_workers;
  }
  for (const auto& [id, channel] : channels) {
    const auto it = channel_settings.find(id);
    const bool plate = it != channel_settings.end() && it->second.plate_enabled;
    const bool face = it != channel_settings.end() && it->second.face_enabled;
    const bool global_plate = snapshot.enabled && snapshot.plate_enabled;
    const bool global_face = snapshot.enabled && snapshot.face_enabled;
    channel_items.push_back(json{{"id", id},
                                 {"name", channel.name},
                                 {"plate", global_plate && channel.enabled && plate},
                                 {"face", global_face && channel.enabled && face}});
  }

  const int64_t now = NowMs();
  double plate_fps = 0.0;
  double face_fps = 0.0;
  {
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    if (impl_->stats_last_ms == 0 || now <= impl_->stats_last_ms) {
      impl_->stats_last_ms = now;
      impl_->stats_last_plate = impl_->plate_processed.load();
      impl_->stats_last_face = impl_->face_processed.load();
    } else {
      const int64_t elapsed = now - impl_->stats_last_ms;
      if (elapsed >= 500) {
        const uint64_t plate_now = impl_->plate_processed.load();
        const uint64_t face_now = impl_->face_processed.load();
        impl_->stats_plate_fps =
            static_cast<double>(plate_now - impl_->stats_last_plate) * 1000.0 / elapsed;
        impl_->stats_face_fps =
            static_cast<double>(face_now - impl_->stats_last_face) * 1000.0 / elapsed;
        impl_->stats_last_ms = now;
        impl_->stats_last_plate = plate_now;
        impl_->stats_last_face = face_now;
      }
    }
    plate_fps = impl_->stats_plate_fps;
    face_fps = impl_->stats_face_fps;
  }

  const uint64_t plate_processed = impl_->plate_processed.load();
  const uint64_t face_processed = impl_->face_processed.load();
  const uint64_t plate_dropped = plate_queue ? plate_queue->dropped() : 0;
  const uint64_t face_dropped = face_queue ? face_queue->dropped() : 0;
  const double plate_avg = plate_processed == 0
                               ? 0.0
                               : static_cast<double>(impl_->plate_total_ms.load()) /
                                     static_cast<double>(plate_processed);
  const double face_avg = face_processed == 0 ? 0.0
                                              : static_cast<double>(impl_->face_total_ms.load()) /
                                                    static_cast<double>(face_processed);

  impl_->snapshot_quota.Refresh(now);

  return json{{"enabled", snapshot.enabled},
              {"running", running},
              {"plate", json{{"workers", active_plate_workers},
                             {"processed", plate_processed},
                             {"failed", impl_->plate_failed.load()},
                             {"dropped", plate_dropped},
                             {"avgMs", plate_avg},
                             {"fps", plate_fps}}},
              {"face", json{{"workers", active_face_workers},
                            {"processed", face_processed},
                            {"failed", impl_->face_failed.load()},
                            {"dropped", face_dropped},
                            {"avgMs", face_avg},
                            {"fps", face_fps},
                            {"deduped", impl_->face_deduped.load()}}},
              {"snapshots", json{{"today", impl_->snapshot_quota.Today()},
                                 {"bytes", impl_->snapshot_quota.DirectoryBytes()},
                                 {"skipped", impl_->snapshot_quota.Skipped()},
                                 {"blocked", impl_->snapshot_quota.Blocked()}}},
              {"channels", channel_items}};
}

json AiService::QueryEvents(const std::string& type, int64_t channel_id, int64_t from, int64_t to,
                            const std::string& plate, int limit, int offset,
                            std::string* error) const {
  const std::string normalized_type = NormalizeType(type);
  if (normalized_type.empty()) {
    if (error != nullptr)
      *error = "type must be plate or face";
    return json();
  }
  json out;
  if (!impl_->QueryEventsImpl(normalized_type, channel_id, from, to, plate, limit, offset, &out,
                              error)) {
    return json();
  }
  return out;
}

json AiService::GetEvent(const std::string& type, int64_t id, std::string* error) const {
  const std::string normalized_type = NormalizeType(type);
  if (normalized_type.empty()) {
    if (error != nullptr)
      *error = "type must be plate or face";
    return json();
  }
  json out;
  if (!impl_->GetEventImpl(normalized_type, id, &out, error))
    return json();
  return out;
}

bool AiService::ReadSnapshot(const std::string& type, int64_t id, const std::string& kind,
                             std::string* bytes, std::string* error) const {
  const std::string normalized_type = NormalizeType(type);
  if (normalized_type.empty()) {
    if (error != nullptr)
      *error = "type must be plate or face";
    return false;
  }
  if (kind != "full" && kind != "crop") {
    if (error != nullptr)
      *error = "kind must be full or crop";
    return false;
  }
  return impl_->ReadSnapshotImpl(normalized_type, id, kind, bytes, error);
}

json AiService::PlateTrail(const std::string& plate, int64_t from, int64_t to,
                           std::string* error) const {
  const std::string normalized = NormalizePlate(plate);
  if (normalized.empty()) {
    if (error != nullptr)
      *error = "plate is required";
    return json();
  }
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return json();
  }
  std::string where = " WHERE e.plate_norm = ?";
  std::vector<uvcpp::uvcpp_db_value> values;
  values.push_back(uvcpp::uvcpp_db_value(normalized));
  if (from > 0) {
    where += " AND e.ts >= ?";
    values.push_back(uvcpp::uvcpp_db_value(from));
  }
  if (to > 0) {
    where += " AND e.ts <= ?";
    values.push_back(uvcpp::uvcpp_db_value(to));
  }
  uvcpp::uvcpp_db_params count_params;
  for (const auto& value : values)
    count_params.add(value);
  uvcpp::uvcpp_db_table count_table;
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->query(
          "SELECT COUNT(*) AS n FROM ai_plate_events e" + where, count_params, &count_table))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return json();
  }
  const int64_t total = count_table.empty() ? 0 : SqlInt(count_table, 0, "n");

  uvcpp::uvcpp_db_params params;
  for (const auto& value : values)
    params.add(value);
  params.add(200);
  uvcpp::uvcpp_db_table table;
  if (!uvcpp::uvcpp_db_ok(
          impl_->opts.db->query("SELECT e.*, c.name AS channel_name FROM ai_plate_events e "
                                "LEFT JOIN channels c ON c.id = e.channel_id" +
                                    where + " ORDER BY e.ts DESC, e.id DESC LIMIT ?",
                                params, &table))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return json();
  }
  json items = json::array();
  for (size_t row = 0; row < table.rows().size(); ++row)
    items.push_back(impl_->PlateItem(table, row));
  return json{{"plate", normalized}, {"total", total}, {"items", items}};
}

json AiService::TestDetect(int64_t channel_id, const std::string& type, std::string* error) {
  const std::string normalized_type = type.empty() || type == "both" ? "both" : NormalizeType(type);
  if (normalized_type.empty()) {
    if (error != nullptr)
      *error = "type must be plate, face or both";
    return json();
  }
  std::string primary = "main";
  AiRuntimeConfig runtime;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->channels.find(channel_id);
    if (it == impl_->channels.end()) {
      if (error != nullptr)
        *error = "channel not found";
      return json();
    }
    if (it->second.sub_enabled)
      primary = "sub";
    runtime = impl_->RuntimeLocked();
  }
  cv::Mat image;
  if (!impl_->CaptureFrame(channel_id, primary, &image, error))
    return json();

  AiConfig engine_config;
  engine_config.stub = runtime.stub;
  json result{
      {"channelId", channel_id}, {"width", image.cols}, {"height", image.rows}, {"engine", "stub"}};
  if (normalized_type == "plate" || normalized_type == "both") {
    const int64_t started = NowMs();
    std::unique_ptr<AiEngine> engine = CreatePlateEngine(engine_config, error);
    if (!engine)
      return json();
    std::vector<AiPlateResult> results;
    if (!engine->DetectPlates(image, &results, error))
      return json();
    json plates = json::array();
    for (const auto& plate : results) {
      plates.push_back(
          json{{"text", plate.text},
               {"normalized", NormalizePlate(plate.text)},
               {"valid", IsValidPlate(plate.text)},
               {"confidence", plate.confidence},
               {"box", json{{"x", plate.x}, {"y", plate.y}, {"w", plate.w}, {"h", plate.h}}},
               {"color", plate.color}});
    }
    result["plates"] = plates;
    result["plateMs"] = NowMs() - started;
  }
  if (normalized_type == "face" || normalized_type == "both") {
    const int64_t started = NowMs();
    std::unique_ptr<AiEngine> engine = CreateFaceEngine(engine_config, error);
    if (!engine)
      return json();
    std::vector<AiFaceResult> results;
    if (!engine->DetectFaces(image, &results, error))
      return json();
    json faces = json::array();
    for (const auto& face : results) {
      faces.push_back(
          json{{"detConfidence", face.detConfidence},
               {"quality", face.quality},
               {"box", json{{"x", face.x}, {"y", face.y}, {"w", face.w}, {"h", face.h}}},
               {"featureSize", face.feature.size()}});
    }
    result["faces"] = faces;
    result["faceMs"] = NowMs() - started;
  }
  return result;
}

// ---- face library ------------------------------------------------------

json AiService::ListFaces(std::string* error) const {
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return json();
  }
  uvcpp::uvcpp_db_table table;
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->query(
          "SELECT id, name, note, image_path, feature_blob, created_at FROM ai_faces "
          "ORDER BY id",
          {}, &table))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return json();
  }
  json items = json::array();
  for (size_t row = 0; row < table.rows().size(); ++row) {
    const std::string feature = table.rows()[row]["feature_blob"].bytes();
    items.push_back(json{{"id", SqlInt(table, row, "id")},
                         {"name", SqlColumn(table, row, "name")},
                         {"note", SqlColumn(table, row, "note")},
                         {"imagePath", SqlColumn(table, row, "image_path")},
                         {"hasFeature", !feature.empty()},
                         {"featureSize", feature.size() / sizeof(float)},
                         {"createdAt", SqlInt(table, row, "created_at")}});
  }
  return json{{"items", items}};
}

json AiService::SearchFaces(const json& body, std::string* error) const {
  if (!body.is_object()) {
    if (error != nullptr)
      *error = "request body must be a JSON object";
    return json();
  }
  const auto feature_it = body.find("feature");
  if (feature_it == body.end() || !feature_it->is_array()) {
    if (error != nullptr)
      *error = "feature must be an array of numbers";
    return json();
  }
  const std::vector<float> feature = JsonToFloats(*feature_it);
  if (feature.empty()) {
    if (error != nullptr)
      *error = "feature must be a non-empty array of numbers";
    return json();
  }
  // Default to the configured global threshold so a caller that omits the
  // field gets the same decision the live pipeline would make.
  double threshold = 0.75;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    threshold = impl_->settings.face_match_threshold;
  }
  if (body.contains("threshold")) {
    if (!body["threshold"].is_number()) {
      if (error != nullptr)
        *error = "threshold must be a number between 0 and 1";
      return json();
    }
    threshold = body["threshold"].get<double>();
    if (threshold < 0.0 || threshold > 1.0) {
      if (error != nullptr)
        *error = "threshold must be between 0 and 1";
      return json();
    }
  }
  int limit = 10;
  if (body.contains("limit")) {
    if (!body["limit"].is_number_integer()) {
      if (error != nullptr)
        *error = "limit must be an integer between 1 and 50";
      return json();
    }
    limit = body["limit"].get<int>();
    if (limit < 1 || limit > 50) {
      if (error != nullptr)
        *error = "limit must be between 1 and 50";
      return json();
    }
  }

  std::vector<FaceLibraryEntry> library;
  {
    std::lock_guard<std::mutex> lock(impl_->faces_mutex);
    library = impl_->faces;
  }
  const std::vector<FaceMatchResult> hits = RankFaceMatches(library, feature, threshold, limit);
  json items = json::array();
  bool matched = false;
  for (const auto& hit : hits) {
    matched = matched || hit.found;
    items.push_back(json{{"id", hit.id},
                         {"name", hit.name},
                         {"score", hit.score},
                         {"matched", hit.found}});
  }
  return json{{"threshold", threshold},
              {"librarySize", library.size()},
              {"matched", matched},
              {"items", items}};
}

bool AiService::CreateFace(const json& body, int64_t* id, std::string* error) {
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return false;
  }
  if (!body.is_object() || !body.contains("name") || !body["name"].is_string() ||
      Trim(body["name"].get<std::string>()).empty()) {
    if (error != nullptr)
      *error = "name is required";
    return false;
  }
  std::vector<float> feature;
  if (body.contains("feature")) {
    feature = JsonToFloats(body["feature"]);
    if (feature.empty()) {
      if (error != nullptr)
        *error = "feature must be an array of numbers";
      return false;
    }
  }
  const std::string name = Trim(body["name"].get<std::string>());
  const std::string note = body.value("note", std::string());
  const std::string image_path = body.value("imagePath", std::string());
  uvcpp::uvcpp_db_params params;
  params.add(name);
  params.add(note);
  if (feature.empty())
    params.add_null();
  else
    params.add(uvcpp::uvcpp_db_value::blob(FloatsToBlob(feature)));
  params.add(image_path);
  params.add(NowSeconds());
  uvcpp::uvcpp_db_value new_id;
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->insert(
          "INSERT INTO ai_faces(name, note, feature_blob, image_path, created_at) "
          "VALUES(?, ?, ?, ?, ?)",
          params, &new_id))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return false;
  }
  if (id != nullptr)
    *id = new_id.to_int64();
  impl_->LoadFaces();
  return true;
}

bool AiService::UpdateFace(int64_t id, const json& body, std::string* error) {
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return false;
  }
  uvcpp::uvcpp_db_params id_params;
  id_params.add(id);
  uvcpp::uvcpp_db_table table;
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->query(
          "SELECT name, note, feature_blob, image_path FROM ai_faces WHERE id = ?", id_params,
          &table))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return false;
  }
  if (table.empty()) {
    if (error != nullptr)
      *error = "face not found";
    return false;
  }
  std::string name = SqlColumn(table, 0, "name");
  std::string note = SqlColumn(table, 0, "note");
  std::string image_path = SqlColumn(table, 0, "image_path");
  std::string feature = table.rows()[0]["feature_blob"].bytes();
  if (body.contains("name")) {
    if (!body["name"].is_string() || Trim(body["name"].get<std::string>()).empty()) {
      if (error != nullptr)
        *error = "name must be a non-empty string";
      return false;
    }
    name = Trim(body["name"].get<std::string>());
  }
  if (body.contains("note")) {
    if (!body["note"].is_string()) {
      if (error != nullptr)
        *error = "note must be a string";
      return false;
    }
    note = body["note"].get<std::string>();
  }
  if (body.contains("imagePath")) {
    if (!body["imagePath"].is_string()) {
      if (error != nullptr)
        *error = "imagePath must be a string";
      return false;
    }
    image_path = body["imagePath"].get<std::string>();
  }
  if (body.contains("feature")) {
    const std::vector<float> values = JsonToFloats(body["feature"]);
    if (values.empty()) {
      if (error != nullptr)
        *error = "feature must be an array of numbers";
      return false;
    }
    feature = FloatsToBlob(values);
  }
  uvcpp::uvcpp_db_params params;
  params.add(name);
  params.add(note);
  if (feature.empty())
    params.add_null();
  else
    params.add(uvcpp::uvcpp_db_value::blob(feature));
  params.add(image_path);
  params.add(id);
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->execute(
          "UPDATE ai_faces SET name = ?, note = ?, feature_blob = ?, image_path = ? WHERE id = ?",
          params))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return false;
  }
  impl_->LoadFaces();
  return true;
}

bool AiService::DeleteFace(int64_t id, std::string* error) {
  if (impl_->opts.db == nullptr) {
    if (error != nullptr)
      *error = "database is not available";
    return false;
  }
  uvcpp::uvcpp_db_params params;
  params.add(id);
  if (!uvcpp::uvcpp_db_ok(impl_->opts.db->execute("DELETE FROM ai_faces WHERE id = ?", params))) {
    if (error != nullptr)
      *error = impl_->opts.db->last_error();
    return false;
  }
  impl_->LoadFaces();
  return true;
}

} // namespace uvnvr
