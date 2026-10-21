#include "app.h"
#include "config.h"
#include "platform.h"
#include "service.h"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
  std::string service_action;
};

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
            << "  UVNVR_DATA_DIR     Data directory\n"
            << "  UVNVR_FFMPEG       FFmpeg executable\n"
            << "  UVNVR_CACHE_MB     Playback LRU cache size in MiB (default: 128)\n"
            << "  UVNVR_READAHEAD_MB Next-segment read-ahead in MiB (default: 4)\n"
            << "  UVNVR_TLS_CERT     PEM certificate; with UVNVR_TLS_KEY enables HTTPS\n"
            << "  UVNVR_TLS_KEY      PEM private key\n";
}

bool ParsePort(std::string_view text, int* port) {
  int value = 0;
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
    if (!uvnvr::LoadConfigFile(config_path, &options.config, &config_error)) {
      if (config_required || std::filesystem::exists(config_path)) {
        std::cerr << "configuration error: " << config_error << "\n";
        options.fatal = true;
        return options;
      }
    } else if (options.config.data_dir != default_data_dir) {
      data_dir_set = true;
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
