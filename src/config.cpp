#include "config.h"

#include "platform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace uvnvr {
namespace {

using json = nlohmann::json;

bool ReadString(const json& object, const char* key, std::string* target, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end())
    return true;
  if (!it->is_string()) {
    *error = std::string(key) + " must be a string";
    return false;
  }
  *target = it->get<std::string>();
  return true;
}

bool ReadInt(const json& object, const char* key, int* target, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end())
    return true;
  if (!it->is_number_integer()) {
    *error = std::string(key) + " must be an integer";
    return false;
  }
  *target = it->get<int>();
  return true;
}

bool ReadBool(const json& object, const char* key, bool* target, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end())
    return true;
  if (!it->is_boolean()) {
    *error = std::string(key) + " must be true or false";
    return false;
  }
  *target = it->get<bool>();
  return true;
}

bool ReadTls(const json& tls, Config* config, std::string* error) {
  if (!tls.is_object()) {
    *error = "tls must be an object";
    return false;
  }
  return ReadString(tls, "cert", &config->tls_cert, error) &&
         ReadString(tls, "key", &config->tls_key, error) &&
         ReadBool(tls, "selfSigned", &config->tls_self_signed, error);
}

} // namespace

std::vector<std::string> DefaultConfigCandidates() {
  std::vector<std::string> candidates;
  const std::string executable = platform::ExecutablePath();
  if (!executable.empty()) {
    const std::filesystem::path directory =
        std::filesystem::path(executable).parent_path();
    candidates.push_back((directory / "uvnvr.json").string());
    candidates.push_back((directory.parent_path() / "uvnvr.json").string());
  }
#ifdef _WIN32
  if (const char* program_data = std::getenv("ProgramData");
      program_data != nullptr && program_data[0] != '\0') {
    candidates.push_back(
        (std::filesystem::path(program_data) / "UvNVR" / "uvnvr.json").string());
  }
#elif defined(__APPLE__)
  candidates.push_back("/Library/Application Support/UvNVR/uvnvr.json");
#else
  candidates.push_back("/etc/uvnvr/uvnvr.json");
#endif

  std::vector<std::string> unique;
  std::set<std::string> seen;
  for (const std::string& candidate : candidates) {
    if (candidate.empty() || !seen.insert(candidate).second)
      continue;
    unique.push_back(candidate);
  }
  return unique;
}

bool ApplyConfigJson(const json& document, Config* config, std::string* error) {
  if (!document.is_object()) {
    *error = "the configuration root must be a JSON object";
    return false;
  }

  // Flat aliases keep hand-written configs short; the nested form below wins
  // when both are present.
  if (!ReadInt(document, "port", &config->port, error) ||
      !ReadInt(document, "rtspPort", &config->rtsp_port, error) ||
      !ReadString(document, "dataDir", &config->data_dir, error) ||
      !ReadString(document, "host", &config->host, error) ||
      !ReadString(document, "ffmpegPath", &config->ffmpeg_path, error) ||
      !ReadString(document, "logLevel", &config->log_level, error) ||
      !ReadBool(document, "maintenance", &config->maintenance, error) ||
      !ReadBool(document, "tlsSelfSigned", &config->tls_self_signed, error) ||
      !ReadString(document, "tlsCert", &config->tls_cert, error) ||
      !ReadString(document, "tlsKey", &config->tls_key, error)) {
    return false;
  }

  if (const auto tls = document.find("tls"); tls != document.end() && !ReadTls(*tls, config, error))
    return false;

  if (const auto http = document.find("http"); http != document.end()) {
    if (!http->is_object()) {
      *error = "http must be an object";
      return false;
    }
    if (!ReadInt(*http, "port", &config->port, error) ||
        !ReadString(*http, "host", &config->host, error))
      return false;
    if (const auto tls = http->find("tls"); tls != http->end() && !ReadTls(*tls, config, error))
      return false;
  }

  if (const auto rtsp = document.find("rtsp"); rtsp != document.end()) {
    if (!rtsp->is_object()) {
      *error = "rtsp must be an object";
      return false;
    }
    if (!ReadInt(*rtsp, "port", &config->rtsp_port, error))
      return false;
  }

  if (const auto ffmpeg = document.find("ffmpeg"); ffmpeg != document.end()) {
    if (ffmpeg->is_string()) {
      config->ffmpeg_path = ffmpeg->get<std::string>();
    } else if (ffmpeg->is_object()) {
      if (!ReadString(*ffmpeg, "path", &config->ffmpeg_path, error))
        return false;
    } else {
      *error = "ffmpeg must be a path string or an object";
      return false;
    }
  }

  if (const auto logging = document.find("logging"); logging != document.end()) {
    if (!logging->is_object()) {
      *error = "logging must be an object";
      return false;
    }
    if (!ReadString(*logging, "level", &config->log_level, error))
      return false;
  }

  return true;
}

bool LoadConfigFile(const std::string& path, Config* config, std::string* error) {
  std::ifstream input(path);
  if (!input) {
    if (error != nullptr)
      *error = "cannot open configuration file " + path;
    return false;
  }
  const json document = json::parse(input, nullptr, false);
  if (document.is_discarded()) {
    if (error != nullptr)
      *error = "invalid JSON in configuration file " + path;
    return false;
  }
  if (!ApplyConfigJson(document, config, error))
    return false;
  config->config_path = path;
  config->config_loaded = true;
  return true;
}

bool ValidateConfig(const Config& config, std::string* error) {
  const auto fail = [error](const std::string& message) {
    if (error != nullptr)
      *error = message;
    return false;
  };

  if (config.port < 1 || config.port > 65535)
    return fail("http.port must be between 1 and 65535");
  if (config.rtsp_port < 1 || config.rtsp_port > 65535)
    return fail("rtsp.port must be between 1 and 65535");
  if (config.port == config.rtsp_port)
    return fail("http.port and rtsp.port must differ");
  if (config.data_dir.empty())
    return fail("dataDir must not be empty");
  if (config.tls_cert.empty() != config.tls_key.empty())
    return fail("tls.cert and tls.key must be configured together");
  if (config.log_level != "trace" && config.log_level != "debug" &&
      config.log_level != "info" && config.log_level != "warn" &&
      config.log_level != "error") {
    return fail("logging.level must be trace, debug, info, warn or error");
  }
  return true;
}

json ConfigToJson(const Config& config) {
  json document;
  document["http"] = {{"port", config.port}, {"host", config.host},
                      {"tls", {{"cert", config.tls_cert},
                               {"key", config.tls_key},
                               {"selfSigned", config.tls_self_signed}}}};
  document["rtsp"] = {{"port", config.rtsp_port}};
  document["dataDir"] = config.data_dir;
  document["maintenance"] = config.maintenance;
  document["ffmpeg"] = {{"path", config.ffmpeg_path}};
  document["logging"] = {{"level", config.log_level}};
  document["config"] = {{"path", config.config_path}, {"loaded", config.config_loaded}};
  return document;
}

} // namespace uvnvr
