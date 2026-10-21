#pragma once

#include <cstdint>
#include <string>

namespace uvnvr {

struct BrandDeviceInfo {
  std::string protocol; // "hikvision" or "dahua"
  std::string brand;
  std::string name;
  std::string model;
  std::string serial;
  std::string firmware;
};

// Probes a Hikvision (ISAPI) or Dahua (CGI) camera over HTTP using Digest/Basic
// authentication. `preferred` may be empty, "hikvision" or "dahua".
bool ProbeBrandHttp(const std::string& host, int port, const std::string& username,
                    const std::string& password, const std::string& preferred,
                    BrandDeviceInfo* info, std::string* error);

struct BrandEncoderConfig {
  std::string encoding;
  int width = 0;
  int height = 0;
  int fps = 0;
  int bitrate_kbps = 0;
  int quality = 0;
};

// Reads the main-stream encoder settings through ISAPI or Dahua CGI.
bool GetBrandEncoderConfig(const std::string& protocol, const std::string& host, int port,
                           const std::string& username, const std::string& password,
                           BrandEncoderConfig* config, std::string* error);

// Applies encoding/resolution/frame rate/bitrate to the main stream.
bool SetBrandEncoderConfig(const std::string& protocol, const std::string& host, int port,
                           const std::string& username, const std::string& password,
                           const BrandEncoderConfig& config, std::string* error);

// Sets the camera clock using the vendor HTTP API. `protocol` selects ISAPI or
// Dahua CGI.
bool SetBrandTime(const std::string& protocol, const std::string& host, int port,
                  const std::string& username, const std::string& password, int64_t epoch_ms,
                  std::string* error);

} // namespace uvnvr
