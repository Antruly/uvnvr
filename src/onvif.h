#pragma once

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

} // namespace uvnvr
