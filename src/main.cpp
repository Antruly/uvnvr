#include "app.h"
#include "config.h"
#include "platform.h"
#include "service.h"

#ifdef UVNVR_ENABLE_SECURE
#include "secure/audit_store.h"
#include "secure/firmware.h"
#endif
#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
#include "secure/config_integrity.h"
#endif

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <uvcpp/uvcpp_threadpool.h>
#include <uvnvr/version.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Options {
  uvnvr::Config config;
  bool help = false;
  bool version = false;
  bool print_config = false;
  bool fatal = false;
  bool service = false;
  bool open_browser = false;
  bool verify_audit = false;
  std::string verify_firmware;
  std::string firmware_key;
  std::string service_action;
  // Configuration integrity options. Parsed unconditionally so a build without
  // the module can reject them with a clear message.
  bool verify_config = false;
  bool write_config_digest = false;
  bool sign_config = false;
  std::string config_private_key;
  std::string config_integrity_mode;
  std::string config_manifest_path;
  std::string config_public_key;
  // Raw file contents kept so verification hashes exactly what was parsed.
  bool config_file_read = false;
  std::string config_file_path;
  std::string config_file_contents;
#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
  uvnvr::secure::ConfigIntegrityMode integrity_mode =
      uvnvr::secure::ConfigIntegrityMode::kWarn;
#endif
};

// Read helper shared by the config loader and the secure command modes.
bool ReadTextFile(const std::string& path, std::string* contents, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error != nullptr)
      *error = "cannot open " + path;
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    if (error != nullptr)
      *error = "cannot read " + path;
    return false;
  }
  return true;
}

#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
std::string ResolveConfigManifestPath(const Options& options) {
  if (!options.config_manifest_path.empty())
    return options.config_manifest_path;
  return uvnvr::secure::DefaultConfigManifestPath(options.config_file_path);
}
#endif

#ifdef _WIN32
std::string NormalizePathForCompare(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
    if (c == '/')
      c = '\\';
  }
  while (!value.empty() && value.back() == '\\')
    value.pop_back();
  return value;
}

bool IsUnderDirectory(const std::string& path, const char* directory) {
  if (directory == nullptr || directory[0] == '\0')
    return false;
  const std::string root = NormalizePathForCompare(directory);
  const std::string candidate = NormalizePathForCompare(path);
  return candidate.size() > root.size() && candidate.compare(0, root.size(), root) == 0 &&
         candidate[root.size()] == '\\';
}
#endif

// Installed Windows builds live under Program Files, which is read-only for
// normal users. Their writable state belongs in ProgramData, matching the
// directory used by the installer's boot task. Portable and development runs
// keep the historical ./data default.
std::string DefaultDataDir() {
#ifdef _WIN32
  const std::string executable = uvnvr::platform::ExecutablePath();
  if (IsUnderDirectory(executable, std::getenv("ProgramFiles")) ||
      IsUnderDirectory(executable, std::getenv("ProgramFiles(x86)"))) {
    if (const char* program_data = std::getenv("ProgramData");
        program_data != nullptr && program_data[0] != '\0') {
      return std::string(program_data) + "\\UvNVR";
    }
  }
  // Installs on another drive keep their recordings next to the application
  // instead of silently using C:.
  if (!executable.empty()) {
    std::filesystem::path directory = std::filesystem::path(executable).parent_path();
    const std::string leaf = directory.filename().string();
    if (leaf == "bin" || leaf == "Bin" || leaf == "BIN")
      directory = directory.parent_path();
    return (directory / "data").string();
  }
#endif
  return "./data";
}

