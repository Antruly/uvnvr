#include "playback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace uvnvr {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr size_t kReadChunk = 64 * 1024;
constexpr int64_t kTsPacket = 188;
constexpr int64_t kKeyframeScanLimit = 8 * 1024 * 1024;
// Send this much media immediately so the MSE player can start without
// waiting for the first paced chunks.
constexpr double kStartupBurstMs = 1500.0;

bool IsKeyframePacket(const uint8_t* packet) {
  if (packet[0] != 0x47 || (packet[1] & 0x40) == 0)
    return false; // no sync byte or no payload-unit start
  size_t payload = 4;
  if ((packet[3] & 0x20) != 0) {
    payload += 1 + static_cast<size_t>(packet[4]);
  }
  if (payload + 4 > static_cast<size_t>(kTsPacket))
    return false;
  const uint8_t* cursor = packet + payload;
  if (cursor[0] != 0 || cursor[1] != 0)
    return false;
  const uint8_t* nal = nullptr;
  if (cursor[2] == 1) {
    nal = cursor + 3;
  } else if (cursor[2] == 0 && cursor[3] == 1) {
    nal = cursor + 4;
  } else {
    return false;
  }
  if (nal >= packet + kTsPacket)
    return false;
  const int h264_type = nal[0] & 0x1f;
  if (h264_type == 5 || h264_type == 7) // IDR or SPS
    return true;
  const int h265_type = (nal[0] >> 1) & 0x3f;
  return h265_type == 19 || h265_type == 20 || h265_type == 32 || h265_type == 33;
}

// Finds a keyframe near `approximate`, aligned to TS packets. Returns 0 when
// no suitable packet is found close by, so the caller starts at the segment
// boundary (which is keyframe-aligned by construction).
int64_t FindKeyframeOffset(std::ifstream& file, int64_t approximate, int64_t file_size) {
  if (approximate <= 0 || file_size <= 0)
    return 0;
  int64_t offset = approximate - approximate % kTsPacket;
  if (offset < 0)
    offset = 0;
  if (offset >= file_size)
    return 0;
  file.clear();
  file.seekg(offset);
  std::vector<uint8_t> window(static_cast<size_t>(kTsPacket) * 2048);
  int64_t scanned = 0;
  while (file && scanned < kKeyframeScanLimit) {
    file.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(window.size()));
    const std::streamsize got = file.gcount();
    if (got < kTsPacket)
      break;
    const size_t packets = static_cast<size_t>(got) / static_cast<size_t>(kTsPacket);
    for (size_t index = 0; index < packets; ++index) {
      const uint8_t* packet = window.data() + index * static_cast<size_t>(kTsPacket);
      if (IsKeyframePacket(packet))
        return offset + static_cast<int64_t>(index) * kTsPacket;
    }
    offset += static_cast<int64_t>(packets) * kTsPacket;
    scanned += static_cast<int64_t>(packets) * kTsPacket;
  }
  return 0;
}

} // namespace

struct PlaybackService::Impl {
  struct Session : std::enable_shared_from_this<Session> {
    uvcpp::uvcpp_web_app* web = nullptr;
    uvcpp::uvcpp_ws_connection* connection = nullptr;
    int64_t channel_id = 0;
    std::string stream;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    double speed = 1.0;
    std::atomic<bool> alive{true};
    std::atomic<bool> pending{false};
    uvcpp::uvcpp_db_pool* db = nullptr;
    BlockCache* cache = nullptr;
    size_t readahead_bytes = 0;
    ActiveFn active_provider;

    void Post(const std::vector<uint8_t>& bytes) {
      auto copy = std::make_shared<std::vector<uint8_t>>(bytes);
      auto self = shared_from_this();
      web->post([self, copy] {
        if (!self->alive.load() || self->connection == nullptr)
          return;
        self->pending = true;
        const int rc =
            self->connection->send_binary(reinterpret_cast<const char*>(copy->data()), copy->size(),
                                          [self](int) { self->pending = false; });
        if (rc != 0) {
          self->alive = false;
          self->connection->close();
        }
      });
    }

