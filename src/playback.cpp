#include "playback.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

namespace uvnvr {

struct PlaybackService::Impl {
  struct Session : std::enable_shared_from_this<Session> {
    uvcpp::uvcpp_web_app* web = nullptr;
    uvcpp::uvcpp_ws_connection* connection = nullptr;
    int64_t channel_id = 0;
    std::string stream;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
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
              "SELECT path, start_ms, end_ms FROM recordings WHERE channel_id = ? AND stream = ? "
              "AND end_ms > ? AND start_ms < ? ORDER BY start_ms",
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

      for (size_t row = 0; row < table.rows().size() && alive.load(); ++row) {
        const std::string path = table.at(row, "path").to_text();
        std::ifstream file(path, std::ios::binary);
        if (!file)
          continue;
        std::vector<uint8_t> buffer(64 * 1024);
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
          buffer.resize(64 * 1024);
        }
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
            int64_t start_ms, int64_t end_ms) {
    auto session = std::make_shared<Session>();
    session->web = web;
    session->connection = connection;
    session->channel_id = channel_id;
    session->stream = stream;
    session->start_ms = start_ms;
    session->end_ms = end_ms;
    session->db = db;
    uvcpp::uvcpp_ws_connection* raw = connection;
    std::weak_ptr<Session> weak = session;
    connection->on_close([weak](uvcpp::ws_close_code, const std::string&) {
      if (auto self = weak.lock()) {
        self->alive = false;
        self->connection = nullptr;
      }
    });
    (void)raw;
    std::thread([session] { session->Run(); }).detach();
  }
};

PlaybackService::PlaybackService(uvcpp::uvcpp_web_app* web, uvcpp::uvcpp_db_pool* db)
    : impl_(std::make_unique<Impl>(web, db)) {}

PlaybackService::~PlaybackService() = default;

void PlaybackService::Open(uvcpp::uvcpp_ws_connection* connection, int64_t channel_id,
                           const std::string& stream, int64_t start_ms, int64_t end_ms) {
  impl_->Open(connection, channel_id, stream, start_ms, end_ms);
}

} // namespace uvnvr
