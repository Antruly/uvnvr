#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace uvnvr {

// RTSP server for the live streams. It exposes
// rtsp://host:port/live/{channel}/{main|sub} as H.264/H.265 RTP (payload type
// 96) with TCP interleaved and UDP unicast transports.
class RtspServer {
public:
  using AuthFn = std::function<bool(const std::string&, const std::string&)>;
  using Ha1Fn = std::function<std::string(const std::string&)>;
  using AttachFn = std::function<uint64_t(
      int64_t, const std::string&, std::function<void(const uint8_t*, size_t)>, std::string*)>;
  using DetachFn = std::function<void(int64_t, const std::string&, uint64_t)>;

  RtspServer(AuthFn auth, Ha1Fn ha1, AttachFn attach, DetachFn detach, int port);
  ~RtspServer();

  RtspServer(const RtspServer&) = delete;
  RtspServer& operator=(const RtspServer&) = delete;

  bool Start(std::string* error);
  void Stop();
  int Port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