void PrintUsage(const char* argv0) {
  std::cout << "UvNVR " << UVNVR_VERSION << " (" << UVNVR_GIT_SHA << ")\n\n"
            << "Usage: " << argv0 << " [options]\n\n"
            << "Options:\n"
            << "  --config <path>    Startup JSON configuration file\n"
            << "  --port <port>      HTTP/HTTPS listen port (default: 8080)\n"
            << "  --host <address>   HTTP bind address (default: 0.0.0.0)\n"
            << "  --rtsp-port <port> RTSP listen port (default: 554; falls back to 8554)\n"
            << "  --private-port <n> Private protocol TCP+QUIC port (default: 1515)\n"
            << "  --data-dir <path>  Data directory (default: " << DefaultDataDir() << ")\n"
            << "  --tls-cert <path>  PEM certificate; requires --tls-key\n"
            << "  --tls-key <path>   PEM private key; requires --tls-cert\n"
            << "  --tls-self-signed  Serve HTTPS with a generated localhost certificate\n"
            << "  --ffmpeg <path>    FFmpeg executable path\n"
            << "  --log-level <lvl>  trace, debug, info, warn or error (default: info)\n"
            << "  --maintenance      Enable maintenance mode\n"
            << "  --service          Run as a Windows service (SCM entry point)\n"
            << "  --open             Open the web UI in the default browser after startup\n"
            << "  --print-config     Print the effective configuration and exit\n"
            << "  --verify-audit     Verify the SM3 audit chain in the configured database\n"
            << "  --verify-firmware <package> <public-key.pem>\n"
            << "                     Verify a signed firmware package and exit\n"
            << "  --config-integrity <mode>\n"
            << "                     off, warn or enforce (default: warn; requires a build\n"
            << "                     with UVNVR_ENABLE_CONFIG_INTEGRITY)\n"
            << "  --config-manifest <path>  Integrity manifest (default: <config>.sm3)\n"
            << "  --config-public-key <pem> SM2 public key or certificate for signed manifests\n"
            << "  --verify-config    Verify the configuration manifest and exit\n"
            << "  --write-config-digest\n"
            << "                     Write an SM3-only manifest for the configuration and exit\n"
            << "  --sign-config <key.pem>\n"
            << "                     Write an SM2-signed manifest for the configuration and exit\n"
            << "  --version          Print version and exit\n"
            << "  --help             Print this help and exit\n\n"
            << "Configuration file:\n"
            << "  JSON, checked in this order: --config, UVNVR_CONFIG, the executable\n"
            << "  directory, its parent, then the platform configuration directory.\n"
            << "  Command-line flags override the file and environment variables.\n\n"
            << "Environment:\n"
            << "  UVNVR_CONFIG       Configuration file path\n"
            << "  UVNVR_PORT         HTTP/HTTPS port\n"
            << "  UVNVR_HOST         HTTP bind address\n"
            << "  UVNVR_RTSP_PORT    RTSP port\n"
            << "  UVNVR_PRIVATE_PORT Private protocol TCP+QUIC port (default: 1515)\n"
            << "  UVNVR_PRIVATE_TCP  0 disables the TCP listener\n"
            << "  UVNVR_PRIVATE_QUIC 0 disables the QUIC listener\n"
            << "  UVNVR_DATA_DIR     Data directory\n"
            << "  UVNVR_FFMPEG       FFmpeg executable\n"
            << "  UVNVR_CACHE_MB     Playback LRU cache size in MiB (default: 128)\n"
            << "  UVNVR_READAHEAD_MB Next-segment read-ahead in MiB (default: 4)\n"
            << "  UVNVR_TLS_CERT     PEM certificate; with UVNVR_TLS_KEY enables HTTPS\n"
            << "  UVNVR_TLS_KEY      PEM private key\n"
            << "  UVNVR_CONFIG_INTEGRITY  off, warn or enforce\n"
            << "  UVNVR_CONFIG_MANIFEST   Integrity manifest path\n"
            << "  UVNVR_CONFIG_PUBLIC_KEY SM2 public key or certificate path\n";
}

bool ParsePort(std::string_view text, int* port) {  int value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value < 1 ||
      value > 65535) {
    return false;
  }
  *port = value;
  return true;
}

struct Overrides {
  std::optional<int> port;
  std::optional<int> rtsp_port;
  std::optional<int> private_port;
  std::optional<std::string> host;
  std::optional<std::string> data_dir;
  std::optional<std::string> tls_cert;
  std::optional<std::string> tls_key;
  std::optional<std::string> ffmpeg_path;
  std::optional<std::string> log_level;
  std::optional<bool> tls_self_signed;
  std::optional<bool> maintenance;
  std::string config_path;
  bool config_set = false;
};

