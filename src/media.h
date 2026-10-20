#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_app.h>

namespace uvnvr {

using json = nlohmann::json;

struct ChannelSpec {
  int64_t id = 0;
  std::string name;
  std::string main_url;
  std::string sub_url;
  bool sub_enabled = false;
  bool enabled = true;
};

struct StreamStatus {
  std::string state = "disabled";
  std::string error;
  int64_t last_data_at_ms = 0;
  int64_t bytes = 0;
  int64_t reconnects = 0;
  bool has_keyframe = false;
};

// One FFmpeg process per enabled channel stream. The process outputs MPEG-TS on
// stdout; the service fans those bytes out to WebSocket subscribers and later to
// the recording engine. All libuv handles live on the web app's loop.
class MediaService {
public:
  explicit MediaService(uvcpp::uvcpp_web_app* web);
  ~MediaService();

  MediaService(const MediaService&) = delete;
  MediaService& operator=(const MediaService&) = delete;

  void SetFfmpegPath(const std::string& path);
  std::string FfmpegPath() const;
  bool Available() const;

  void Start();
  void Stop();

  void Upsert(const ChannelSpec& spec);
  void Remove(int64_t channel_id);
  void Reconnect(int64_t channel_id);
  void SetDataSink(std::function<void(int64_t, const std::string&, const uint8_t*, size_t)> sink);

  bool Subscribe(int64_t channel_id, const std::string& stream,
                 uvcpp::uvcpp_ws_connection* connection, std::string* error);

  StreamStatus Status(int64_t channel_id, const std::string& stream) const;
  json StatusJson() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
