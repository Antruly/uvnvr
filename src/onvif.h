#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr {

struct OnvifDevice {
  std::string xaddr;
  std::string ip;
};

struct OnvifProfile {
  std::string token;
  std::string name;
  std::string stream_uri;
};

struct OnvifInfo {
  std::string manufacturer;
  std::string model;
  std::string firmware;
  std::string serial;
  std::vector<OnvifProfile> profiles;
};

bool DiscoverOnvif(std::vector<OnvifDevice>* devices, int timeout_ms, std::string* error);
bool ProbeOnvif(const std::string& xaddr, const std::string& username, const std::string& password,
                OnvifInfo* info, std::string* error);

struct OnvifEvent {
  std::string topic;
  std::string name;
  std::string value;
};

struct OnvifEventSubscription {
  std::string address;
};

// Resolves the camera's event service from GetCapabilities, falling back to
// http://host:port/onvif/events.
bool ResolveOnvifEventUrl(const std::string& host, int port, const std::string& username,
                          const std::string& password, std::string* url, std::string* error);

// Creates a PullPoint subscription. `event_url` is the event service endpoint.
bool CreateOnvifEventSubscription(const std::string& event_url, const std::string& username,
                                  const std::string& password, OnvifEventSubscription* subscription,
                                  std::string* error);

// Pulls pending notifications. The HTTP timeout is sized for `timeout_ms` plus
// a small margin so a camera holding the request open does not look like a
// failure.
bool PullOnvifEvents(const std::string& address, const std::string& username,
                     const std::string& password, int timeout_ms, std::vector<OnvifEvent>* events,
                     std::string* error);

struct OnvifEncoderConfig {
  std::string token;
  std::string encoding;
  int width = 0;
  int height = 0;
  int fps = 0;
  int bitrate_kbps = 0;
  int quality = 0;
};

// Reads the first video encoder configuration through the ONVIF media service.
bool GetOnvifEncoderConfig(const std::string& host, int port, const std::string& username,
                           const std::string& password, OnvifEncoderConfig* config,
                           std::string* error);

// Applies encoding, resolution, frame rate and bitrate to that configuration.
bool SetOnvifEncoderConfig(const std::string& host, int port, const std::string& username,
                           const std::string& password, const OnvifEncoderConfig& config,
                           std::string* error);

// Sets the camera clock through the ONVIF device service (Manual UTC time).
bool SetOnvifTime(const std::string& host, int port, const std::string& username,
                  const std::string& password, int64_t epoch_ms, std::string* error);

} // namespace uvnvr