bool TakeValue(int argc, char** argv, int* index, const char* option, std::string* value) {
  if (*index + 1 >= argc) {
    std::cerr << option << " requires a value\n";
    return false;
  }
  *value = argv[++(*index)];
  return true;
}

void ApplyEnvironment(uvnvr::Config* config) {
  if (const char* port = std::getenv("UVNVR_PORT")) {
    int value = 0;
    if (ParsePort(port, &value))
      config->port = value;
    else
      std::cerr << "ignoring invalid UVNVR_PORT: " << port << "\n";
  }
  if (const char* rtsp = std::getenv("UVNVR_RTSP_PORT")) {
    int value = 0;
    if (ParsePort(rtsp, &value))
      config->rtsp_port = value;
    else
      std::cerr << "ignoring invalid UVNVR_RTSP_PORT: " << rtsp << "\n";
  }
  if (const char* private_port = std::getenv("UVNVR_PRIVATE_PORT")) {
    int value = 0;
    if (ParsePort(private_port, &value))
      config->private_port = value;
    else
      std::cerr << "ignoring invalid UVNVR_PRIVATE_PORT: " << private_port << "\n";
  }
  const auto read_bool_env = [](const char* name, bool* target) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
      return;
    const std::string_view text(value);
    if (text == "0" || text == "false" || text == "no")
      *target = false;
    else if (text == "1" || text == "true" || text == "yes")
      *target = true;
    else
      std::cerr << "ignoring invalid " << name << ": " << value << "\n";
  };
  read_bool_env("UVNVR_PRIVATE_TCP", &config->private_tcp_enabled);
  read_bool_env("UVNVR_PRIVATE_QUIC", &config->private_quic_enabled);
  if (const char* host = std::getenv("UVNVR_HOST"); host != nullptr && host[0] != '\0')
    config->host = host;
  if (const char* data_dir = std::getenv("UVNVR_DATA_DIR");
      data_dir != nullptr && data_dir[0] != '\0')
    config->data_dir = data_dir;
  if (const char* ffmpeg = std::getenv("UVNVR_FFMPEG"); ffmpeg != nullptr && ffmpeg[0] != '\0')
    config->ffmpeg_path = ffmpeg;
  if (const char* log_level = std::getenv("UVNVR_LOG_LEVEL");
      log_level != nullptr && log_level[0] != '\0')
    config->log_level = log_level;
  const auto read_int_env = [](const char* name, int* target) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
      return;
    int parsed = 0;
    const auto result = std::from_chars(value, value + std::strlen(value), parsed);
    if (result.ec == std::errc() && result.ptr == value + std::strlen(value))
      *target = parsed;
    else
      std::cerr << "ignoring invalid " << name << ": " << value << "\n";
  };
  read_int_env("UVNVR_CACHE_MB", &config->cache_mb);
  read_int_env("UVNVR_READAHEAD_MB", &config->readahead_mb);
  if (const char* cert = std::getenv("UVNVR_TLS_CERT"); cert != nullptr && cert[0] != '\0')
    config->tls_cert = cert;
  if (const char* key = std::getenv("UVNVR_TLS_KEY"); key != nullptr && key[0] != '\0')
    config->tls_key = key;
}

