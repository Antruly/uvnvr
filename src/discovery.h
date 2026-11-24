#pragma once

#include "onvif.h"

#include <string>
#include <vector>

namespace uvnvr {

// Read-only LAN discovery for the three protocols a camera may answer:
// ONVIF WS-Discovery, Hikvision SADP and Dahua DHDiscover.
struct ScanOptions {
  // Wall-clock budget shared by all protocols. They are probed in parallel.
  int window_ms = 4000;
  bool onvif = true;
  bool sadp = true;
  bool dahua = true;
  // "address:port" entries. Protocol defaults are used when a list is empty.
  std::vector<std::string> sadp_targets;
  std::vector<std::string> dahua_targets;
};

struct SadpDevice {
  std::string ip;
  std::string mac;
  std::string serial;
  std::string device_type;
  std::string description;
  std::string firmware;
  std::string uuid;
  int http_port = 80;
  int rtsp_port = 554;
};

struct DahuaDevice {
  std::string ip;
  std::string mac;
  std::string serial;
  std::string device_type;
  std::string name;
  std::string version;
  int http_port = 80;
  int rtsp_port = 554;
};

struct ScanProtocolStats {
  std::string protocol;
  int sent = 0;
  int responses = 0;
  std::string error;
};

struct ScanDiscoveryResult {
  int window_ms = 4000;
  std::vector<OnvifDevice> onvif_devices;
  std::vector<SadpDevice> sadp_devices;
  std::vector<DahuaDevice> dahua_devices;
  std::vector<ScanProtocolStats> stats;
  std::vector<std::string> warnings;
};

// Runs every enabled protocol concurrently and returns once the window
// expires. Never changes camera configuration.
ScanDiscoveryResult RunMultiProtocolDiscovery(const ScanOptions& options);

// Exposed for tests and diagnostics.
std::string NormaliseMacAddress(const std::string& value);

} // namespace uvnvr