    void StreamActive(const RecordingService::ActiveSegment& segment,
                      int64_t* position_ms) {
      const bool bounded = end_ms > 0 && end_ms < (1LL << 59);
      uint64_t offset = 0;
      *position_ms = std::max(start_ms, segment.start_ms);
      std::error_code size_error;
      const auto initial_size = std::filesystem::file_size(segment.part_path, size_error);
      if (!size_error && initial_size > 0 && start_ms > segment.start_ms) {
        const int64_t elapsed = std::max<int64_t>(1, NowMs() - segment.start_ms);
        const double fraction = std::clamp(
            static_cast<double>(start_ms - segment.start_ms) / static_cast<double>(elapsed), 0.0,
            1.0);
        std::ifstream scan_file(segment.part_path, std::ios::binary);
        if (scan_file) {
          const int64_t approximate = static_cast<int64_t>(fraction * initial_size);
          const int64_t keyframe =
              FindKeyframeOffset(scan_file, approximate, static_cast<int64_t>(initial_size));
          offset = static_cast<uint64_t>(std::max<int64_t>(0, keyframe));
        }
      }
      bool was_partial = true;
      while (alive.load()) {
        std::error_code ec;
        const bool part_exists =
            !segment.part_path.empty() && std::filesystem::exists(segment.part_path, ec);
        const std::string path = part_exists ? segment.part_path : segment.final_path;
        if (path.empty() || !std::filesystem::exists(path, ec))
          break;
        std::ifstream file(path, std::ios::binary);
        if (!file)
          break;
        file.seekg(static_cast<std::streamoff>(offset));
        std::vector<uint8_t> buffer(kReadChunk);
        while (alive.load()) {
          while (alive.load() && pending.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          file.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
          const size_t got = static_cast<size_t>(file.gcount());
          if (got == 0)
            break;
          buffer.resize(got);
          Post(buffer);
          offset += got;
          const int64_t elapsed = std::max<int64_t>(1, NowMs() - segment.start_ms);
          const double bytes_per_ms =
              static_cast<double>(std::max<int64_t>(segment.bytes, static_cast<int64_t>(offset))) /
              static_cast<double>(elapsed);
          if (bytes_per_ms > 0.0)
            *position_ms =
                segment.start_ms + static_cast<int64_t>(static_cast<double>(offset) / bytes_per_ms);
          if (bounded && *position_ms >= end_ms)
            return;
        }
        if (std::filesystem::exists(segment.part_path, ec)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          was_partial = true;
          continue;
        }
        if (path == segment.final_path || !was_partial)
          break;
        was_partial = false;
      }
    }

    void Run() {
      uvcpp::uvcpp_db_table table;
      uvcpp::uvcpp_db_params params;
      params.add(channel_id);
      params.add(stream);
      params.add(start_ms > 0 ? start_ms : 0);
      params.add(end_ms > 0 ? end_ms : (1LL << 60));
      std::string error;
      if (!uvcpp::uvcpp_db_ok(
              db->query("SELECT path, start_ms, end_ms, size_bytes, codec FROM recordings "
                        "WHERE channel_id = ? AND stream = ? AND end_ms > ? AND start_ms < ? "
                        "ORDER BY start_ms",
                        params, &table))) {
        const std::string message = R"({"error":"database_error"})";
        web->post([self = shared_from_this(), message] {
          if (self->alive.load() && self->connection != nullptr) {
            self->connection->send_text(message.data(), message.size());
            self->connection->close();
          }
        });
        return;
      }

      const double rate = speed > 0.0 ? speed : 1.0;
      const bool bounded = end_ms > 0 && end_ms < (1LL << 59);
      std::string expected_codec =
          table.empty() ? std::string() : table.at(0, "codec").to_text();
      const auto clock_start = std::chrono::steady_clock::now();
      double media_ms = 0.0;
      int64_t playback_position = start_ms;

      for (size_t row = 0; row < table.rows().size() && alive.load(); ++row) {
        const std::string codec = table.at(row, "codec").to_text();
        if (row > 0 && !expected_codec.empty() && !codec.empty() && codec != expected_codec)
          break; // a codec switch needs a new player/session
        const std::string path = table.at(row, "path").to_text();
        const int64_t segment_start = table.at(row, "start_ms").to_int64();
        const int64_t segment_end = table.at(row, "end_ms").to_int64();
        int64_t file_size = table.at(row, "size_bytes").to_int64();
        std::error_code size_error;
        const auto actual_size = std::filesystem::file_size(path, size_error);
        if (!size_error)
          file_size = static_cast<int64_t>(actual_size);
        const int64_t duration_ms = std::max<int64_t>(1, segment_end - segment_start);

        int64_t offset = 0;
        int64_t read_start_ms = segment_start;
        if (row == 0 && start_ms > segment_start && file_size > 0) {
          std::ifstream scan_file(path, std::ios::binary);
          if (scan_file) {
            const double fraction = std::clamp(static_cast<double>(start_ms - segment_start) /
                                                   static_cast<double>(duration_ms),
                                               0.0, 1.0);
            const int64_t approximate = static_cast<int64_t>(fraction * file_size);
            offset = FindKeyframeOffset(scan_file, approximate, file_size);
            if (offset <= 0)
              offset = 0;
            if (offset > 0)
              read_start_ms = segment_start + static_cast<int64_t>(fraction * duration_ms);
          }
        }
        if (file_size <= 0)
          file_size = 1;
        const double ms_per_byte =
            static_cast<double>(duration_ms) / static_cast<double>(file_size);
        int64_t position_ms = read_start_ms;

        // Fill the cache for the next segment while this one is streaming.
        if (cache != nullptr && readahead_bytes > 0 && row + 1 < table.rows().size()) {
          cache->Preload(table.at(row + 1, "path").to_text(), 0, readahead_bytes);
        }

        uint64_t read_offset = static_cast<uint64_t>(offset);
        std::vector<uint8_t> buffer;
        std::string read_error;
        while (alive.load()) {
          while (alive.load() && pending.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
          if (!alive.load())
            break;
          if (cache == nullptr || !cache->Read(path, read_offset, kReadChunk, &buffer, &read_error))
            break;
          const size_t got = buffer.size();
          if (got == 0)
            break;
          Post(buffer);
          read_offset += got;
          position_ms += static_cast<int64_t>(static_cast<double>(got) * ms_per_byte);
          media_ms += static_cast<double>(got) * ms_per_byte;

          // Pace to the requested rate. The startup burst lets the player
          // build a small buffer before real-time pacing begins.
          const double wall_ms = (media_ms - kStartupBurstMs) / rate;
          if (wall_ms > 0.0) {
            const auto target =
                clock_start + std::chrono::microseconds(static_cast<int64_t>(wall_ms * 1000.0));
            if (target > std::chrono::steady_clock::now())
              std::this_thread::sleep_until(target);
          }
          if (bounded && position_ms >= end_ms)
            break;
        }
        playback_position = position_ms;
        if (bounded && position_ms >= end_ms)
          break;
      }

      // Segments still being written are not in the index yet. Follow them
      // through the active-recorder provider until the range ends or the
      // recorder rotates and no successor appears.
      std::set<std::string> handled;
      bool sent_media = !table.empty();
      int idle_polls = 0;
      while (alive.load()) {
        if (bounded && playback_position >= end_ms)
          break;
        std::vector<RecordingService::ActiveSegment> active =
            active_provider ? active_provider() : std::vector<RecordingService::ActiveSegment>();
        const RecordingService::ActiveSegment* match = nullptr;
        for (const auto& segment : active) {
          if (segment.channel_id != channel_id || segment.stream != stream)
            continue;
          if (handled.find(segment.part_path) != handled.end())
            continue;
          if (bounded && segment.start_ms >= end_ms)
            continue;
          if (!expected_codec.empty() && !segment.codec.empty() &&
              segment.codec != expected_codec)
            continue;
          match = &segment;
          break;
        }
        if (match == nullptr) {
          if (!sent_media && idle_polls >= 2) {
            const std::string message = R"({"error":"no_recordings"})";
            web->post([self = shared_from_this(), message] {
              if (self->alive.load() && self->connection != nullptr) {
                self->connection->send_text(message.data(), message.size());
                self->connection->close();
              }
            });
            return;
          }
          if (bounded && idle_polls >= 3)
            break;
          ++idle_polls;
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          continue;
        }
        idle_polls = 0;
        handled.insert(match->part_path);
        expected_codec = match->codec.empty() ? expected_codec : match->codec;
        StreamActive(*match, &playback_position);
        sent_media = true;
        if (!bounded && !alive.load())
          break;
      }
      auto self = shared_from_this();
      web->post([self] {
        if (self->alive.load() && self->connection != nullptr) {
          self->connection->close();
        }
      });
    }
  };

  Impl(uvcpp::uvcpp_web_app* web_in, uvcpp::uvcpp_db_pool* db_in, size_t cache_bytes,
       size_t readahead_bytes_in)
      : web(web_in), db(db_in), cache(std::make_unique<BlockCache>(cache_bytes)),
        readahead_bytes(readahead_bytes_in) {}

  uvcpp::uvcpp_web_app* web;
  uvcpp::uvcpp_db_pool* db;
  std::unique_ptr<BlockCache> cache;
  size_t readahead_bytes;
  ActiveFn active_provider;

  void Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id, const std::string& stream,
            int64_t start_ms, int64_t end_ms, double speed) {
    auto session = std::make_shared<Session>();
    session->web = web;
    session->connection = connection;
    session->channel_id = channel_id;
    session->stream = stream;
    session->start_ms = start_ms;
    session->end_ms = end_ms;
    session->speed = speed;
    session->db = db;
    session->cache = cache.get();
    session->readahead_bytes = readahead_bytes;
    session->active_provider = active_provider;
    std::weak_ptr<Session> weak = session;
    connection->on_close([weak](uvcpp::ws_close_code, const std::string&) {
      if (auto self = weak.lock()) {
        self->alive = false;
        self->connection = nullptr;
      }
    });
    std::thread([session] { session->Run(); }).detach();
  }
};

PlaybackService::PlaybackService(uvcpp::uvcpp_web_app* web, uvcpp::uvcpp_db_pool* db,
                                 size_t cache_bytes, size_t readahead_bytes)
    : impl_(std::make_unique<Impl>(web, db, cache_bytes, readahead_bytes)) {}

PlaybackService::~PlaybackService() = default;

void PlaybackService::Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id,
                           const std::string& stream, int64_t start_ms, int64_t end_ms,
                           double speed) {
  impl_->Open(connection, channel_id, stream, start_ms, end_ms, speed);
}

void PlaybackService::SetActiveProvider(ActiveFn provider) {
  impl_->active_provider = std::move(provider);
}

BlockCache::Stats PlaybackService::CacheStats() const {
  return impl_->cache->GetStats();
}

} // namespace uvnvr