void ApplyOverrides(const Overrides& overrides, uvnvr::Config* config) {
  if (overrides.port)
    config->port = *overrides.port;
  if (overrides.rtsp_port)
    config->rtsp_port = *overrides.rtsp_port;
  if (overrides.private_port)
    config->private_port = *overrides.private_port;
  if (overrides.host)
    config->host = *overrides.host;
  if (overrides.data_dir)
    config->data_dir = *overrides.data_dir;
  if (overrides.tls_cert)
    config->tls_cert = *overrides.tls_cert;
  if (overrides.tls_key)
    config->tls_key = *overrides.tls_key;
  if (overrides.ffmpeg_path)
    config->ffmpeg_path = *overrides.ffmpeg_path;
  if (overrides.log_level)
    config->log_level = *overrides.log_level;
  if (overrides.tls_self_signed)
    config->tls_self_signed = *overrides.tls_self_signed;
  if (overrides.maintenance)
    config->maintenance = *overrides.maintenance;
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  Overrides overrides;
  std::string config_error;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    std::string value;
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--version" || arg == "-v") {
      options.version = true;
    } else if (arg == "--print-config") {
      options.print_config = true;
    } else if (arg == "--config") {
      if (!TakeValue(argc, argv, &i, "--config", &overrides.config_path)) {
        options.help = true;
      } else {
        overrides.config_set = true;
      }
    } else if (arg == "--port") {
      int port = 0;
      if (!TakeValue(argc, argv, &i, "--port", &value) || !ParsePort(value, &port)) {
        if (!value.empty())
          std::cerr << "invalid port: " << value << "\n";
        options.help = true;
      } else {
        overrides.port = port;
      }
    } else if (arg == "--host") {
      if (!TakeValue(argc, argv, &i, "--host", &value))
        options.help = true;
      else
        overrides.host = value;
    } else if (arg == "--rtsp-port") {
      int port = 0;
      if (!TakeValue(argc, argv, &i, "--rtsp-port", &value) || !ParsePort(value, &port)) {
        if (!value.empty())
          std::cerr << "invalid RTSP port: " << value << "\n";
        options.help = true;
      } else {
        overrides.rtsp_port = port;
      }
    } else if (arg == "--private-port") {
      int port = 0;
      if (!TakeValue(argc, argv, &i, "--private-port", &value) || !ParsePort(value, &port)) {
        if (!value.empty())
          std::cerr << "invalid private port: " << value << "\n";
        options.help = true;
      } else {
        overrides.private_port = port;
      }
    } else if (arg == "--data-dir") {
      if (!TakeValue(argc, argv, &i, "--data-dir", &value))
        options.help = true;
      else
        overrides.data_dir = value;
    } else if (arg == "--tls-cert") {
      if (!TakeValue(argc, argv, &i, "--tls-cert", &value))
        options.help = true;
      else
        overrides.tls_cert = value;
    } else if (arg == "--tls-key") {
      if (!TakeValue(argc, argv, &i, "--tls-key", &value))
        options.help = true;
      else
        overrides.tls_key = value;
    } else if (arg == "--tls-self-signed") {
      overrides.tls_self_signed = true;
    } else if (arg == "--ffmpeg") {
      if (!TakeValue(argc, argv, &i, "--ffmpeg", &value))
        options.help = true;
      else
        overrides.ffmpeg_path = value;
    } else if (arg == "--log-level") {
      if (!TakeValue(argc, argv, &i, "--log-level", &value))
        options.help = true;
      else
        overrides.log_level = value;
    } else if (arg == "--maintenance") {
      overrides.maintenance = true;
    } else if (arg == "--service") {
      options.service = true;
    } else if (arg == "--open") {
      options.open_browser = true;
    } else if (arg == "--verify-audit") {
      options.verify_audit = true;
    } else if (arg == "--verify-firmware") {
      if (!TakeValue(argc, argv, &i, "--verify-firmware", &value) || value.empty()) {
        options.help = true;
      } else {
        options.verify_firmware = value;
        if (!TakeValue(argc, argv, &i, "--verify-firmware", &value) || value.empty())
          options.help = true;
        else
          options.firmware_key = value;
      }
    } else if (arg == "--config-integrity") {
      if (!TakeValue(argc, argv, &i, "--config-integrity", &value))
        options.help = true;
      else
        options.config_integrity_mode = value;
    } else if (arg == "--config-manifest") {
      if (!TakeValue(argc, argv, &i, "--config-manifest", &value))
        options.help = true;
      else
        options.config_manifest_path = value;
    } else if (arg == "--config-public-key") {
      if (!TakeValue(argc, argv, &i, "--config-public-key", &value))
        options.help = true;
      else
        options.config_public_key = value;
    } else if (arg == "--verify-config") {
      options.verify_config = true;
    } else if (arg == "--write-config-digest") {
      options.write_config_digest = true;
    } else if (arg == "--sign-config") {
      if (!TakeValue(argc, argv, &i, "--sign-config", &value))
        options.help = true;
      else {
        options.sign_config = true;
        options.config_private_key = value;
      }
    } else if (arg == "--service-action") {
      if (!TakeValue(argc, argv, &i, "--service-action", &value))
        options.help = true;
      else
        options.service_action = value;
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      options.help = true;
    }
  }

  if (options.help || options.version)
    return options;

