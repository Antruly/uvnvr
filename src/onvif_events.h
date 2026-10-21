#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace uvnvr {

struct OnvifEventConfig {
  int64_t channel_id = 0;
  std::string host;
  int port = 80;
  std::string username;
  std::string password;
};

// Polls ONVIF PullPoint subscriptions for motion, person, tamper and line
// crossing notifications and forwards them as alerts.
class OnvifEventService {
public:
  using AlertFn =
      std::function<void(int64_t channel_id, const std::string& type, const std::string& message)>;

  explicit OnvifEventService(AlertFn alert);
  ~OnvifEventService();

  OnvifEventService(const OnvifEventService&) = delete;
  OnvifEventService& operator=(const OnvifEventService&) = delete;

  void Upsert(const OnvifEventConfig& config);
  void Remove(int64_t channel_id);
  void Stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr
