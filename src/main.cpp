#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <uvcpp/uvcpp_threadpool.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <uvnvr/version.h>

namespace {

struct Options {
  int port = 8080;
  bool help = false;
  bool version = false;
};

void PrintUsage(const char* argv0) {
  std::cout << "UvNVR " << UVNVR_VERSION << " (" << UVNVR_GIT_SHA << ")\n\n"
            << "Usage: " << argv0 << " [options]\n\n"
            << "Options:\n"
            << "  --port <port>   HTTP/HTTPS listen port (default: 8080)\n"
            << "  --version       Print version and exit\n"
            << "  --help          Print this help and exit\n\n"
            << "Environment:\n"
            << "  UVNVR_TLS_CERT  PEM certificate; with UVNVR_TLS_KEY enables HTTPS\n"
            << "  UVNVR_TLS_KEY   PEM private key\n";
}

bool ParsePort(std::string_view text, int* port) {
  int value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      value < 1 || value > 65535) {
    return false;
  }
  *port = value;
  return true;
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--version" || arg == "-v") {
      options.version = true;
    } else if (arg == "--port") {
      if (i + 1 >= argc) {
        std::cerr << "--port requires a value\n";
        options.help = true;
      } else if (!ParsePort(argv[++i], &options.port)) {
        std::cerr << "invalid port: " << argv[i] << "\n";
        options.help = true;
      }
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      options.help = true;
    }
  }
  return options;
}

std::string HealthJson() {
  return std::string(R"({"status":"ok","name":")") + UVNVR_PRODUCT_NAME +
         R"(","version":")" + UVNVR_VERSION + R"(","commit":")" + UVNVR_GIT_SHA + R"("})";
}

}  // namespace

int main(int argc, char** argv) {
  // Must happen before the first work item reaches libuv's thread pool. The
  // default of four threads is shared by file I/O, DNS and the SQLite pool.
  (void)uvcpp::uvcpp_set_threadpool_size(0);  // 0 = size from the CPU count

  const Options options = ParseOptions(argc, argv);
  if (options.version) {
    std::cout << UVNVR_PRODUCT_NAME << " " << UVNVR_VERSION << " (" << UVNVR_GIT_SHA << ")\n";
    return 0;
  }
  if (options.help) {
    PrintUsage(argv[0]);
    return 0;
  }

  uvcpp::uvcpp_web_app app;
  app.set_port(options.port);
  // h1 and h2c share this port; TLS is enabled only when a real pair is given.
  app.set_h2c_enabled(true);

  const char* cert_file = std::getenv("UVNVR_TLS_CERT");
  const char* key_file = std::getenv("UVNVR_TLS_KEY");
  const bool has_cert = cert_file != nullptr && cert_file[0] != '\0';
  const bool has_key = key_file != nullptr && key_file[0] != '\0';
  if (has_cert != has_key) {
    std::cerr << "UVNVR_TLS_CERT and UVNVR_TLS_KEY must be set together\n";
    return 1;
  }
  if (has_cert && has_key) {
    app.enable_ssl(cert_file, key_file);
    if (!app.ssl_error().empty()) {
      std::cerr << "cannot enable TLS: " << app.ssl_error() << "\n";
      return 1;
    }
    std::cout << "TLS enabled with " << cert_file << "\n";
  }

  app.get("/healthz", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                         uvcpp::uvcpp_web_next) {
    resp.status(200).json_str(HealthJson());
    resp.end();
  });

  app.get("/api/v1/system/info", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                                    uvcpp::uvcpp_web_next) {
    resp.status(200).json_str(HealthJson());
    resp.end();
  });

  // The frontend is a Vite build. In development `frontend/dist` may not exist
  // yet; say so instead of registering a document root that cannot be served.
  std::ifstream frontend_entry("./frontend/dist/index.html");
  if (frontend_entry.good()) {
    uvcpp::uvcpp_web_static_options frontend_options;
    frontend_options.spa_fallback = true;
    app.serve_static("/", "./frontend/dist", frontend_options);
  } else {
    app.get("/", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                    uvcpp::uvcpp_web_next) {
      resp.status(200).body(
          "UvNVR is running. The frontend has not been built yet.\n"
          "Run `npm ci && npm run build` in frontend/, then restart the service.\n",
          "text/plain; charset=utf-8");
      resp.end();
    });
  }

  const int rc = app.start();
  if (rc != 0) {
    std::cerr << "failed to start on port " << options.port << ": libuv error " << rc << "\n";
    return 1;
  }

  std::cout << "UvNVR " << UVNVR_VERSION << " listening on "
            << (has_cert ? "https" : "http") << "://127.0.0.1:" << app.bound_port() << "\n";
  std::cout << "health: " << (has_cert ? "https" : "http") << "://127.0.0.1:"
            << app.bound_port() << "/healthz\n"
            << std::flush;

  app.join();
  return 0;
}