#ifndef UVNVR_ENABLE_CONFIG_INTEGRITY
  // The flags exist in every build so scripts get a precise message instead of
  // "unknown option"; the actual behaviour needs the compiled-in module.
  if (options.verify_config || options.write_config_digest || options.sign_config ||
      !options.config_integrity_mode.empty() || !options.config_manifest_path.empty() ||
      !options.config_public_key.empty()) {
    std::cerr << "configuration error: the configuration integrity options require a build "
                 "with UVNVR_ENABLE_CONFIG_INTEGRITY\n";
    options.fatal = true;
    return options;
  }
#else
  {
    std::string mode_text = options.config_integrity_mode;
    if (mode_text.empty()) {
      if (const char* env = std::getenv("UVNVR_CONFIG_INTEGRITY");
          env != nullptr && env[0] != '\0')
        mode_text = env;
    }
    if (!mode_text.empty() &&
        !uvnvr::secure::ParseConfigIntegrityMode(mode_text, &options.integrity_mode)) {
      std::cerr << "configuration error: configuration integrity mode must be off, warn or "
                   "enforce\n";
      options.fatal = true;
      return options;
    }
    if (options.config_manifest_path.empty()) {
      if (const char* env = std::getenv("UVNVR_CONFIG_MANIFEST");
          env != nullptr && env[0] != '\0')
        options.config_manifest_path = env;
    }
    if (options.config_public_key.empty()) {
      if (const char* env = std::getenv("UVNVR_CONFIG_PUBLIC_KEY");
          env != nullptr && env[0] != '\0')
        options.config_public_key = env;
    }
  }
#endif

  // Configuration file: explicit paths are errors when unreadable; discovered
  // candidates are silently skipped when absent.
  bool data_dir_set = false;
  std::string config_path;
  bool config_required = false;
  if (overrides.config_set) {
    config_path = overrides.config_path;
    config_required = true;
    if (config_path.empty()) {
      std::cerr << "configuration error: --config requires a non-empty path\n";
      options.fatal = true;
      return options;
    }
  } else if (const char* env = std::getenv("UVNVR_CONFIG"); env != nullptr && env[0] != '\0') {
    config_path = env;
    config_required = true;
  } else {
    for (const std::string& candidate : uvnvr::DefaultConfigCandidates()) {
      if (std::filesystem::exists(candidate)) {
        config_path = candidate;
        break;
      }
    }
  }

  if (!config_path.empty()) {
    const std::string default_data_dir = options.config.data_dir;
    std::string read_error;
    if (!ReadTextFile(config_path, &options.config_file_contents, &read_error)) {
      if (config_required || std::filesystem::exists(config_path)) {
        std::cerr << "configuration error: " << read_error << "\n";
        options.fatal = true;
        return options;
      }
    } else {
      options.config_file_path = config_path;
      options.config_file_read = true;
      uvnvr::ConfigLoadReport load_report;
      if (!uvnvr::LoadConfigText(options.config_file_contents, config_path, &options.config,
                                 &config_error, &load_report)) {
        std::cerr << "configuration error: " << config_error << "\n";
        options.fatal = true;
        return options;
      }
      if (options.config.data_dir != default_data_dir)
        data_dir_set = true;
#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
      if (options.integrity_mode != uvnvr::secure::ConfigIntegrityMode::kOff) {
        for (const std::string& key : load_report.unknown_keys)
          std::cerr << "configuration warning: unknown key \"" << key << "\" (ignored)\n";
      }
#endif
    }
  }

  const std::string before_env_data_dir = options.config.data_dir;
  ApplyEnvironment(&options.config);
  if (options.config.data_dir != before_env_data_dir)
    data_dir_set = true;
  if (overrides.data_dir)
    data_dir_set = true;
  ApplyOverrides(overrides, &options.config);
  if (!data_dir_set)
    options.config.data_dir = DefaultDataDir();

  if (!uvnvr::ValidateConfig(options.config, &config_error)) {
    std::cerr << "configuration error: " << config_error << "\n";
    options.fatal = true;
  }

