#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace uvcpp {
class uvcpp_db_pool;
}

namespace uvnvr {

class MediaService;

// Owns the AI capture timers, worker pools, persistence and retention cleanup.
//
// Threading contract:
//   * Start()/Stop()/Reload() are serialized by an internal lifecycle mutex.
//   * Every worker creates and destroys its own AiEngine instance.
//   * All database calls go through the thread-safe uvcpp_db_pool.
//   * Disk writes happen on capture/worker threads, never on the libuv loop.
class AiService {
public:
  struct Options {
    uvcpp::uvcpp_db_pool* db = nullptr;
    MediaService* media = nullptr;
    std::string data_dir;
    // Broadcasts one JSON message on /ws/events.
    std::function<void(const nlohmann::json&)> broadcast;
    // Delivers the same recognition as a REST-shaped event item. The private
    // protocol forwards it to subscribed SDK clients. Kept separate from
    // `broadcast` because the WebSocket message uses its own field names.
    std::function<void(const nlohmann::json&)> event_sink;
    // Insert a snapshot path; may be empty.
    std::function<void(int64_t, const std::string&, const std::string&, const std::string&,
                       const std::string&)>
        raise_alert;
    // Optional data-at-rest hooks supplied when the secure module is compiled
    // in. `seal_snapshot` replaces the bytes written to disk (or returns the
    // input unchanged when encryption is off); `open_snapshot` reverses it and
    // also passes legacy plaintext through. Both receive the on-disk path so
    // key derivation can bind to it.
    std::function<bool(const std::string&, const std::string&, std::string*, std::string*)>
        seal_snapshot;
    std::function<bool(const std::string&, const std::string&, std::string*, std::string*)>
        open_snapshot;
  };

  explicit AiService(Options options);
  ~AiService();

  AiService(const AiService&) = delete;
  AiService& operator=(const AiService&) = delete;

  void Start();
  void Stop();
  // Re-reads ai.* settings and per-channel flags from the database, then
  // restarts the pipeline so changes take effect without a service restart.
  void Reload();

  nlohmann::json ConfigJson() const;
  bool UpdateConfig(const nlohmann::json& body, nlohmann::json* out, std::string* error);

  bool GetChannelSettings(int64_t channel_id, nlohmann::json* out, std::string* error) const;
  bool UpdateChannelSettings(int64_t channel_id, const nlohmann::json& body, std::string* error);
  bool UpdateChannels(const std::vector<int64_t>& channel_ids, bool plate_enabled,
                      bool face_enabled, std::string* error);

  nlohmann::json Stats();

  nlohmann::json QueryEvents(const std::string& type, int64_t channel_id, int64_t from, int64_t to,
                             const std::string& plate, int limit, int offset,
                             std::string* error) const;
  nlohmann::json GetEvent(const std::string& type, int64_t id, std::string* error) const;
  bool ReadSnapshot(const std::string& type, int64_t id, const std::string& kind,
                    std::string* bytes, std::string* error) const;
  nlohmann::json PlateTrail(const std::string& plate, int64_t from, int64_t to,
                            std::string* error) const;

  nlohmann::json TestDetect(int64_t channel_id, const std::string& type, std::string* error);

  // Local face library CRUD. Stage 3 recognition uses the stored features.
  nlohmann::json ListFaces(std::string* error) const;
  bool CreateFace(const nlohmann::json& body, int64_t* id, std::string* error);
  bool UpdateFace(int64_t id, const nlohmann::json& body, std::string* error);
  bool DeleteFace(int64_t id, std::string* error);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
