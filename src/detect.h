#pragma once

#include <string>
#include <vector>

namespace uvnvr {

struct DetectAttempt {
  std::string protocol; // onvif, hikvision, dahua, rtsp
  int port = 0;
  bool ok = false;
  std::string error;
};

struct CameraDetectResult {
  std::string detected; // empty when no protocol answered
  int http_port = 0;
  int rtsp_port = 0;
  std::string vendor;
  std::string model;
  std::string firmware;
  std::string serial;
  std::string device_name;
  // onvif (GetScopes name scope), isapi (ISAPI deviceName), cgi
  // (magicBox getMachineName). Empty when the device exposed no name.
  std::string name_source;
  std::vector<std::string> stream_uris;
  std::vector<DetectAttempt> tried;
};

// Recognises one camera. `protocol` may be onvif (default), hikvision, dahua,
// rtsp or auto. `port` is the HTTP port supplied by the operator (0 when it
// was not given). Every attempt, including failures, is appended to
// `result.tried` so the UI can explain each port that was probed.
bool DetectCamera(const std::string& host, int port, const std::string& username,
                  const std::string& password, const std::string& protocol,
                  CameraDetectResult* result, std::string* error);

// Sends an RTSP OPTIONS request, retrying with Digest when the server
// challenges. Returns true when an RTSP service answered (200 or 401).
bool ProbeRtspOptions(const std::string& host, int port, const std::string& username,
                      const std::string& password, std::string* error);

} // namespace uvnvr
