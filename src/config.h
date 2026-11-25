#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr {

// Startup configuration. Values come from defaults, then a JSON file, then
// environment variables, then command-line flags (highest precedence).
struct Config {
  std::string data_dir = "./data";
  std::string db_path;
  int port = 8080;
  std::string host = "0.0.0.0";
  int rtsp_port = 554;
  int private_port = 1515;
  bool private_tcp_enabled = true;
  bool private_quic_enabled = true;
  bool tls_self_signed = false;
  std::string tls_cert;
  std::string tls_key;
  bool maintenance = false;
  std::string ffmpeg_path;
  std::string log_level = "info";
  int cache_mb = 128;
  int readahead_mb = 4;

  // Resolved startup-config file, for diagnostics and the system API.
  std::string config_path;
  bool config_loaded = false;
};

// Optional diagnostics from a config load. Unknown keys are still ignored, but
// their dotted paths are collected so the caller can warn about typos instead
// of silently dropping them (e.g. "http.port" vs "http.prot").
struct ConfigLoadReport {
  std::vector<std::string> unknown_keys;
};

// Candidate config files in priority order: executable directory, its parent,
// then the platform-wide configuration directory.
std::vector<std::string> DefaultConfigCandidates();

// Applies a JSON config document to `config`. Unknown keys are ignored and, when
// `report` is given, recorded; malformed values return false with a message
// naming the key.
bool ApplyConfigJson(const nlohmann::json& document, Config* config, std::string* error,
                     ConfigLoadReport* report = nullptr);

// Reads and applies a config file. A missing file is an error for explicitly
// requested paths and is handled by the caller for discovery candidates.
bool LoadConfigFile(const std::string& path, Config* config, std::string* error,
                    ConfigLoadReport* report = nullptr);

// Applies config text that was already read by the caller. This lets startup
// verify a digest/signature over the exact bytes that are parsed, instead of
// opening the file a second time. `path` is used for diagnostics and stored in
// Config::config_path.
bool LoadConfigText(std::string_view contents, const std::string& path, Config* config,
                    std::string* error, ConfigLoadReport* report = nullptr);

// Validates ports, TLS pairing and the data directory value.
bool ValidateConfig(const Config& config, std::string* error);

// Effective configuration with defaults filled in, for --print-config and the
// read-only system API. Secrets are not part of the file format.
nlohmann::json ConfigToJson(const Config& config);

} // namespace uvnvr
