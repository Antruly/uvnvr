#include "config.h"

#include "platform.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

namespace uvnvr {
namespace {

using json = nlohmann::json;

// Records keys the parser does not know. `prefix` is the dotted path of the
// containing object ("" for the root), so "http" + "prot" becomes "http.prot".
void CollectUnknownKeys(const json& object, std::string_view prefix,
                        std::initializer_list<std::string_view> known,
                        ConfigLoadReport* report) {
  if (report == nullptr || !object.is_object())
    return;
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool recognized = false;
    for (const std::string_view key : known) {
      if (std::string_view(it.key()) == key) {
        recognized = true;
        break;
      }
    }
    if (recognized)
      continue;
    if (prefix.empty())
      report->unknown_keys.push_back(it.key());
    else
      report->unknown_keys.push_back(std::string(prefix) + "." + it.key());
  }
}

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
    const std::filesystem::path directory = std::filesystem::path(executable).parent_path();
    candidates.push_back((directory / "uvnvr.json").string());
    candidates.push_back((directory.parent_path() / "uvnvr.json").string());
  }
#ifdef _WIN32
  if (const char* program_data = std::getenv("ProgramData");
      program_data != nullptr && program_data[0] != '\0') {
    candidates.push_back((std::filesystem::path(program_data) / "UvNVR" / "uvnvr.json").string());
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

bool ApplyConfigJson(const json& document, Config* config, std::string* error,
                     ConfigLoadReport* report) {
  if (!document.is_object()) {
    *error = "the configuration root must be a JSON object";
    return false;
  }

  // `config` is what ConfigToJson emits for diagnostics, so a --print-config
  // document can be fed back without a spurious unknown-key warning.
  CollectUnknownKeys(document, "",
                     {"port", "rtspPort", "privatePort", "privateTcpEnabled",
                      "privateQuicEnabled", "dataDir", "host", "ffmpegPath", "logLevel",
                      "cacheMb", "readaheadMb", "maintenance", "tlsSelfSigned", "tlsCert",
                      "tlsKey", "tls", "http", "rtsp", "private", "ffmpeg", "logging",
                      "storage", "config"},
                     report);

  // Flat aliases keep hand-written configs short; the nested form below wins
  // when both are present.
  if (!ReadInt(document, "port", &config->port, error) ||
      !ReadInt(document, "rtspPort", &config->rtsp_port, error) ||
      !ReadInt(document, "privatePort", &config->private_port, error) ||
      !ReadBool(document, "privateTcpEnabled", &config->private_tcp_enabled, error) ||
      !ReadBool(document, "privateQuicEnabled", &config->private_quic_enabled, error) ||
      !ReadString(document, "dataDir", &config->data_dir, error) ||
      !ReadString(document, "host", &config->host, error) ||
      !ReadString(document, "ffmpegPath", &config->ffmpeg_path, error) ||
      !ReadString(document, "logLevel", &config->log_level, error) ||
      !ReadInt(document, "cacheMb", &config->cache_mb, error) ||
      !ReadInt(document, "readaheadMb", &config->readahead_mb, error) ||
      !ReadBool(document, "maintenance", &config->maintenance, error) ||
      !ReadBool(document, "tlsSelfSigned", &config->tls_self_signed, error) ||
      !ReadString(document, "tlsCert", &config->tls_cert, error) ||
      !ReadString(document, "tlsKey", &config->tls_key, error)) {
    return false;
  }

  if (const auto tls = document.find("tls"); tls != document.end()) {
    CollectUnknownKeys(*tls, "tls", {"cert", "key", "selfSigned"}, report);
    if (!ReadTls(*tls, config, error))
      return false;
  }

  if (const auto http = document.find("http"); http != document.end()) {
    if (!http->is_object()) {
      *error = "http must be an object";
      return false;
    }
    CollectUnknownKeys(*http, "http", {"port", "host", "tls"}, report);
    if (!ReadInt(*http, "port", &config->port, error) ||
        !ReadString(*http, "host", &config->host, error))
      return false;
    if (const auto tls = http->find("tls"); tls != http->end()) {
      CollectUnknownKeys(*tls, "http.tls", {"cert", "key", "selfSigned"}, report);
      if (!ReadTls(*tls, config, error))
        return false;
    }
  }

  if (const auto rtsp = document.find("rtsp"); rtsp != document.end()) {
    if (!rtsp->is_object()) {
      *error = "rtsp must be an object";
      return false;
    }
    CollectUnknownKeys(*rtsp, "rtsp", {"port"}, report);
    if (!ReadInt(*rtsp, "port", &config->rtsp_port, error))
      return false;
  }

  // Private protocol (TCP + UDP/QUIC on the same port).
  if (const auto private_section = document.find("private");
      private_section != document.end()) {
    if (!private_section->is_object()) {
      *error = "private must be an object";
      return false;
    }
    CollectUnknownKeys(*private_section, "private", {"port", "tcp", "quic"}, report);
    if (!ReadInt(*private_section, "port", &config->private_port, error) ||
        !ReadBool(*private_section, "tcp", &config->private_tcp_enabled, error) ||
        !ReadBool(*private_section, "quic", &config->private_quic_enabled, error))
      return false;
  }

  if (const auto ffmpeg = document.find("ffmpeg"); ffmpeg != document.end()) {
    if (ffmpeg->is_string()) {
      config->ffmpeg_path = ffmpeg->get<std::string>();
    } else if (ffmpeg->is_object()) {
      CollectUnknownKeys(*ffmpeg, "ffmpeg", {"path"}, report);
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
    CollectUnknownKeys(*logging, "logging", {"level"}, report);
    if (!ReadString(*logging, "level", &config->log_level, error))
      return false;
  }

  if (const auto storage = document.find("storage"); storage != document.end()) {
    if (!storage->is_object()) {
      *error = "storage must be an object";
      return false;
    }
    CollectUnknownKeys(*storage, "storage", {"cacheMb", "readaheadMb"}, report);
    if (!ReadInt(*storage, "cacheMb", &config->cache_mb, error) ||
        !ReadInt(*storage, "readaheadMb", &config->readahead_mb, error))
      return false;
  }

  if (const auto diagnostics = document.find("config"); diagnostics != document.end())
    CollectUnknownKeys(*diagnostics, "config", {"path", "loaded"}, report);

  return true;
}

bool LoadConfigText(std::string_view contents, const std::string& path, Config* config,
                    std::string* error, ConfigLoadReport* report) {
  const json document = json::parse(contents.begin(), contents.end(), nullptr, false);
  if (document.is_discarded()) {
    if (error != nullptr)
      *error = "invalid JSON in configuration file " + path;
    return false;
  }
  if (!ApplyConfigJson(document, config, error, report))
    return false;
  config->config_path = path;
  config->config_loaded = true;
  return true;
}

bool LoadConfigFile(const std::string& path, Config* config, std::string* error,
                    ConfigLoadReport* report) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error != nullptr)
      *error = "cannot open configuration file " + path;
    return false;
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  if (input.bad()) {
    if (error != nullptr)
      *error = "cannot read configuration file " + path;
    return false;
  }
  return LoadConfigText(contents, path, config, error, report);
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
  if (config.private_port < 1 || config.private_port > 65535)
    return fail("private.port must be between 1 and 65535");
  if (config.port == config.rtsp_port)
    return fail("http.port and rtsp.port must differ");
  // TCP listeners cannot share a port. UDP/QUIC could, but keeping the three
  // ports distinct makes the configuration unambiguous and avoids a silent
  // "QUIC works, TCP does not" split brain.
  if (config.private_port == config.port || config.private_port == config.rtsp_port)
    return fail("private.port must differ from http.port and rtsp.port");
  if (!config.private_tcp_enabled && !config.private_quic_enabled)
    return fail("at least one private transport must be enabled");
  if (config.data_dir.empty())
    return fail("dataDir must not be empty");
  if (config.tls_cert.empty() != config.tls_key.empty())
    return fail("tls.cert and tls.key must be configured together");
  if (config.log_level != "trace" && config.log_level != "debug" && config.log_level != "info" &&
      config.log_level != "warn" && config.log_level != "error") {
    return fail("logging.level must be trace, debug, info, warn or error");
  }
  if (config.cache_mb < 0 || config.cache_mb > 8192)
    return fail("storage.cacheMb must be between 0 and 8192");
  if (config.readahead_mb < 0 || config.readahead_mb > 1024)
    return fail("storage.readaheadMb must be between 0 and 1024");
  return true;
}

json ConfigToJson(const Config& config) {
  json document;
  document["http"] = {{"port", config.port},
                      {"host", config.host},
                      {"tls",
                       {{"cert", config.tls_cert},
                        {"key", config.tls_key},
                        {"selfSigned", config.tls_self_signed}}}};
  document["rtsp"] = {{"port", config.rtsp_port}};
  document["private"] = {{"port", config.private_port},
                         {"tcp", config.private_tcp_enabled},
                         {"quic", config.private_quic_enabled}};
  document["dataDir"] = config.data_dir;
  document["maintenance"] = config.maintenance;
  document["ffmpeg"] = {{"path", config.ffmpeg_path}};
  document["logging"] = {{"level", config.log_level}};
  document["storage"] = {{"cacheMb", config.cache_mb}, {"readaheadMb", config.readahead_mb}};
  document["config"] = {{"path", config.config_path}, {"loaded", config.config_loaded}};
  return document;
}

} // namespace uvnvr
