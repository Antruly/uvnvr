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
#include <thread>
#include <vector>

namespace uvnvr {
namespace {

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
    file.read(reinterpret_cast<char*>(window.data()),
              static_cast<std::streamsize>(window.size()));
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

    void Run() {
      uvcpp::uvcpp_db_table table;
      uvcpp::uvcpp_db_params params;
      params.add(channel_id);
      params.add(stream);
      params.add(start_ms > 0 ? start_ms : 0);
      params.add(end_ms > 0 ? end_ms : (1LL << 60));
      std::string error;
      if (!uvcpp::uvcpp_db_ok(db->query(
              "SELECT path, start_ms, end_ms, size_bytes FROM recordings "
              "WHERE channel_id = ? AND stream = ? AND end_ms > ? AND start_ms < ? "
              "ORDER BY start_ms",
              params, &table)) ||
          table.empty()) {
        const std::string message = R"({"error":"no_recordings"})";
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
      const auto clock_start = std::chrono::steady_clock::now();
      double media_ms = 0.0;

      for (size_t row = 0; row < table.rows().size() && alive.load(); ++row) {
        const std::string path = table.at(row, "path").to_text();
        const int64_t segment_start = table.at(row, "start_ms").to_int64();
        const int64_t segment_end = table.at(row, "end_ms").to_int64();
        int64_t file_size = table.at(row, "size_bytes").to_int64();
        std::error_code size_error;
        const auto actual_size = std::filesystem::file_size(path, size_error);
        if (!size_error)
          file_size = static_cast<int64_t>(actual_size);
        const int64_t duration_ms = std::max<int64_t>(1, segment_end - segment_start);

        std::ifstream file(path, std::ios::binary);
        if (!file)
          continue;

        int64_t offset = 0;
        int64_t read_start_ms = segment_start;
        if (row == 0 && start_ms > segment_start && file_size > 0) {
          const double fraction =
              std::clamp(static_cast<double>(start_ms - segment_start) /
                             static_cast<double>(duration_ms),
                         0.0, 1.0);
          const int64_t approximate = static_cast<int64_t>(fraction * file_size);
          offset = FindKeyframeOffset(file, approximate, file_size);
          if (offset <= 0)
            offset = 0;
          if (offset > 0) {
            read_start_ms =
                segment_start + static_cast<int64_t>(fraction * duration_ms);
            file.clear();
            file.seekg(offset);
          } else {
            file.clear();
            file.seekg(0);
          }
        }
        if (file_size <= 0)
          file_size = 1;
        const double ms_per_byte = static_cast<double>(duration_ms) / static_cast<double>(file_size);
        int64_t position_ms = read_start_ms;

        std::vector<uint8_t> buffer(kReadChunk);
        while (alive.load()) {
          while (alive.load() && pending.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
          if (!alive.load())
            break;
          file.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
          const std::streamsize got = file.gcount();
          if (got <= 0)
            break;
          buffer.resize(static_cast<size_t>(got));
          Post(buffer);
          buffer.resize(kReadChunk);
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
        if (bounded && position_ms >= end_ms)
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

  Impl(uvcpp::uvcpp_web_app* web_in, uvcpp::uvcpp_db_pool* db_in) : web(web_in), db(db_in) {}

  uvcpp::uvcpp_web_app* web;
  uvcpp::uvcpp_db_pool* db;

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

PlaybackService::PlaybackService(uvcpp::uvcpp_web_app* web, uvcpp::uvcpp_db_pool* db)
    : impl_(std::make_unique<Impl>(web, db)) {}

PlaybackService::~PlaybackService() = default;

void PlaybackService::Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id,
                           const std::string& stream, int64_t start_ms, int64_t end_ms,
                           double speed) {
  impl_->Open(connection, channel_id, stream, start_ms, end_ms, speed);
}

} // namespace uvnvr
