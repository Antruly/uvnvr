// sip_reg_cli: standalone smoke test for the GB/T 28181 SIP user agent.
//
// It runs the registration state machine against a local fake registrar and
// prints every SIP message on the wire, every state transition and every
// retry, so the registration/heartbeat/backoff behaviour can be checked
// without a platform or packet capture.
//
// Example:
//   sip_reg_cli --server-host 127.0.0.1 --server-port 5060 \
//       --username 34020000001320000001 --password 12345678 \
//       --keepalive 5 --request-timeout-ms 2000 --duration 12

#include "gb28181/sip.h"

#include <handle/uvcpp_loop.h>
#include <uv.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace uvnvr::gb28181;

struct Options {
  SipUaConfig config;
  int duration_seconds = 0;
};

std::string Timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min,
                local.tm_sec, static_cast<int>(ms.count()));
  return buffer;
}

void PrintRaw(const char* direction, const std::string& transport, const std::string& peer,
              const std::string& raw) {
  std::cout << "[" << Timestamp() << "] " << direction;
  if (!transport.empty())
    std::cout << " " << transport;
  if (!peer.empty())
    std::cout << " " << peer;
  std::cout << " (" << raw.size() << " bytes)\n";

  std::size_t pos = 0;
  while (pos <= raw.size()) {
    const std::size_t newline = raw.find('\n', pos);
    std::string_view line(raw);
    if (newline == std::string::npos) {
      line = line.substr(pos);
    } else {
      line = line.substr(pos, newline - pos);
    }
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    std::cout << "    " << line << "\n";
    if (newline == std::string::npos)
      break;
    pos = newline + 1;
  }
  std::cout.flush();
}

bool ParseUint(const char* text, std::uint32_t max, std::uint32_t* out) {
  if (text == nullptr || *text == '\0')
    return false;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == nullptr || *end != '\0' || value > max)
    return false;
  *out = static_cast<std::uint32_t>(value);
  return true;
}

void Usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "  --server-host HOST        SIP registrar address (default 127.0.0.1)\n"
      "  --server-port PORT        SIP registrar port (default 5060)\n"
      "  --tcp                     use SIP over TCP instead of UDP\n"
      "  --username ID             device SIP ID (default 34020000001320000001)\n"
      "  --password PASSWORD       Digest password (default 12345678)\n"
      "  --domain DOMAIN           SIP domain (default: server host)\n"
      "  --server-id ID            platform ID used in MESSAGE To/Request-URI\n"
      "  --registrar URI           full registrar Request-URI\n"
      "  --local-ip IP             address advertised in Via/Contact\n"
      "  --local-port PORT         local port (0 = ephemeral)\n"
      "  --bind-ip IP              local bind address (default 0.0.0.0)\n"
      "  --expires SECONDS         registration lifetime (default 3600)\n"
      "  --keepalive SECONDS       keepalive interval, 0 disables (default 60)\n"
      "  --request-timeout-ms MS   REGISTER/MESSAGE timeout (default 32000)\n"
      "  --connect-timeout-ms MS   TCP connect timeout (default 10000)\n"
      "  --initial-backoff-ms MS   first reconnect delay (default 1000)\n"
      "  --max-backoff-ms MS       backoff cap (default 60000)\n"
      "  --duration SECONDS        stop after N seconds, 0 = until interrupted\n"
      "  --help                    show this help\n",
      argv0);
}

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
  options->config.server_host = "127.0.0.1";
  options->config.username = "34020000001320000001";
  options->config.password = "12345678";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        *error = std::string("missing value for ") + name;
        return nullptr;
      }
      return argv[++i];
    };
    auto next_uint = [&](const char* name, std::uint32_t* out) -> bool {
      const char* value = next(name);
      if (value == nullptr)
        return false;
      if (!ParseUint(value, 0xffffffffu, out)) {
        *error = std::string("invalid number for ") + name;
        return false;
      }
      return true;
    };

    if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else if (arg == "--tcp") {
      options->config.tcp = true;
    } else if (arg == "--server-host") {
      const char* value = next("--server-host");
      if (value == nullptr)
        return false;
      options->config.server_host = value;
    } else if (arg == "--server-port") {
      std::uint32_t value = 0;
      if (!next_uint("--server-port", &value))
        return false;
      if (value == 0 || value > 65535) {
        *error = "--server-port must be 1..65535";
        return false;
      }
      options->config.server_port = static_cast<std::uint16_t>(value);
    } else if (arg == "--username") {
      const char* value = next("--username");
      if (value == nullptr)
        return false;
      options->config.username = value;
    } else if (arg == "--password") {
      const char* value = next("--password");
      if (value == nullptr)
        return false;
      options->config.password = value;
    } else if (arg == "--domain") {
      const char* value = next("--domain");
      if (value == nullptr)
        return false;
      options->config.domain = value;
    } else if (arg == "--server-id") {
      const char* value = next("--server-id");
      if (value == nullptr)
        return false;
      options->config.server_id = value;
    } else if (arg == "--registrar") {
      const char* value = next("--registrar");
      if (value == nullptr)
        return false;
      options->config.registrar_uri = value;
    } else if (arg == "--local-ip") {
      const char* value = next("--local-ip");
      if (value == nullptr)
        return false;
      options->config.local_ip = value;
    } else if (arg == "--local-port") {
      std::uint32_t value = 0;
      if (!next_uint("--local-port", &value))
        return false;
      if (value > 65535) {
        *error = "--local-port must be 0..65535";
        return false;
      }
      options->config.local_port = static_cast<std::uint16_t>(value);
    } else if (arg == "--bind-ip") {
      const char* value = next("--bind-ip");
      if (value == nullptr)
        return false;
      options->config.bind_ip = value;
    } else if (arg == "--expires") {
      std::uint32_t value = 0;
      if (!next_uint("--expires", &value))
        return false;
      options->config.expires_seconds = value;
    } else if (arg == "--keepalive") {
      std::uint32_t value = 0;
      if (!next_uint("--keepalive", &value))
        return false;
      options->config.keepalive_seconds = value;
    } else if (arg == "--request-timeout-ms") {
      std::uint32_t value = 0;
      if (!next_uint("--request-timeout-ms", &value))
        return false;
      options->config.request_timeout_ms = value;
    } else if (arg == "--connect-timeout-ms") {
      std::uint32_t value = 0;
      if (!next_uint("--connect-timeout-ms", &value))
        return false;
      options->config.connect_timeout_ms = value;
    } else if (arg == "--initial-backoff-ms") {
      std::uint32_t value = 0;
      if (!next_uint("--initial-backoff-ms", &value))
        return false;
      options->config.initial_backoff_ms = value;
    } else if (arg == "--max-backoff-ms") {
      std::uint32_t value = 0;
      if (!next_uint("--max-backoff-ms", &value))
        return false;
      options->config.max_backoff_ms = value;
    } else if (arg == "--duration") {
      std::uint32_t value = 0;
      if (!next_uint("--duration", &value))
        return false;
      options->duration_seconds = static_cast<int>(value);
    } else {
      *error = "unknown argument: " + std::string(arg);
      return false;
    }
  }

  if (options->config.server_port == 0)
    options->config.server_port = 5060;
  return true;
}

