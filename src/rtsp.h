#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace uvnvr {

// Minimal RTSP server for the live MPEG-TS streams. It exposes
// rtsp://host:port/live/{channel}/{main|sub} as RTP/MP2T (payload type 33),
// with TCP interleaved and UDP unicast transports.
class RtspServer {
public:
  using AuthFn = std::function<bool(const std::string&, const std::string&)>;
  using AttachFn = std::function<uint64_t(
      int64_t, const std::string&, std::function<void(const uint8_t*, size_t)>, std::string*)>;
  using DetachFn = std::function<void(int64_t, const std::string&, uint64_t)>;

  RtspServer(AuthFn auth, AttachFn attach, DetachFn detach, int port);
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