#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
  // Explicit --verify-config / signing modes print their own diagnostics and
  // must not be pre-empted by the startup policy.
  const bool integrity_action =
      options.verify_config || options.write_config_digest || options.sign_config;
  if (!options.fatal && options.config_file_read && !integrity_action &&
      options.integrity_mode != uvnvr::secure::ConfigIntegrityMode::kOff) {
    const std::string manifest_path = ResolveConfigManifestPath(options);
    std::string public_key;
    std::string key_error;
    bool key_ok = true;
    if (!options.config_public_key.empty())
      key_ok = ReadTextFile(options.config_public_key, &public_key, &key_error);

    uvnvr::secure::ConfigIntegrityResult result;
    std::string integrity_error;
    bool performed = false;
    if (key_ok) {
      performed = uvnvr::secure::VerifyConfigText(options.config_file_contents, manifest_path,
                                                  public_key, &result, &integrity_error);
    } else {
      integrity_error = key_error;
    }

    const auto report_failure = [&](const std::string& detail) {
      const std::string message =
          "config integrity: " + detail + " (manifest: " + manifest_path + ")";
      if (options.integrity_mode == uvnvr::secure::ConfigIntegrityMode::kEnforce) {
        std::cerr << "configuration error: " << message << "; refusing to start (mode=enforce)\n";
        options.fatal = true;
      } else {
        std::cerr << "configuration warning: " << message << "; continuing (mode=warn)\n";
      }
    };

    if (!performed) {
      report_failure(integrity_error.empty() ? "verification could not be performed"
                                             : integrity_error);
    } else if (!result.ok) {
      std::string detail = result.message;
      if (!result.expected_sm3.empty() || !result.actual_sm3.empty()) {
        detail += " [expected sm3 " + result.expected_sm3 + ", actual " + result.actual_sm3 +
                  "]";
      }
      report_failure(detail);
    }
  }
