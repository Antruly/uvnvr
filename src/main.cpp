#include "app.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <uvcpp/uvcpp_threadpool.h>
#include <uvnvr/version.h>

namespace {

struct Options {
  uvnvr::Config config;
  bool help = false;
  bool version = false;
};

void PrintUsage(const char* argv0) {
  std::cout << "UvNVR " << UVNVR_VERSION << " (" << UVNVR_GIT_SHA << ")\n\n"
            << "Usage: " << argv0 << " [options]\n\n"
            << "Options:\n"
            << "  --port <port>      HTTP/HTTPS listen port (default: 8080)\n"
            << "  --data-dir <path>  Data directory (default: ./data)\n"
            << "  --tls-self-signed  Serve HTTPS with a generated localhost certificate\n"
            << "  --maintenance      Enable maintenance mode\n"
            << "  --version          Print version and exit\n"
            << "  --help             Print this help and exit\n\n"
            << "Environment:\n"
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

Options ParseOptions(int argc, char** argv) {
  Options options;
  if (const char* cert = std::getenv("UVNVR_TLS_CERT"))
    options.config.tls_cert = cert;
  if (const char* key = std::getenv("UVNVR_TLS_KEY"))
    options.config.tls_key = key;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--version" || arg == "-v") {
      options.version = true;
    } else if (arg == "--tls-self-signed") {
      options.config.tls_self_signed = true;
    } else if (arg == "--maintenance") {
      options.config.maintenance = true;
    } else if (arg == "--port") {
      if (i + 1 >= argc) {
        std::cerr << "--port requires a value\n";
        options.help = true;
      } else if (!ParsePort(argv[++i], &options.config.port)) {
        std::cerr << "invalid port: " << argv[i] << "\n";
        options.help = true;
      }
    } else if (arg == "--data-dir") {
      if (i + 1 >= argc) {
        std::cerr << "--data-dir requires a value\n";
        options.help = true;
      } else {
        options.config.data_dir = argv[++i];
      }
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      options.help = true;
    }
  }

  const bool has_cert = !options.config.tls_cert.empty();
  const bool has_key = !options.config.tls_key.empty();
  if (has_cert != has_key) {
    std::cerr << "UVNVR_TLS_CERT and UVNVR_TLS_KEY must be set together\n";
    options.help = true;
  }
  return options;
}

} // namespace

int main(int argc, char** argv) {
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

  uvnvr::App app(options.config);
  return app.Run();
}