struct LoopStopContext {
  uvcpp::uvcpp_loop* loop = nullptr;
  uv_timer_t* timer = nullptr;
  uv_signal_t* signals[2] = {nullptr, nullptr};
};

void OnExitTimer(uv_timer_t* handle) {
  auto* context = static_cast<LoopStopContext*>(handle->data);
  if (context != nullptr && context->loop != nullptr)
    context->loop->stop();
}

void OnSignal(uv_signal_t* handle, int /*signum*/) {
  auto* context = static_cast<LoopStopContext*>(handle->data);
  if (context != nullptr && context->loop != nullptr)
    context->loop->stop();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseArgs(argc, argv, &options, &error)) {
    std::fprintf(stderr, "sip_reg_cli: %s\n", error.c_str());
    Usage(argv[0]);
    return 1;
  }

  uvcpp::uvcpp_loop* loop = uvcpp::uvcpp_loop::default_loop();
  auto* raw_loop = reinterpret_cast<uv_loop_t*>(loop->get_handle());

  LoopStopContext stop_context;
  stop_context.loop = loop;

  if (options.duration_seconds > 0) {
    stop_context.timer = new uv_timer_t();
    uv_timer_init(raw_loop, stop_context.timer);
    stop_context.timer->data = &stop_context;
    uv_timer_start(stop_context.timer, &OnExitTimer,
                   static_cast<std::uint64_t>(options.duration_seconds) * 1000, 0);
  }
  const int signal_numbers[2] = {SIGINT, SIGTERM};
  for (int i = 0; i < 2; ++i) {
    stop_context.signals[i] = new uv_signal_t();
    uv_signal_init(raw_loop, stop_context.signals[i]);
    stop_context.signals[i]->data = &stop_context;
    uv_signal_start(stop_context.signals[i], &OnSignal, signal_numbers[i]);
  }
  bool registered_once = false;
  bool start_failed = false;
  {
    SipUserAgent agent(loop, options.config);
    SipUaCallbacks callbacks;
    callbacks.on_event = [](const std::string& text) {
      std::cout << "[" << Timestamp() << "] event " << text << "\n";
      std::cout.flush();
    };
    const std::string transport = options.config.tcp ? "tcp" : "udp";
    callbacks.on_outbound = [&transport](const std::string& raw) {
      PrintRaw("TX", transport, {}, raw);
    };
    callbacks.on_inbound = [&transport](const std::string& raw, const std::string& peer) {
      PrintRaw("RX", transport, peer, raw);
    };
    callbacks.on_state = [&registered_once](SipRegistrationState state,
                                            const std::string& detail) {
      if (state == SipRegistrationState::kRegistered)
        registered_once = true;
      std::cout << "[" << Timestamp() << "] state=" << SipRegistrationStateName(state) << " "
                << detail << "\n";
      std::cout.flush();
    };
    agent.set_callbacks(std::move(callbacks));

    if (!agent.Start(&error)) {
      std::fprintf(stderr, "sip_reg_cli: %s\n", error.c_str());
      start_failed = true;
    } else {
      loop->run(UV_RUN_DEFAULT);
      agent.Stop();
      std::printf("[%s] summary: state=%s sent=%llu received=%llu\n", Timestamp().c_str(),
                  std::string(SipRegistrationStateName(agent.state())).c_str(),
                  static_cast<unsigned long long>(agent.sent_messages()),
                  static_cast<unsigned long long>(agent.received_messages()));
    }
  }

  if (stop_context.timer != nullptr) {
    uv_timer_stop(stop_context.timer);
    uv_close(reinterpret_cast<uv_handle_t*>(stop_context.timer), nullptr);
  }
  for (uv_signal_t* signal : stop_context.signals) {
    if (signal == nullptr)
      continue;
    uv_signal_stop(signal);
    uv_close(reinterpret_cast<uv_handle_t*>(signal), nullptr);
  }
  loop->run(UV_RUN_NOWAIT);

  if (start_failed)
    return 1;
  return registered_once ? 0 : 2;
}