#endif
  return options;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // The service logs and help text contain UTF-8 Chinese; make the console
  // display it instead of mojibake.
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  // Must happen before the first work item reaches libuv's thread pool.
  (void)uvcpp::uvcpp_set_threadpool_size(0);

  const Options options = ParseOptions(argc, argv);
  if (options.version) {
    std::cout << UVNVR_PRODUCT_NAME << " " << UVNVR_VERSION << " (" << UVNVR_GIT_SHA << ")\n";
    return 0;
  }
  if (options.help) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (options.fatal)
    return 1;
  if (options.print_config) {
    std::cout << uvnvr::ConfigToJson(options.config).dump(2) << "\n";
    return 0;
  }

  if (options.verify_config) {
#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
    if (!options.config_file_read) {
      std::cerr << "config verify: no configuration file was loaded; use --config\n";
      return 2;
    }
    std::string public_key;
    std::string error;
    if (!options.config_public_key.empty() &&
        !ReadTextFile(options.config_public_key, &public_key, &error)) {
      std::cerr << "config verify: " << error << "\n";
      return 2;
    }
    uvnvr::secure::ConfigIntegrityResult result;
    if (!uvnvr::secure::VerifyConfigText(options.config_file_contents,
                                         ResolveConfigManifestPath(options), public_key, &result,
                                         &error)) {
      std::cerr << "config verify: " << error << "\n";
      return 2;
    }
    if (!result.ok) {
      std::cout << "FAIL " << options.config_file_path << ": " << result.message << "\n"
                << "  manifest:    " << ResolveConfigManifestPath(options) << "\n"
                << "  expected sm3: " << result.expected_sm3 << "\n"
                << "  actual sm3:   " << result.actual_sm3 << "\n";
      return 1;
    }
    std::cout << "OK " << options.config_file_path << ": sm3=" << result.actual_sm3;
    if (result.signed_manifest)
      std::cout << " (SM2 signature verified)";
    std::cout << "\n";
    return 0;
#else
    std::cerr << "config verify: this build was configured without "
                 "UVNVR_ENABLE_CONFIG_INTEGRITY\n";
    return 2;
#endif
  }

  if (options.write_config_digest || options.sign_config) {
#ifdef UVNVR_ENABLE_CONFIG_INTEGRITY
    if (!options.config_file_read) {
      std::cerr << "config manifest: no configuration file was loaded; use --config\n";
      return 2;
    }
    std::string private_key;
    std::string error;
    if (options.sign_config &&
        !ReadTextFile(options.config_private_key, &private_key, &error)) {
      std::cerr << "config manifest: " << error << "\n";
      return 2;
    }
    const std::string manifest_path = ResolveConfigManifestPath(options);
    if (!uvnvr::secure::WriteConfigManifestForContents(options.config_file_contents, manifest_path,
                                                       private_key, &error)) {
      std::cerr << "config manifest: " << error << "\n";
      return 2;
    }
    std::cout << "config manifest " << (options.sign_config ? "signed with SM2" : "written")
              << ": " << manifest_path << "\n";
    return 0;
#else
    std::cerr << "config manifest: this build was configured without "
                 "UVNVR_ENABLE_CONFIG_INTEGRITY\n";
    return 2;
#endif
  }

  if (options.verify_audit) {
#ifdef UVNVR_ENABLE_SECURE
    std::string db_path = options.config.db_path;
    if (db_path.empty())
      db_path = (std::filesystem::path(options.config.data_dir) / "uvnvr.db").string();
    uvnvr::secure::AuditVerification verification;
    std::string error;
    if (!uvnvr::secure::VerifyAuditDatabase(db_path, &verification, &error)) {
      std::cerr << "audit verify: " << error << "\n";
      return 2;
    }
    if (!verification.ok) {
      std::cout << "FAIL " << db_path << ": " << verification.message << "\n"
                << "  broken index: " << verification.broken_index
                << "  entry id: " << verification.broken_id << "\n";
      if (!verification.expected_hash.empty())
        std::cout << "  expected hash: " << verification.expected_hash << "\n";
      if (!verification.actual_hash.empty())
        std::cout << "  actual hash:   " << verification.actual_hash << "\n";
      return 1;
    }
    std::cout << "OK " << db_path << ": " << verification.checked << " chained entries verified";
    if (verification.skipped_legacy > 0)
      std::cout << " (" << verification.skipped_legacy << " legacy rows skipped)";
    std::cout << "\n";
    return 0;
#else
    std::cerr << "audit verify: this build was configured without UVNVR_ENABLE_SECURE\n";
    return 2;
#endif
  }

  if (!options.verify_firmware.empty()) {
#ifdef UVNVR_ENABLE_SECURE
    std::string key_pem;
    std::string error;
    if (!ReadTextFile(options.firmware_key, &key_pem, &error)) {
      std::cerr << "firmware verify: " << error << "\n";
      return 2;
    }
    uvnvr::secure::FirmwarePackage package;
    if (!uvnvr::secure::VerifyFirmwareFile(options.verify_firmware, key_pem, "", &package,
                                           &error)) {
      std::cerr << "firmware verify: " << error << "\n";
      return 1;
    }
    std::cout << "OK product=" << package.product << " version=" << package.version
              << " payload=" << package.payload.size() << " bytes"
              << " sm3=" << package.payload_sm3 << "\n";
    return 0;
#else
    std::cerr << "firmware verify: this build was configured without UVNVR_ENABLE_SECURE\n";
    return 2;
#endif
  }

  if (!options.service_action.empty()) {
    uvnvr::service::PerformAction(options.service_action, options.config);
    return 0;
  }

  uvnvr::App app(options.config);
  app.SetOpenBrowser(options.open_browser);
#ifdef _WIN32
  if (options.service) {
    // The SCM owns this process: run the normal startup under a service
    // control handler so Stop and shutdown reach the web loop. Startup is
    // reported as RUNNING only after the web and RTSP listeners are ready.
    app.SetStartedCallback([] { uvnvr::service::NotifyStarted(); });
    return uvnvr::service::RunWindowsService(
        options.config, {[&app] { return app.Run(); }, [&app] { app.RequestShutdown(); }});
  }
#endif
  return app.Run();
}
