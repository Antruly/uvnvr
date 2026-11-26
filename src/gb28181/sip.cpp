// Minimal SIP user agent for GB/T 28181 registration and keepalive.
//
// This file contains three layers:
//   1. pure message helpers (URI, address, header, message, Digest) that have
//      no I/O and are covered by the unit tests;
//   2. UDP/TCP transports built directly on libuv, driven by the libuvcpp
//      event loop supplied by the caller;
//   3. the registration state machine (REGISTER, auth retry, refresh,
//      keepalive, exponential backoff).

#include "gb28181/sip.h"

#include <handle/uvcpp_loop.h>
#include <uv.h>

#include <openssl/evp.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace uvnvr::gb28181 {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

std::string_view Trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool HasControlChars(std::string_view text) {
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f)
      return true;
  }
  return false;
}

std::string SanitizeHeaderValue(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\r' || c == '\n')
      out.push_back(' ');
    else
      out.push_back(c);
  }
  return out;
}

std::string SanitizeHeaderName(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || c == ':' || u == 0x7f)
      continue;
    out.push_back(c);
  }
  return out;
}

bool ParseUint32(std::string_view text, std::uint32_t* value) {
  text = Trim(text);
  if (text.empty())
    return false;
  std::uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    result = result * 10 + static_cast<unsigned>(c - '0');
    if (result > std::numeric_limits<std::uint32_t>::max())
      return false;
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

std::string FormatHostPort(const std::string& host, std::uint16_t port) {
  std::string out;
  if (host.find(':') != std::string::npos)
    out = "[" + host + "]";
  else
    out = host.empty() ? "0.0.0.0" : host;
  if (port != 0) {
    out.push_back(':');
    out += std::to_string(port);
  }
  return out;
}

std::string MakeUri(const std::string& user, const std::string& host, std::uint16_t port = 0) {
  SipUri uri;
  uri.scheme = "sip";
  uri.user = user;
  uri.host = host;
  uri.port = port;
  return FormatSipUri(uri);
}

// Parses a list of ";name=value" parameters. Values may be quoted and may
// contain escaped characters. The input may or may not start with ';'.
bool ParseParameterList(std::string_view text, std::vector<std::pair<std::string, std::string>>* out,
                        std::string* error) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && (text[pos] == ';' || std::isspace(static_cast<unsigned char>(text[pos]))))
      ++pos;
    if (pos >= text.size())
      break;

    std::size_t end = pos;
    bool quoted = false;
    while (end < text.size()) {
      const char c = text[end];
      if (quoted) {
        if (c == '\\' && end + 1 < text.size()) {
          end += 2;
          continue;
        }
        if (c == '"')
          quoted = false;
      } else if (c == '"') {
        quoted = true;
      } else if (c == ';') {
        break;
      }
      ++end;
    }
    if (quoted) {
      if (error)
        *error = "unterminated quoted parameter";
      return false;
    }

    std::string_view item = text.substr(pos, end - pos);
    pos = end;

    const std::size_t eq = item.find('=');
    std::string name;
    std::string value;
    if (eq == std::string_view::npos) {
      name = std::string(Trim(item));
    } else {
      name = std::string(Trim(item.substr(0, eq)));
      std::string_view raw = Trim(item.substr(eq + 1));
      if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
        for (std::size_t i = 0; i < raw.size(); ++i) {
          if (raw[i] == '\\' && i + 1 < raw.size()) {
            ++i;
            value.push_back(raw[i]);
          } else {
            value.push_back(raw[i]);
          }
        }
      } else {
        value = std::string(raw);
      }
    }
    if (name.empty()) {
      if (error)
        *error = "empty parameter name";
      return false;
    }
    out->emplace_back(std::move(name), std::move(value));
  }
  return true;
}

// Splits an authentication parameter list on commas that are not inside
// quoted strings. Returns name/value pairs with lower-case names.
bool ParseAuthParameterList(std::string_view text,
                            std::vector<std::pair<std::string, std::string>>* out,
                            std::string* error) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() &&
           (text[pos] == ',' || std::isspace(static_cast<unsigned char>(text[pos]))))
      ++pos;
    if (pos >= text.size())
      break;

    std::size_t end = pos;
    bool quoted = false;
    while (end < text.size()) {
      const char c = text[end];
      if (quoted) {
        if (c == '\\' && end + 1 < text.size()) {
          end += 2;
          continue;
        }
        if (c == '"')
          quoted = false;
      } else if (c == '"') {
        quoted = true;
      } else if (c == ',') {
        break;
      }
      ++end;
    }
    if (quoted) {
      if (error)
        *error = "unterminated quoted authentication parameter";
      return false;
    }

    std::string_view item = Trim(text.substr(pos, end - pos));
    pos = end;
    const std::size_t eq = item.find('=');
    if (eq == std::string_view::npos || Trim(item.substr(0, eq)).empty()) {
      if (error)
        *error = "malformed authentication parameter";
      return false;
    }
    std::string name = ToLowerAscii(Trim(item.substr(0, eq)));
    std::string_view raw = Trim(item.substr(eq + 1));
    std::string value;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
      raw = raw.substr(1, raw.size() - 2);
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
          ++i;
          value.push_back(raw[i]);
        } else {
          value.push_back(raw[i]);
        }
      }
    } else {
      value = std::string(raw);
    }
    out->emplace_back(std::move(name), std::move(value));
  }
  return true;
}

std::string EscapeQuoted(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::string UnquoteDisplayName(std::string_view text) {
  text = Trim(text);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size() - 1) {
        ++i;
        out.push_back(text[i]);
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }
  return std::string(text);
}

bool ValidHeaderToken(std::string_view name) {
  if (name.empty())
    return false;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7f)
      return false;
    switch (c) {
      case '(':
      case ')':
      case '<':
      case '>':
      case '@':
      case ',':
      case ';':
      case ':':
      case '\\':
      case '"':
      case '/':
      case '[':
      case ']':
      case '?':
      case '=':
      case '{':
      case '}':
        return false;
      default:
        break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// MD5 (OpenSSL 3 default provider) for SIP Digest
// ---------------------------------------------------------------------------

std::string Md5Hex(std::string_view data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr)
    return {};
  const bool ok = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
                  EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
  EVP_MD_CTX_free(ctx);
  if (!ok)
    return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest_len * 2);
  for (unsigned int i = 0; i < digest_len; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// URI / address parsing
// ---------------------------------------------------------------------------

bool ParseUriHeaders(std::string_view text, std::vector<std::pair<std::string, std::string>>* out,
                     std::string* error) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t amp = text.find('&', pos);
    std::string_view item =
        text.substr(pos, amp == std::string_view::npos ? text.size() - pos : amp - pos);
    if (!item.empty()) {
      const std::size_t eq = item.find('=');
      std::string name = std::string(eq == std::string_view::npos ? Trim(item)
                                                                  : Trim(item.substr(0, eq)));
      std::string value =
          std::string(eq == std::string_view::npos ? std::string_view{} : Trim(item.substr(eq + 1)));
      if (name.empty()) {
        if (error)
          *error = "empty URI header name";
        return false;
      }
      out->emplace_back(std::move(name), std::move(value));
    }
    if (amp == std::string_view::npos)
      break;
    pos = amp + 1;
  }
  return true;
}

// Parses a "host:port" pair used by URI and Via. IPv6 literals must be
// bracketed, matching RFC 3261.
bool ParseHostPort(std::string_view text, std::string* host, std::uint16_t* port,
                   std::string* error) {
  text = Trim(text);
  if (text.empty()) {
    if (error)
      *error = "empty host";
    return false;
  }

  std::string_view port_text;
  if (text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos) {
      if (error)
        *error = "unterminated IPv6 literal";
      return false;
    }
    *host = std::string(text.substr(1, close - 1));
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') {
        if (error)
          *error = "unexpected text after IPv6 literal";
        return false;
      }
      port_text = text.substr(close + 2);
    }
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon != std::string_view::npos) {
      if (text.find(':') != colon) {
        if (error)
          *error = "IPv6 address must be bracketed";
        return false;
      }
      *host = std::string(text.substr(0, colon));
      port_text = text.substr(colon + 1);
    } else {
      *host = std::string(text);
    }
  }

  if (host->empty()) {
    if (error)
      *error = "empty host";
    return false;
  }
  if (HasControlChars(*host) || host->find(' ') != std::string::npos) {
    if (error)
      *error = "invalid host";
    return false;
  }

  *port = 0;
  if (!port_text.empty()) {
    std::uint32_t value = 0;
    if (!ParseUint32(port_text, &value) || value == 0 || value > 65535) {
      if (error)
        *error = "invalid port";
      return false;
    }
    *port = static_cast<std::uint16_t>(value);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Digest helpers
// ---------------------------------------------------------------------------

bool LooksLikeDigest(std::string_view value) {
  value = Trim(value);
  if (!StartsWithIgnoreCase(value, "Digest"))
    return false;
  return value.size() == 6 || std::isspace(static_cast<unsigned char>(value[6])) != 0;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

struct ResolvedEndpoint {
  sockaddr_storage address{};
  int address_length = 0;
  int family = AF_UNSPEC;
  std::string text;  // numeric "ip:port"
};

std::string SockaddrText(const sockaddr* address) {
  if (address == nullptr)
    return {};
  char host[256];
  int port = 0;
  if (address->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
    uv_ip4_name(v4, host, sizeof(host));
    port = ntohs(v4->sin_port);
  } else if (address->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
    uv_ip6_name(v6, host, sizeof(host));
    port = ntohs(v6->sin6_port);
  } else {
    return {};
  }
  return FormatHostPort(host, static_cast<std::uint16_t>(port));
}

std::string SockaddrHost(const sockaddr* address) {
  if (address == nullptr)
    return {};
  char host[256];
  if (address->sa_family == AF_INET) {
    if (uv_ip4_name(reinterpret_cast<const sockaddr_in*>(address), host, sizeof(host)) != 0)
      return {};
  } else if (address->sa_family == AF_INET6) {
    if (uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(address), host, sizeof(host)) != 0)
      return {};
  } else {
    return {};
  }
  return std::string(host);
}

bool ResolveEndpoint(const std::string& host_in, std::uint16_t port, int socktype,
                     ResolvedEndpoint* out, std::string* error) {
  std::string host = host_in;
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  if (host.empty() || port == 0) {
    if (error)
      *error = "invalid server address";
    return false;
  }

  sockaddr_in v4{};
  if (uv_ip4_addr(host.c_str(), static_cast<int>(port), &v4) == 0) {
    std::memcpy(&out->address, &v4, sizeof(v4));
    out->address_length = static_cast<int>(sizeof(v4));
    out->family = AF_INET;
    out->text = SockaddrText(reinterpret_cast<const sockaddr*>(&v4));
    return true;
  }
  sockaddr_in6 v6{};
  if (uv_ip6_addr(host.c_str(), static_cast<int>(port), &v6) == 0) {
    std::memcpy(&out->address, &v6, sizeof(v6));
    out->address_length = static_cast<int>(sizeof(v6));
    out->family = AF_INET6;
    out->text = SockaddrText(reinterpret_cast<const sockaddr*>(&v6));
    return true;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_protocol = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port);
  const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
  if (rc != 0 || results == nullptr) {
    if (error)
      *error = "cannot resolve " + host + " (getaddrinfo error " + std::to_string(rc) + ")";
    if (results != nullptr)
      freeaddrinfo(results);
    return false;
  }

  const addrinfo* chosen = results;
  for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
    if (it->ai_family == AF_INET) {
      chosen = it;
      break;
    }
    if (chosen->ai_family != AF_INET)
      chosen = it;
  }
  if (chosen->ai_addrlen > sizeof(out->address)) {
    freeaddrinfo(results);
    if (error)
      *error = "resolved address does not fit sockaddr_storage";
    return false;
  }
  std::memcpy(&out->address, chosen->ai_addr, chosen->ai_addrlen);
  out->address_length = static_cast<int>(chosen->ai_addrlen);
  out->family = chosen->ai_family;
  out->text = SockaddrText(chosen->ai_addr);
  freeaddrinfo(results);
  return true;
}

bool BindAddressForFamily(int family, const std::string& bind_ip, std::uint16_t port,
                          sockaddr_storage* out, int* length, std::string* error) {
  std::string ip = bind_ip;
  if (ip.empty())
    ip = family == AF_INET6 ? "::" : "0.0.0.0";
  if (family == AF_INET) {
    sockaddr_in addr{};
    if (uv_ip4_addr(ip.c_str(), static_cast<int>(port), &addr) != 0) {
      if (error)
        *error = "invalid IPv4 bind address " + ip;
      return false;
    }
    std::memcpy(out, &addr, sizeof(addr));
    *length = static_cast<int>(sizeof(addr));
    return true;
  }
  sockaddr_in6 addr{};
  if (uv_ip6_addr(ip.c_str(), static_cast<int>(port), &addr) != 0) {
    if (error)
      *error = "invalid IPv6 bind address " + ip;
    return false;
  }
  std::memcpy(out, &addr, sizeof(addr));
  *length = static_cast<int>(sizeof(addr));
  return true;
}

bool IsWildcardAddress(const std::string& ip) {
  return ip.empty() || ip == "0.0.0.0" || ip == "::" || ip == "*";
}

// ---------------------------------------------------------------------------
// Transport abstraction
// ---------------------------------------------------------------------------

class SipTransport {
 public:
  struct Callbacks {
    std::function<void(std::string raw, std::string peer)> on_message;
    std::function<void(std::string error)> on_error;
    std::function<void()> on_connected;
    std::function<void(std::string reason)> on_disconnected;
  };

  virtual ~SipTransport() = default;
  virtual bool Start(Callbacks callbacks, std::string* error) = 0;
  virtual bool Send(std::string message, std::string* error) = 0;
  virtual bool connected() const = 0;
  virtual std::string local_ip() const = 0;
  virtual std::uint16_t local_port() const = 0;
  virtual std::string peer() const = 0;
};

struct TransportConfig {
  std::string server_host;
  std::uint16_t server_port = 0;
  std::string bind_ip;
  std::string local_ip;
  std::uint16_t local_port = 0;
};

// --- UDP -------------------------------------------------------------------

class UdpTransport final : public SipTransport {
 public:
  UdpTransport(uvcpp::uvcpp_loop* loop, TransportConfig config)
      : loop_(loop), config_(std::move(config)) {}
  ~UdpTransport() override {
    if (state_ != nullptr) {
      state_->owner = nullptr;
      if (!state_->closing) {
        state_->closing = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&state_->handle), &UdpTransport::OnClosed);
      }
      state_ = nullptr;
    }
  }

  bool Start(Callbacks callbacks, std::string* error) override {
    callbacks_ = std::move(callbacks);
    ResolvedEndpoint endpoint;
    if (!ResolveEndpoint(config_.server_host, config_.server_port, SOCK_DGRAM, &endpoint, error))
      return false;
    peer_ = endpoint.text;

    auto* raw_loop = reinterpret_cast<uv_loop_t*>(loop_->get_handle());
    state_ = new UdpState();
    state_->owner = this;
    if (uv_udp_init(raw_loop, &state_->handle) != 0) {
      delete state_;
      state_ = nullptr;
      if (error)
        *error = "uv_udp_init failed";
      return false;
    }
    state_->handle.data = state_;

    sockaddr_storage bind_addr{};
    int bind_len = 0;
    if (!BindAddressForFamily(endpoint.family, config_.bind_ip, config_.local_port, &bind_addr,
                              &bind_len, error)) {
      CloseState();
      return false;
    }
    const int bind_rc = uv_udp_bind(&state_->handle,
                                    reinterpret_cast<const sockaddr*>(&bind_addr), 0);
    if (bind_rc != 0) {
      if (error)
        *error = std::string("udp bind failed: ") + uv_strerror(bind_rc);
      CloseState();
      return false;
    }

    sockaddr_storage local{};
    int local_len = static_cast<int>(sizeof(local));
    if (uv_udp_getsockname(&state_->handle, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
      std::string bound_host = SockaddrHost(reinterpret_cast<const sockaddr*>(&local));
      if (local.ss_family == AF_INET)
        local_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
      else if (local.ss_family == AF_INET6)
        local_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
      if (!IsWildcardAddress(bound_host))
        local_ip_ = bound_host;
    }
    if (!config_.local_ip.empty())
      local_ip_ = config_.local_ip;
    if (local_ip_.empty())
      local_ip_ = SockaddrHost(reinterpret_cast<const sockaddr*>(&endpoint.address));
    if (local_port_ == 0) {
      if (error)
        *error = "udp socket has no local port";
      CloseState();
      return false;
    }

    peer_addr_ = endpoint.address;
    const int read_rc = uv_udp_recv_start(&state_->handle, &UdpTransport::OnAlloc,
                                          &UdpTransport::OnRecv);
    if (read_rc != 0) {
      if (error)
        *error = std::string("udp recv_start failed: ") + uv_strerror(read_rc);
      CloseState();
      return false;
    }
    started_ = true;
    return true;
  }

  bool Send(std::string message, std::string* error) override {
    if (!started_ || state_ == nullptr || state_->closing) {
      if (error)
        *error = "udp transport is not running";
      return false;
    }
    state_->queue.push_back(std::move(message));
    Pump();
    return true;
  }

  bool connected() const override { return started_; }
  std::string local_ip() const override { return local_ip_; }
  std::uint16_t local_port() const override { return local_port_; }
  std::string peer() const override { return peer_; }

 private:
  struct UdpState {
    uv_udp_t handle{};
    UdpTransport* owner = nullptr;
    std::deque<std::string> queue;
    std::string in_flight;
    bool send_in_flight = false;
    bool closing = false;
  };

  static void OnAlloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    const std::size_t size = suggested == 0 ? 65536 : suggested;
    buf->base = static_cast<char*>(std::malloc(size));
    buf->len = buf->base == nullptr ? 0 : size;
  }

  static void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                     const sockaddr* addr, unsigned int /*flags*/) {
    auto* state = static_cast<UdpState*>(handle->data);
    if (state == nullptr || state->owner == nullptr) {
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      return;
    }
    if (nread < 0) {
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      state->owner->OnAsyncError(std::string("udp read: ") + uv_strerror(static_cast<int>(nread)));
      return;
    }
    if (nread > 0 && addr != nullptr && state->owner->callbacks_.on_message) {
      std::string raw(buf->base, static_cast<std::size_t>(nread));
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      state->owner->callbacks_.on_message(std::move(raw), SockaddrText(addr));
      return;
    }
    if (buf != nullptr && buf->base != nullptr)
      std::free(buf->base);
  }

  static void OnSent(uv_udp_send_t* req, int status) {
    auto* state = static_cast<UdpState*>(req->data);
    delete req;
    if (state == nullptr)
      return;
    state->send_in_flight = false;
    state->in_flight.clear();
    if (state->owner == nullptr)
      return;
    if (status < 0) {
      state->owner->OnAsyncError(std::string("udp send: ") + uv_strerror(status));
      return;
    }
    state->owner->Pump();
  }

  static void OnClosed(uv_handle_t* handle) {
    auto* state = static_cast<UdpState*>(handle->data);
    delete state;
  }

  void Pump() {
    if (state_ == nullptr || state_->send_in_flight || state_->queue.empty())
      return;
    state_->in_flight = std::move(state_->queue.front());
    state_->queue.pop_front();
    auto* req = new uv_udp_send_t();
    req->data = state_;
    uv_buf_t buf = uv_buf_init(const_cast<char*>(state_->in_flight.data()),
                               static_cast<unsigned int>(state_->in_flight.size()));
    const int rc = uv_udp_send(req, &state_->handle, &buf, 1,
                               reinterpret_cast<const sockaddr*>(&peer_addr_), &UdpTransport::OnSent);
    if (rc != 0) {
      delete req;
      state_->in_flight.clear();
      OnAsyncError(std::string("udp send: ") + uv_strerror(rc));
      return;
    }
    state_->send_in_flight = true;
  }

  void OnAsyncError(std::string error) {
    if (callbacks_.on_error)
      callbacks_.on_error(std::move(error));
  }

  void CloseState() {
    if (state_ == nullptr)
      return;
    state_->owner = nullptr;
    if (!state_->closing) {
      state_->closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&state_->handle), &UdpTransport::OnClosed);
    }
    state_ = nullptr;
  }

  uvcpp::uvcpp_loop* loop_;
  TransportConfig config_;
  Callbacks callbacks_;
  UdpState* state_ = nullptr;
  sockaddr_storage peer_addr_{};
  std::string peer_;
  std::string local_ip_;
  std::uint16_t local_port_ = 0;
  bool started_ = false;
};

// --- TCP -------------------------------------------------------------------

class TcpTransport final : public SipTransport {
 public:
  TcpTransport(uvcpp::uvcpp_loop* loop, TransportConfig config)
      : loop_(loop), config_(std::move(config)) {}
  ~TcpTransport() override {
    if (state_ != nullptr) {
      state_->owner = nullptr;
      if (!state_->closing) {
        state_->closing = true;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&state_->handle));
        uv_close(reinterpret_cast<uv_handle_t*>(&state_->handle), &TcpTransport::OnClosed);
      }
      state_ = nullptr;
    }
  }

  bool Start(Callbacks callbacks, std::string* error) override {
    callbacks_ = std::move(callbacks);
    ResolvedEndpoint endpoint;
    if (!ResolveEndpoint(config_.server_host, config_.server_port, SOCK_STREAM, &endpoint, error))
      return false;
    peer_ = endpoint.text;

    auto* raw_loop = reinterpret_cast<uv_loop_t*>(loop_->get_handle());
    state_ = new TcpState();
    state_->owner = this;
    if (uv_tcp_init(raw_loop, &state_->handle) != 0) {
      delete state_;
      state_ = nullptr;
      if (error)
        *error = "uv_tcp_init failed";
      return false;
    }
    state_->handle.data = state_;

    if (!config_.bind_ip.empty() || config_.local_port != 0) {
      sockaddr_storage bind_addr{};
      int bind_len = 0;
      if (!BindAddressForFamily(endpoint.family, config_.bind_ip, config_.local_port, &bind_addr,
                                &bind_len, error)) {
        CloseState();
        return false;
      }
      const int bind_rc =
          uv_tcp_bind(&state_->handle, reinterpret_cast<const sockaddr*>(&bind_addr), 0);
      if (bind_rc != 0) {
        if (error)
          *error = std::string("tcp bind failed: ") + uv_strerror(bind_rc);
        CloseState();
        return false;
      }
    }

    state_->connect_req.data = state_;
    const int rc = uv_tcp_connect(&state_->connect_req, &state_->handle,
                                  reinterpret_cast<const sockaddr*>(&endpoint.address),
                                  &TcpTransport::OnConnect);
    if (rc != 0) {
      if (error)
        *error = std::string("tcp connect failed: ") + uv_strerror(rc);
      CloseState();
      return false;
    }
    state_->connecting = true;
    return true;
  }

  bool Send(std::string message, std::string* error) override {
    if (!ConnectedState()) {
      if (error)
        *error = "tcp transport is not connected";
      return false;
    }
    state_->queue.push_back(std::move(message));
    Pump();
    return true;
  }

  bool connected() const override { return ConnectedState(); }
  std::string local_ip() const override { return local_ip_; }
  std::uint16_t local_port() const override { return local_port_; }
  std::string peer() const override { return peer_; }

 private:
  struct TcpState {
    uv_tcp_t handle{};
    uv_connect_t connect_req{};
    TcpTransport* owner = nullptr;
    std::deque<std::string> queue;
    std::string in_flight;
    std::string in_buffer;
    bool connecting = false;
    bool connected = false;
    bool send_in_flight = false;
    bool closing = false;
  };

  bool ConnectedState() const { return state_ != nullptr && state_->connected && !state_->closing; }

  static void OnAlloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    const std::size_t size = suggested == 0 ? 65536 : suggested;
    buf->base = static_cast<char*>(std::malloc(size));
    buf->len = buf->base == nullptr ? 0 : size;
  }

  static void OnConnect(uv_connect_t* req, int status) {
    auto* state = static_cast<TcpState*>(req->data);
    if (state == nullptr || state->owner == nullptr)
      return;
    state->connecting = false;
    if (status < 0) {
      state->owner->OnAsyncError(std::string("tcp connect: ") + uv_strerror(status));
      return;
    }
    sockaddr_storage local{};
    int local_len = static_cast<int>(sizeof(local));
    if (uv_tcp_getsockname(&state->handle, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
      std::string bound_host = SockaddrHost(reinterpret_cast<const sockaddr*>(&local));
      if (local.ss_family == AF_INET)
        state->owner->local_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
      else if (local.ss_family == AF_INET6)
        state->owner->local_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
      if (!IsWildcardAddress(bound_host))
        state->owner->local_ip_ = bound_host;
    }
    if (!state->owner->config_.local_ip.empty())
      state->owner->local_ip_ = state->owner->config_.local_ip;
    if (state->owner->local_ip_.empty())
      state->owner->local_ip_ = SockaddrHost(reinterpret_cast<const sockaddr*>(&local));

    const int read_rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&state->handle),
                                      &TcpTransport::OnAlloc, &TcpTransport::OnRead);
    if (read_rc != 0) {
      state->owner->OnAsyncError(std::string("tcp read_start: ") + uv_strerror(read_rc));
      return;
    }
    state->connected = true;
    if (state->owner->callbacks_.on_connected)
      state->owner->callbacks_.on_connected();
  }

  static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* state = static_cast<TcpState*>(stream->data);
    if (state == nullptr || state->owner == nullptr) {
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      return;
    }
    if (nread < 0) {
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      if (state->connected) {
        state->connected = false;
        uv_read_stop(stream);
        if (state->owner->callbacks_.on_disconnected) {
          state->owner->callbacks_.on_disconnected(
              std::string("tcp connection closed: ") + uv_strerror(static_cast<int>(nread)));
        }
      }
      return;
    }
    if (nread == 0) {
      if (buf != nullptr && buf->base != nullptr)
        std::free(buf->base);
      return;
    }
    state->in_buffer.append(buf->base, static_cast<std::size_t>(nread));
    if (buf != nullptr && buf->base != nullptr)
      std::free(buf->base);
    state->owner->ProcessInput();
  }

  static void OnWritten(uv_write_t* req, int status) {
    auto* state = static_cast<TcpState*>(req->data);
    delete req;
    if (state == nullptr)
      return;
    state->send_in_flight = false;
    state->in_flight.clear();
    if (state->owner == nullptr)
      return;
    if (status < 0) {
      state->connected = false;
      state->owner->OnAsyncError(std::string("tcp write: ") + uv_strerror(status));
      return;
    }
    state->owner->Pump();
  }

  static void OnClosed(uv_handle_t* handle) {
    auto* state = static_cast<TcpState*>(handle->data);
    delete state;
  }

  void ProcessInput() {
    while (state_ != nullptr && !state_->in_buffer.empty()) {
      SipMessage message;
      std::size_t consumed = 0;
      std::string error;
      const SipParseStatus status =
          SipMessage::Parse(state_->in_buffer, &message, &consumed, &error);
      if (status == SipParseStatus::kIncomplete)
        break;
      if (status == SipParseStatus::kInvalid) {
        OnAsyncError("invalid SIP message on tcp stream: " + error);
        state_->connected = false;
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&state_->handle));
        return;
      }
      std::string raw = state_->in_buffer.substr(0, consumed);
      state_->in_buffer.erase(0, consumed);
      if (callbacks_.on_message)
        callbacks_.on_message(std::move(raw), peer_);
    }
    if (state_ != nullptr && state_->in_buffer.size() > 4 * 1024 * 1024) {
      OnAsyncError("SIP message exceeded the 4 MiB stream limit");
      state_->connected = false;
      uv_read_stop(reinterpret_cast<uv_stream_t*>(&state_->handle));
    }
  }

  void Pump() {
    if (state_ == nullptr || !state_->connected || state_->closing || state_->send_in_flight ||
        state_->queue.empty())
      return;
    state_->in_flight = std::move(state_->queue.front());
    state_->queue.pop_front();
    auto* req = new uv_write_t();
    req->data = state_;
    uv_buf_t buf = uv_buf_init(const_cast<char*>(state_->in_flight.data()),
                               static_cast<unsigned int>(state_->in_flight.size()));
    const int rc = uv_write(req, reinterpret_cast<uv_stream_t*>(&state_->handle), &buf, 1,
                            &TcpTransport::OnWritten);
    if (rc != 0) {
      delete req;
      state_->in_flight.clear();
      OnAsyncError(std::string("tcp write: ") + uv_strerror(rc));
      return;
    }
    state_->send_in_flight = true;
  }

  void OnAsyncError(std::string error) {
    if (callbacks_.on_error)
      callbacks_.on_error(std::move(error));
  }

  void CloseState() {
    if (state_ == nullptr)
      return;
    state_->owner = nullptr;
    if (!state_->closing) {
      state_->closing = true;
      uv_read_stop(reinterpret_cast<uv_stream_t*>(&state_->handle));
      uv_close(reinterpret_cast<uv_handle_t*>(&state_->handle), &TcpTransport::OnClosed);
    }
    state_ = nullptr;
  }

  uvcpp::uvcpp_loop* loop_;
  TransportConfig config_;
  Callbacks callbacks_;
  TcpState* state_ = nullptr;
  std::string peer_;
  std::string local_ip_;
  std::uint16_t local_port_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// SIP method names
// ---------------------------------------------------------------------------

std::string_view SipMethodName(SipMethod method) {
  switch (method) {
    case SipMethod::kRegister:
      return "REGISTER";
    case SipMethod::kInvite:
      return "INVITE";
    case SipMethod::kAck:
      return "ACK";
    case SipMethod::kBye:
      return "BYE";
    case SipMethod::kCancel:
      return "CANCEL";
    case SipMethod::kOptions:
      return "OPTIONS";
    case SipMethod::kMessage:
      return "MESSAGE";
    case SipMethod::kInfo:
      return "INFO";
    case SipMethod::kPrack:
      return "PRACK";
    case SipMethod::kSubscribe:
      return "SUBSCRIBE";
    case SipMethod::kNotify:
      return "NOTIFY";
    case SipMethod::kRefer:
      return "REFER";
    case SipMethod::kUnknown:
      break;
  }
  return "UNKNOWN";
}

SipMethod ParseSipMethod(std::string_view name) {
  if (EqualsIgnoreCase(name, "REGISTER"))
    return SipMethod::kRegister;
  if (EqualsIgnoreCase(name, "INVITE"))
    return SipMethod::kInvite;
  if (EqualsIgnoreCase(name, "ACK"))
    return SipMethod::kAck;
  if (EqualsIgnoreCase(name, "BYE"))
    return SipMethod::kBye;
  if (EqualsIgnoreCase(name, "CANCEL"))
    return SipMethod::kCancel;
  if (EqualsIgnoreCase(name, "OPTIONS"))
    return SipMethod::kOptions;
  if (EqualsIgnoreCase(name, "MESSAGE"))
    return SipMethod::kMessage;
  if (EqualsIgnoreCase(name, "INFO"))
    return SipMethod::kInfo;
  if (EqualsIgnoreCase(name, "PRACK"))
    return SipMethod::kPrack;
  if (EqualsIgnoreCase(name, "SUBSCRIBE"))
    return SipMethod::kSubscribe;
  if (EqualsIgnoreCase(name, "NOTIFY"))
    return SipMethod::kNotify;
  if (EqualsIgnoreCase(name, "REFER"))
    return SipMethod::kRefer;
  return SipMethod::kUnknown;
}

// ---------------------------------------------------------------------------
// Header names
// ---------------------------------------------------------------------------

std::string CanonicalHeaderName(std::string_view name) {
  const std::string lower = ToLowerAscii(name);
  if (lower == "v")
    return "Via";
  if (lower == "f")
    return "From";
  if (lower == "t")
    return "To";
  if (lower == "i")
    return "Call-ID";
  if (lower == "m")
    return "Contact";
  if (lower == "l")
    return "Content-Length";
  if (lower == "c")
    return "Content-Type";
  if (lower == "s")
    return "Subject";
  if (lower == "k")
    return "Supported";
  if (lower == "e")
    return "Content-Encoding";
  if (lower == "o")
    return "Event";
  if (lower == "r")
    return "Refer-To";
  if (lower == "b")
    return "Referred-By";
  if (lower == "x")
    return "Session-Expires";
  if (lower == "u")
    return "Allow-Events";
  return std::string(name);
}

namespace {

std::string NormalizedHeaderName(std::string_view name) {
  return ToLowerAscii(CanonicalHeaderName(name));
}

}  // namespace

bool HeaderNameEquals(std::string_view a, std::string_view b) {
  return NormalizedHeaderName(a) == NormalizedHeaderName(b);
}

// ---------------------------------------------------------------------------
// SIP URI
// ---------------------------------------------------------------------------

std::string SipUri::Parameter(std::string_view name) const {
  for (const auto& [key, value] : parameters) {
    if (EqualsIgnoreCase(key, name))
      return value;
  }
  return {};
}

std::string SipUri::Header(std::string_view name) const {
  for (const auto& [key, value] : headers) {
    if (EqualsIgnoreCase(key, name))
      return value;
  }
  return {};
}

bool ParseSipUri(std::string_view text, SipUri* uri, std::string* error) {
  if (uri == nullptr) {
    if (error)
      *error = "null output";
    return false;
  }
  *uri = SipUri{};
  text = Trim(text);
  if (text.empty()) {
    if (error)
      *error = "empty URI";
    return false;
  }

  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    if (error)
      *error = "missing URI scheme";
    return false;
  }
  uri->scheme = ToLowerAscii(text.substr(0, colon));
  if (uri->scheme != "sip" && uri->scheme != "sips") {
    if (error)
      *error = "unsupported URI scheme '" + uri->scheme + "'";
    return false;
  }

  std::string_view rest = text.substr(colon + 1);
  if (rest.empty()) {
    if (error)
      *error = "empty URI after scheme";
    return false;
  }

  std::string_view headers_text;
  const std::size_t question = rest.find('?');
  if (question != std::string_view::npos) {
    headers_text = rest.substr(question + 1);
    rest = rest.substr(0, question);
  }

  const std::size_t at = rest.find('@');
  if (at != std::string_view::npos) {
    std::string_view userinfo = rest.substr(0, at);
    rest = rest.substr(at + 1);
    if (userinfo.empty()) {
      if (error)
        *error = "empty userinfo";
      return false;
    }
    const std::size_t semi = userinfo.find(';');
    if (semi != std::string_view::npos) {
      uri->user_parameters = std::string(userinfo.substr(semi + 1));
      userinfo = userinfo.substr(0, semi);
    }
    const std::size_t user_colon = userinfo.find(':');
    if (user_colon != std::string_view::npos) {
      uri->user = std::string(userinfo.substr(0, user_colon));
      uri->password = std::string(userinfo.substr(user_colon + 1));
    } else {
      uri->user = std::string(userinfo);
    }
    if (uri->user.empty()) {
      if (error)
        *error = "empty URI user";
      return false;
    }
  }

  const std::size_t semicolon = rest.find(';');
  std::string_view hostport = rest;
  std::string_view parameters_text;
  if (semicolon != std::string_view::npos) {
    hostport = rest.substr(0, semicolon);
    parameters_text = rest.substr(semicolon);
  }

  if (!ParseHostPort(hostport, &uri->host, &uri->port, error))
    return false;

  if (!parameters_text.empty()) {
    if (!ParseParameterList(parameters_text, &uri->parameters, error))
      return false;
  }
  if (!headers_text.empty()) {
    if (!ParseUriHeaders(headers_text, &uri->headers, error))
      return false;
  }
  return true;
}

std::string FormatSipUri(const SipUri& uri) {
  std::string out = uri.scheme.empty() ? "sip" : uri.scheme;
  out.push_back(':');
  if (!uri.user.empty() || !uri.password.empty() || !uri.user_parameters.empty()) {
    out += uri.user;
    if (!uri.password.empty()) {
      out.push_back(':');
      out += uri.password;
    }
    if (!uri.user_parameters.empty()) {
      out.push_back(';');
      out += uri.user_parameters;
    }
    out.push_back('@');
  }
  out += FormatHostPort(uri.host, uri.port);
  for (const auto& [name, value] : uri.parameters) {
    out.push_back(';');
    out += name;
    if (!value.empty()) {
      out.push_back('=');
      out += value;
    }
  }
  bool first = true;
  for (const auto& [name, value] : uri.headers) {
    out.push_back(first ? '?' : '&');
    first = false;
    out += name;
    if (!value.empty()) {
      out.push_back('=');
      out += value;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// SIP address
// ---------------------------------------------------------------------------

std::string SipAddress::Parameter(std::string_view name) const {
  for (const auto& [key, value] : parameters) {
    if (EqualsIgnoreCase(key, name))
      return value;
  }
  // A bare addr-spec carries the tag between the URI parameters; accept it
  // there as well so From/To tags are found regardless of bracket style.
  for (const auto& [key, value] : uri.parameters) {
    if (EqualsIgnoreCase(key, name))
      return value;
  }
  return {};
}

bool ParseSipAddress(std::string_view text, SipAddress* address, std::string* error) {
  if (address == nullptr) {
    if (error)
      *error = "null output";
    return false;
  }
  *address = SipAddress{};
  text = Trim(text);
  if (text.empty()) {
    if (error)
      *error = "empty address";
    return false;
  }

  const std::size_t open = text.find('<');
  if (open != std::string_view::npos) {
    const std::size_t close = text.find('>', open + 1);
    if (close == std::string_view::npos) {
      if (error)
        *error = "unterminated '<' in address";
      return false;
    }
    address->display_name = UnquoteDisplayName(text.substr(0, open));
    const std::string_view uri_text = Trim(text.substr(open + 1, close - open - 1));
    std::string_view rest = Trim(text.substr(close + 1));
    if (!rest.empty()) {
      if (rest.front() != ';') {
        if (error)
          *error = "unexpected text after '>' in address";
        return false;
      }
      if (!ParseParameterList(rest, &address->parameters, error))
        return false;
    }
    return ParseSipUri(uri_text, &address->uri, error);
  }

  // Bare addr-spec: parameters belong to the URI, but Parameter() also looks
  // there, so no rewriting is needed.
  if (!ParseSipUri(text, &address->uri, error))
    return false;
  address->display_name.clear();
  return true;
}

std::string FormatSipAddress(const SipAddress& address) {
  std::string out;
  if (!address.display_name.empty()) {
    out.push_back('"');
    out += EscapeQuoted(address.display_name);
    out += "\" ";
  }
  out.push_back('<');
  out += FormatSipUri(address.uri);
  out.push_back('>');
  for (const auto& [name, value] : address.parameters) {
    out.push_back(';');
    out += name;
    if (!value.empty()) {
      out.push_back('=');
      out += value;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// SipMessage
// ---------------------------------------------------------------------------

SipParseStatus SipMessage::Parse(std::string_view data, SipMessage* message,
                                 std::size_t* consumed, std::string* error) {
  if (message == nullptr)
    return SipParseStatus::kInvalid;
  *message = SipMessage{};
  if (consumed != nullptr)
    *consumed = 0;

  std::size_t offset = 0;
  while (offset < data.size() && (data[offset] == '\r' || data[offset] == '\n'))
    ++offset;
  if (offset >= data.size()) {
    if (error)
      *error = "empty message";
    return SipParseStatus::kIncomplete;
  }

  std::vector<std::string_view> lines;
  std::size_t pos = offset;
  std::size_t header_end = 0;
  bool header_complete = false;
  while (pos < data.size()) {
    const std::size_t newline = data.find('\n', pos);
    if (newline == std::string_view::npos) {
      if (error)
        *error = "incomplete header block";
      return SipParseStatus::kIncomplete;
    }
    std::string_view line = data.substr(pos, newline - pos);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    pos = newline + 1;
    if (line.empty()) {
      header_end = pos;
      header_complete = true;
      break;
    }
    lines.push_back(line);
  }
  if (!header_complete) {
    if (error)
      *error = "incomplete header block";
    return SipParseStatus::kIncomplete;
  }
  if (lines.empty()) {
    if (error)
      *error = "missing start line";
    return SipParseStatus::kInvalid;
  }

  const std::string_view start_line = Trim(lines.front());
  if (StartsWithIgnoreCase(start_line, "SIP/2.0")) {
    message->is_request = false;
    std::size_t i = 7;
    while (i < start_line.size() && start_line[i] == ' ')
      ++i;
    const std::size_t code_begin = i;
    while (i < start_line.size() && start_line[i] >= '0' && start_line[i] <= '9')
      ++i;
    if (i - code_begin != 3) {
      if (error)
        *error = "invalid status code";
      return SipParseStatus::kInvalid;
    }
    message->status_code = (start_line[code_begin] - '0') * 100 +
                           (start_line[code_begin + 1] - '0') * 10 +
                           (start_line[code_begin + 2] - '0');
    while (i < start_line.size() && start_line[i] == ' ')
      ++i;
    message->reason_phrase = std::string(Trim(start_line.substr(i)));
  } else {
    message->is_request = true;
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < start_line.size()) {
      while (i < start_line.size() && start_line[i] == ' ')
        ++i;
      if (i >= start_line.size())
        break;
      const std::size_t begin = i;
      while (i < start_line.size() && start_line[i] != ' ')
        ++i;
      parts.push_back(start_line.substr(begin, i - begin));
    }
    if (parts.size() != 3 || !EqualsIgnoreCase(parts[2], "SIP/2.0")) {
      if (error)
        *error = "malformed request line";
      return SipParseStatus::kInvalid;
    }
    message->method = ParseSipMethod(parts[0]);
    if (!ParseSipUri(parts[1], &message->request_uri, error))
      return SipParseStatus::kInvalid;
  }

  for (std::size_t i = 1; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      if (message->headers.empty()) {
        if (error)
          *error = "header continuation without a header";
        return SipParseStatus::kInvalid;
      }
      std::string& value = message->headers.back().value;
      value.push_back(' ');
      value += std::string(Trim(line));
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      if (error)
        *error = "header line without ':'";
      return SipParseStatus::kInvalid;
    }
    std::string_view name = Trim(line.substr(0, colon));
    if (!ValidHeaderToken(name)) {
      if (error)
        *error = "invalid header name";
      return SipParseStatus::kInvalid;
    }
    message->headers.push_back(
        SipHeader{std::string(name), std::string(Trim(line.substr(colon + 1)))});
  }

  std::uint32_t content_length = 0;
  for (const SipHeader& header : message->headers) {
    if (HeaderNameEquals(header.name, "Content-Length")) {
      if (!ParseUint32(header.value, &content_length)) {
        if (error)
          *error = "invalid Content-Length";
        return SipParseStatus::kInvalid;
      }
      break;
    }
  }
  constexpr std::uint32_t kMaxBody = 16u * 1024u * 1024u;
  if (content_length > kMaxBody) {
    if (error)
      *error = "Content-Length exceeds the 16 MiB limit";
    return SipParseStatus::kInvalid;
  }
  if (data.size() - header_end < content_length) {
    if (error)
      *error = "incomplete message body";
    return SipParseStatus::kIncomplete;
  }

  message->body.assign(data.data() + header_end, content_length);
  if (consumed != nullptr)
    *consumed = header_end + content_length;
  return SipParseStatus::kComplete;
}

std::string SipMessage::Serialize() const {
  std::string out;
  if (is_request) {
    out += std::string(SipMethodName(method));
    out.push_back(' ');
    out += FormatSipUri(request_uri);
    out += " SIP/2.0\r\n";
  } else {
    out += "SIP/2.0 ";
    out += std::to_string(status_code);
    out.push_back(' ');
    out += reason_phrase;
    out += "\r\n";
  }

  for (const SipHeader& header : headers) {
    if (HeaderNameEquals(header.name, "Content-Length"))
      continue;
    const std::string name = SanitizeHeaderName(header.name);
    if (name.empty())
      continue;
    out += name;
    out += ": ";
    out += SanitizeHeaderValue(header.value);
    out += "\r\n";
  }
  out += "Content-Length: ";
  out += std::to_string(body.size());
  out += "\r\n\r\n";
  out += body;
  return out;
}

const SipHeader* SipMessage::FindHeader(std::string_view name) const {
  for (const SipHeader& header : headers) {
    if (HeaderNameEquals(header.name, name))
      return &header;
  }
  return nullptr;
}

std::vector<const SipHeader*> SipMessage::FindHeaders(std::string_view name) const {
  std::vector<const SipHeader*> out;
  for (const SipHeader& header : headers) {
    if (HeaderNameEquals(header.name, name))
      out.push_back(&header);
  }
  return out;
}

std::string SipMessage::GetHeader(std::string_view name) const {
  const SipHeader* header = FindHeader(name);
  return header == nullptr ? std::string() : header->value;
}

void SipMessage::SetHeader(std::string name, std::string value) {
  const std::string canonical = CanonicalHeaderName(name);
  for (SipHeader& header : headers) {
    if (HeaderNameEquals(header.name, canonical)) {
      header.name = canonical;
      header.value = std::move(value);
      return;
    }
  }
  headers.push_back(SipHeader{canonical, std::move(value)});
}

void SipMessage::AddHeader(std::string name, std::string value) {
  headers.push_back(SipHeader{std::move(name), std::move(value)});
}

void SipMessage::RemoveHeader(std::string_view name) {
  headers.erase(std::remove_if(headers.begin(), headers.end(),
                               [&](const SipHeader& header) {
                                 return HeaderNameEquals(header.name, name);
                               }),
                headers.end());
}

std::string SipMessage::CallId() const {
  return GetHeader("Call-ID");
}

std::uint32_t SipMessage::CSeqNumber() const {
  const std::string value = GetHeader("CSeq");
  const std::size_t space = value.find(' ');
  const std::string_view number =
      space == std::string::npos ? std::string_view(value) : std::string_view(value).substr(0, space);
  std::uint32_t out = 0;
  return ParseUint32(number, &out) ? out : 0;
}

std::string SipMessage::CSeqMethodRaw() const {
  const std::string value = GetHeader("CSeq");
  const std::size_t space = value.find(' ');
  if (space == std::string::npos)
    return {};
  return std::string(Trim(std::string_view(value).substr(space + 1)));
}

SipMethod SipMessage::CSeqMethod() const {
  return ParseSipMethod(CSeqMethodRaw());
}

std::string SipMessage::FromTag() const {
  SipAddress address;
  std::string error;
  if (!ParseSipAddress(GetHeader("From"), &address, &error))
    return {};
  return address.Parameter("tag");
}

std::string SipMessage::ToTag() const {
  SipAddress address;
  std::string error;
  if (!ParseSipAddress(GetHeader("To"), &address, &error))
    return {};
  return address.Parameter("tag");
}

// ---------------------------------------------------------------------------
// Digest authentication
// ---------------------------------------------------------------------------

bool ParseSipDigestChallenge(std::string_view value, SipDigestChallenge* challenge,
                             std::string* error) {
  if (challenge == nullptr) {
    if (error)
      *error = "null output";
    return false;
  }
  *challenge = SipDigestChallenge{};
  value = Trim(value);
  if (!LooksLikeDigest(value)) {
    if (error)
      *error = "authentication challenge is not Digest";
    return false;
  }
  value = Trim(value.substr(6));

  std::vector<std::pair<std::string, std::string>> params;
  if (!ParseAuthParameterList(value, &params, error))
    return false;

  bool have_realm = false;
  bool have_nonce = false;
  for (const auto& [name, item] : params) {
    if (name == "realm") {
      challenge->realm = item;
      have_realm = true;
    } else if (name == "nonce") {
      challenge->nonce = item;
      have_nonce = true;
    } else if (name == "opaque") {
      challenge->opaque = item;
    } else if (name == "algorithm") {
      challenge->algorithm = item.empty() ? "MD5" : item;
    } else if (name == "qop") {
      challenge->qop.clear();
      std::size_t pos = 0;
      while (pos <= item.size()) {
        const std::size_t comma = item.find(',', pos);
        const std::string_view part =
            Trim(std::string_view(item).substr(
                pos, comma == std::string::npos ? item.size() - pos : comma - pos));
        if (!part.empty())
          challenge->qop.emplace_back(part);
        if (comma == std::string::npos)
          break;
        pos = comma + 1;
      }
    } else if (name == "stale") {
      challenge->stale = EqualsIgnoreCase(Trim(item), "true");
    }
  }

  if (!have_realm || !have_nonce) {
    if (error)
      *error = "Digest challenge is missing realm or nonce";
    return false;
  }
  if (challenge->algorithm.empty())
    challenge->algorithm = "MD5";
  return true;
}

bool BuildSipDigestAuthorization(std::string_view method, std::string_view uri,
                                 const SipDigestChallenge& challenge,
                                 const SipDigestCredentials& credentials,
                                 std::string_view cnonce, std::uint32_t nonce_count,
                                 std::string* authorization, std::string* error) {
  if (authorization == nullptr) {
    if (error)
      *error = "null output";
    return false;
  }
  authorization->clear();

  const std::string algorithm = ToLowerAscii(Trim(challenge.algorithm));
  const bool md5_sess = algorithm == "md5-sess";
  if (!algorithm.empty() && algorithm != "md5" && !md5_sess) {
    if (error)
      *error = "unsupported Digest algorithm '" + challenge.algorithm + "'";
    return false;
  }
  if (credentials.username.empty() || challenge.realm.empty() || challenge.nonce.empty() ||
      uri.empty() || method.empty()) {
    if (error)
      *error = "incomplete Digest parameters";
    return false;
  }
  if (HasControlChars(credentials.username) || HasControlChars(credentials.password) ||
      HasControlChars(challenge.realm) || HasControlChars(challenge.nonce) ||
      HasControlChars(uri) || HasControlChars(cnonce) || HasControlChars(challenge.opaque)) {
    if (error)
      *error = "Digest parameter contains a control character";
    return false;
  }

  const std::string ha1_initial =
      SipDigestMd5Hex(credentials.username + ":" + challenge.realm + ":" + credentials.password);
  if (ha1_initial.empty()) {
    if (error)
      *error = "MD5 is unavailable";
    return false;
  }
  std::string ha1 = ha1_initial;
  if (md5_sess) {
    ha1 = SipDigestMd5Hex(ha1_initial + ":" + challenge.nonce + ":" + std::string(cnonce));
    if (ha1.empty()) {
      if (error)
        *error = "MD5 is unavailable";
      return false;
    }
  }
  const std::string ha2 = SipDigestMd5Hex(std::string(method) + ":" + std::string(uri));
  if (ha2.empty()) {
    if (error)
      *error = "MD5 is unavailable";
    return false;
  }

  bool use_qop_auth = false;
  for (const std::string& qop : challenge.qop) {
    if (EqualsIgnoreCase(Trim(qop), "auth")) {
      use_qop_auth = true;
      break;
    }
  }
  if (!challenge.qop.empty() && !use_qop_auth) {
    if (error)
      *error = "no supported qop in the Digest challenge";
    return false;
  }
  if (use_qop_auth && cnonce.empty()) {
    if (error)
      *error = "qop=auth requires a cnonce";
    return false;
  }

  char nc[9] = {};
  std::snprintf(nc, sizeof(nc), "%08x", nonce_count);

  std::string response;
  if (use_qop_auth) {
    response = SipDigestMd5Hex(ha1 + ":" + challenge.nonce + ":" + nc + ":" + std::string(cnonce) +
                               ":auth:" + ha2);
  } else {
    response = SipDigestMd5Hex(ha1 + ":" + challenge.nonce + ":" + ha2);
  }
  if (response.empty()) {
    if (error)
      *error = "MD5 is unavailable";
    return false;
  }

  std::string out = "Digest username=\"";
  out += EscapeQuoted(credentials.username);
  out += "\", realm=\"";
  out += EscapeQuoted(challenge.realm);
  out += "\", nonce=\"";
  out += EscapeQuoted(challenge.nonce);
  out += "\", uri=\"";
  out += EscapeQuoted(uri);
  out += "\", response=\"";
  out += response;
  out += "\", algorithm=";
  out += md5_sess ? "MD5-sess" : "MD5";
  if (use_qop_auth) {
    out += ", cnonce=\"";
    out += EscapeQuoted(cnonce);
    out += "\", qop=auth, nc=";
    out += nc;
  }
  if (!challenge.opaque.empty()) {
    out += ", opaque=\"";
    out += EscapeQuoted(challenge.opaque);
    out += "\"";
  }
  *authorization = std::move(out);
  return true;
}

std::string SipDigestMd5Hex(std::string_view data) {
  return Md5Hex(data);
}

// ---------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------

std::uint32_t ComputeRegistrationBackoffMs(std::uint32_t attempt, std::uint32_t initial_ms,
                                           std::uint32_t max_ms) {
  if (initial_ms == 0)
    initial_ms = 1;
  if (max_ms < initial_ms)
    max_ms = initial_ms;
  if (attempt == 0)
    attempt = 1;
  std::uint64_t delay = initial_ms;
  for (std::uint32_t i = 1; i < attempt && delay < max_ms; ++i)
    delay *= 2;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, max_ms));
}

std::string_view SipRegistrationStateName(SipRegistrationState state) {
  switch (state) {
    case SipRegistrationState::kStopped:
      return "stopped";
    case SipRegistrationState::kConnecting:
      return "connecting";
    case SipRegistrationState::kRegistering:
      return "registering";
    case SipRegistrationState::kRegistered:
      return "registered";
    case SipRegistrationState::kBackoff:
      return "backoff";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// SipUserAgent
// ---------------------------------------------------------------------------

struct SipUserAgent::Impl {
  enum class PendingKind { kRegister, kKeepalive };
  struct Pending {
    PendingKind kind = PendingKind::kRegister;
    std::uint32_t cseq = 0;
    std::string raw;
    TimePoint deadline{};
    TimePoint retransmit_at{};
    std::uint32_t retransmit_ms = 500;
    bool auth_retried = false;
  };

  // Tracks whether a transport callback is currently on the stack. Stop()
  // must not destroy the transport from inside one of its own callbacks;
  // Tick() performs the deferred teardown instead.
  struct CallbackScope {
    explicit CallbackScope(Impl* value) : owner(value) { ++owner->callback_depth; }
    ~CallbackScope() { --owner->callback_depth; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    Impl* owner;
  };

  Impl(uvcpp::uvcpp_loop* event_loop, SipUaConfig user_config)
      : loop(event_loop), config(std::move(user_config)) {
    std::random_device device;
    rng.seed((static_cast<std::uint64_t>(device()) << 32) ^ device());
  }

  ~Impl() {
    running = false;
    pending.reset();
    transport.reset();
    if (timer != nullptr) {
      uv_timer_stop(timer);
      timer->data = nullptr;
      uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* handle) {
        delete reinterpret_cast<uv_timer_t*>(handle);
      });
      timer = nullptr;
    }
  }

  TimePoint Now() const { return Clock::now(); }

  std::string RandomHex(std::size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
      const unsigned value = static_cast<unsigned>(rng() & 0xff);
      out.push_back(kHex[value >> 4]);
      out.push_back(kHex[value & 0x0f]);
    }
    return out;
  }

  void Event(const std::string& text) {
    if (callbacks.on_event)
      callbacks.on_event(text);
  }

  void SetState(SipRegistrationState new_state, const std::string& detail) {
    state = new_state;
    if (callbacks.on_state)
      callbacks.on_state(new_state, detail);
  }

  std::string TransportName() const { return config.tcp ? "TCP" : "UDP"; }

  std::string ContactUri() const {
    SipUri contact;
    contact.scheme = "sip";
    contact.user = config.username;
    contact.host = local_ip;
    contact.port = local_port;
    contact.parameters.emplace_back("transport", config.tcp ? "tcp" : "udp");
    return FormatSipUri(contact);
  }

  bool AddAuthorization(SipMessage* message, std::string_view method, std::string_view uri,
                        std::string* error) {
    if (!have_challenge || config.password.empty())
      return true;
    ++nonce_count;
    const std::string cnonce = RandomHex(16);
    std::string value;
    if (!BuildSipDigestAuthorization(method, uri, challenge,
                                     SipDigestCredentials{config.username, config.password},
                                     cnonce, nonce_count, &value, error)) {
      return false;
    }
    message->AddHeader(use_proxy_auth ? "Proxy-Authorization" : "Authorization", std::move(value));
    return true;
  }

  bool InitTimer(std::string* error) {
    if (timer != nullptr) {
      uv_timer_start(timer, &Impl::OnTimer, 100, 100);
      return true;
    }
    auto* raw_loop = reinterpret_cast<uv_loop_t*>(loop->get_handle());
    timer = new uv_timer_t();
    const int rc = uv_timer_init(raw_loop, timer);
    if (rc != 0) {
      delete timer;
      timer = nullptr;
      if (error)
        *error = std::string("uv_timer_init failed: ") + uv_strerror(rc);
      return false;
    }
    timer->data = this;
    uv_timer_start(timer, &Impl::OnTimer, 100, 100);
    return true;
  }

  static void OnTimer(uv_timer_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    if (self != nullptr)
      self->Tick();
  }

  bool CreateTransport(std::string* error) {
    TransportConfig transport_config;
    transport_config.server_host = config.server_host;
    transport_config.server_port = config.server_port;
    transport_config.bind_ip = config.bind_ip;
    transport_config.local_ip = config.local_ip;
    transport_config.local_port = config.local_port;

    std::unique_ptr<SipTransport> created;
    if (config.tcp) {
      created = std::make_unique<TcpTransport>(loop, transport_config);
    } else {
      created = std::make_unique<UdpTransport>(loop, transport_config);
    }

    SipTransport::Callbacks transport_callbacks;
    transport_callbacks.on_message = [this](std::string raw, std::string peer) {
      OnTransportMessage(std::move(raw), std::move(peer));
    };
    transport_callbacks.on_error = [this](std::string message) {
      OnTransportFailure(std::move(message));
    };
    transport_callbacks.on_connected = [this] { OnTransportConnected(); };
    transport_callbacks.on_disconnected = [this](std::string reason) {
      OnTransportFailure(std::move(reason));
    };

    if (!created->Start(std::move(transport_callbacks), error))
      return false;
    transport = std::move(created);
    local_ip = transport->local_ip();
    local_port = transport->local_port();
    transport_dead = false;
    if (config.tcp) {
      connect_pending = true;
      connect_deadline = Now() + std::chrono::milliseconds(config.connect_timeout_ms);
    }
    return true;
  }

  bool BeginAttempt(std::string* error) {
    if (!transport) {
      if (!CreateTransport(error))
        return false;
    }
    if (!config.tcp) {
      UpdateLocalAddress();
      EnsureIdentity();
      SendRegister(have_challenge, false);
    }
    // TCP sends REGISTER from OnTransportConnected().
    return true;
  }

  void UpdateLocalAddress() {
    if (!transport)
      return;
    const std::string ip = transport->local_ip();
    const std::uint16_t port = transport->local_port();
    if (!ip.empty())
      local_ip = ip;
    if (port != 0)
      local_port = port;
  }

  void EnsureIdentity() {
    if (call_id.empty()) {
      call_id = RandomHex(16) + "@" + (local_ip.empty() ? std::string("unknown") : local_ip);
    }
    if (from_tag.empty())
      from_tag = RandomHex(8);
  }

  void OnTransportConnected() {
    if (!running)
      return;
    CallbackScope callback_scope(this);
    connect_pending = false;
    transport_dead = false;
    UpdateLocalAddress();
    EnsureIdentity();
    local_port = transport ? transport->local_port() : 0;
    if (local_ip.empty() || local_port == 0) {
      RegistrationFailed("TCP connection has no usable local address", true);
      return;
    }
    Event("transport connected to " + (transport ? transport->peer() : std::string()));
    SendRegister(have_challenge, false);
  }

  void OnTransportFailure(std::string reason) {
    if (!running || transport_dead)
      return;
    CallbackScope callback_scope(this);
    connect_pending = false;
    RegistrationFailed(std::move(reason), true);
  }

  void OnTransportMessage(std::string raw, std::string peer) {
    if (!running)
      return;
    CallbackScope callback_scope(this);
    ++received_messages;
    if (callbacks.on_inbound)
      callbacks.on_inbound(raw, peer);
    if (!running)
      return;

    SipMessage message;
    std::size_t consumed = 0;
    std::string error;
    if (SipMessage::Parse(raw, &message, &consumed, &error) != SipParseStatus::kComplete) {
      Event("dropping malformed inbound SIP message: " + error);
      return;
    }
    if (!message.is_request) {
      OnResponse(message);
      return;
    }
    if (message.method == SipMethod::kOptions) {
      SendSimpleResponse(message, 200, "OK");
      Event("answered inbound OPTIONS with 200 OK");
    } else if (message.method == SipMethod::kMessage) {
      // Acknowledge first: MANSCDP answers travel in a separate MESSAGE
      // request, built by the callback through SendSipMessage().
      SendSimpleResponse(message, 200, "OK");
      Event("answered inbound MESSAGE with 200 OK");
      if (callbacks.on_message)
        callbacks.on_message(message);
    } else if (message.method == SipMethod::kInvite) {
      // The media service owns the offer/answer and must call SendResponse()
      // with 200 + SDP (or an error status) before this callback returns.
      if (callbacks.on_invite)
        callbacks.on_invite(message, peer);
      else
        SendSimpleResponse(message, 501, "Not Implemented");
    } else if (message.method == SipMethod::kAck) {
      if (callbacks.on_ack)
        callbacks.on_ack(message);
    } else if (message.method == SipMethod::kBye) {
      SendSimpleResponse(message, 200, "OK");
      Event("answered inbound BYE with 200 OK");
      if (callbacks.on_bye)
        callbacks.on_bye(message);
    } else if (message.method == SipMethod::kInfo) {
      if (callbacks.on_info)
        callbacks.on_info(message);
      else
        SendSimpleResponse(message, 501, "Not Implemented");
    } else if (message.method == SipMethod::kSubscribe) {
      // The callback owns the 200 OK because it must also select the Event
      // package and the Expires value. It can use SendResponse() with the
      // To tag from SipDialogTag().
      if (callbacks.on_subscribe)
        callbacks.on_subscribe(message);
      else
        SendSimpleResponse(message, 501, "Not Implemented");
    } else if (message.method == SipMethod::kNotify) {
      SendSimpleResponse(message, 200, "OK");
      Event("answered inbound NOTIFY with 200 OK");
      if (callbacks.on_notify)
        callbacks.on_notify(message);
    } else if (message.method == SipMethod::kCancel) {
      SendSimpleResponse(message, 481, "Call/Transaction Does Not Exist");
      Event("answered inbound CANCEL with 481");
    } else {
      SendSimpleResponse(message, 501, "Not Implemented");
      Event("answered inbound " + std::string(SipMethodName(message.method)) +
            " with 501 Not Implemented");
    }
  }

  static std::string StableDialogTag(const std::string& call_id) {
    return SipDialogTag(call_id);
  }

  bool SendResponse(const SipMessage& request, int status_code,
                    const std::string& reason, const std::string& body,
                    const std::string& content_type,
                    const std::vector<std::pair<std::string, std::string>>& extra_headers,
                    const std::string& to_tag, std::string* error) {
    SipMessage response;
    response.is_request = false;
    response.status_code = status_code;
    response.reason_phrase = reason;
    for (const SipHeader* via : request.FindHeaders("Via"))
      response.AddHeader("Via", via->value);
    response.AddHeader("From", request.GetHeader("From"));
    std::string to = request.GetHeader("To");
    if (to.empty())
      to = request.GetHeader("t");
    const bool dialog_response =
        (request.method == SipMethod::kInvite || request.method == SipMethod::kSubscribe) &&
        status_code >= 200;
    if (dialog_response && request.ToTag().empty()) {
      const std::string tag =
          to_tag.empty() ? StableDialogTag(request.CallId()) : to_tag;
      if (!to.empty() && to.find(";tag=") == std::string::npos)
        to += ";tag=" + tag;
    }
    response.AddHeader("To", to);
    response.AddHeader("Call-ID", request.GetHeader("Call-ID"));
    response.AddHeader("CSeq", request.GetHeader("CSeq"));
    if (dialog_response && !config.username.empty() && !local_ip.empty()) {
      SipUri contact;
      contact.scheme = "sip";
      contact.user = config.username;
      contact.host = local_ip;
      contact.port = local_port;
      contact.parameters.emplace_back("transport", config.tcp ? "tcp" : "udp");
      response.AddHeader("Contact", "<" + FormatSipUri(contact) + ">");
    }
    response.AddHeader("User-Agent", "UvNVR-GB28181/0.1");
    for (const auto& [name, value] : extra_headers)
      response.AddHeader(name, value);
    if (!content_type.empty()) {
      response.SetHeader("Content-Type", content_type);
      response.SetHeader("Content-Length", std::to_string(body.size()));
      response.body = body;
    } else {
      response.SetHeader("Content-Length", "0");
    }
    return Emit(response.Serialize(), error);
  }

  void SendSimpleResponse(const SipMessage& request, int status, const std::string& reason) {
    std::string error;
    if (!SendResponse(request, status, reason, {}, {}, {}, {}, &error))
      Event("failed to answer " + std::string(SipMethodName(request.method)) + ": " + error);
  }

  void OnResponse(const SipMessage& message) {
    if (!pending)
      return;
    if (message.CSeqNumber() != pending->cseq)
      return;
    const SipMethod expected =
        pending->kind == PendingKind::kRegister ? SipMethod::kRegister : SipMethod::kMessage;
    const SipMethod actual = message.CSeqMethod();
    if (actual != SipMethod::kUnknown && actual != expected)
      return;
    const std::string call_id_header = message.CallId();
    if (!call_id_header.empty() && !call_id.empty() && call_id_header != call_id)
      return;
    if (message.status_code < 200)
      return;  // 100 Trying and other provisional responses

    Pending request = std::move(*pending);
    pending.reset();

    if (message.status_code == 401 || message.status_code == 407) {
      HandleAuthChallenge(message, request);
      return;
    }
    if (message.status_code == 423 && request.kind == PendingKind::kRegister) {
      std::uint32_t min_expires = 0;
      const bool have_min = ParseUint32(message.GetHeader("Min-Expires"), &min_expires);
      if (have_min && min_expires > 0 && interval_adjustments < 1) {
        ++interval_adjustments;
        config.expires_seconds = min_expires;
        Event("registrar requested a longer interval; retrying with Expires=" +
              std::to_string(min_expires));
        SendRegister(have_challenge, request.auth_retried);
      } else {
        RegistrationFailed("REGISTER rejected: 423 Interval Too Brief", false);
      }
      return;
    }
    if (message.status_code >= 200 && message.status_code < 300) {
      HandleSuccess(message, request);
      return;
    }

    RegistrationFailed("request rejected: " + std::to_string(message.status_code) + " " +
                           message.reason_phrase,
                       false);
  }

  void HandleAuthChallenge(const SipMessage& message, const Pending& request) {
    if (request.auth_retried) {
      RegistrationFailed("authentication rejected again after a Digest retry", false);
      return;
    }
    if (config.password.empty()) {
      RegistrationFailed("registrar sent a 401/407 challenge but no password is configured", false);
      return;
    }
    const bool proxy = message.status_code == 407;
    const std::string value = message.GetHeader(proxy ? "Proxy-Authenticate" : "WWW-Authenticate");
    if (value.empty()) {
      RegistrationFailed("authentication challenge response has no challenge header", false);
      return;
    }
    SipDigestChallenge parsed;
    std::string error;
    if (!ParseSipDigestChallenge(value, &parsed, &error)) {
      RegistrationFailed("invalid Digest challenge: " + error, false);
      return;
    }
    challenge = std::move(parsed);
    have_challenge = true;
    use_proxy_auth = proxy;
    nonce_count = 0;
    Event("authentication challenge received (realm=" + challenge.realm + ", algorithm=" +
          challenge.algorithm + ")");
    if (request.kind == PendingKind::kRegister) {
      SendRegister(true, true);
    } else {
      SendKeepalive(true, true);
    }
  }

  void HandleSuccess(const SipMessage& message, const Pending& request) {
    if (request.kind == PendingKind::kKeepalive) {
      Event("keepalive acknowledged");
      return;
    }

    const std::uint32_t expires = RegistrationExpires(message);
    if (expires == 0) {
      RegistrationFailed("registrar returned an empty registration (Expires: 0)", false);
      return;
    }
    const TimePoint now = Now();
    const std::uint64_t expires_ms = static_cast<std::uint64_t>(expires) * 1000ull;
    std::uint64_t margin = expires_ms / 10;
    margin = std::min<std::uint64_t>(margin, 30000);
    margin = std::max<std::uint64_t>(margin, 500);
    std::uint64_t refresh_ms = expires_ms > margin ? expires_ms - margin : 500;
    refresh_at = now + std::chrono::milliseconds(refresh_ms);
    if (config.keepalive_seconds > 0)
      keepalive_at = now + std::chrono::seconds(config.keepalive_seconds);
    else
      keepalive_at = TimePoint::max();
    reg_failures = 0;
    interval_adjustments = 0;
    transport_dead = false;
    connect_pending = false;
    SetState(SipRegistrationState::kRegistered,
             "REGISTER accepted: expires=" + std::to_string(expires) + "s, refresh in " +
                 std::to_string(refresh_ms / 1000) + "s");
    Event("registered with " + (transport ? transport->peer() : std::string("registrar")));
  }

  std::uint32_t RegistrationExpires(const SipMessage& message) const {
    std::uint32_t expires = 0;
    if (ParseUint32(message.GetHeader("Expires"), &expires))
      return expires;
    const std::string contact = message.GetHeader("Contact");
    if (!contact.empty()) {
      SipAddress address;
      std::string error;
      if (ParseSipAddress(contact, &address, &error)) {
        const std::string param = address.Parameter("expires");
        if (ParseUint32(param, &expires))
          return expires;
      }
    }
    return config.expires_seconds;
  }

  void SendRegister(bool with_auth, bool auth_retried) {
    if (!running)
      return;
    if (!transport || !transport->connected()) {
      RegistrationFailed("cannot send REGISTER: transport is not connected", true);
      return;
    }
    UpdateLocalAddress();
    EnsureIdentity();
    if (local_ip.empty() || local_port == 0) {
      RegistrationFailed("cannot send REGISTER: no local address", true);
      return;
    }

    SipUri registrar_uri;
    std::string error;
    if (!ParseSipUri(registrar, &registrar_uri, &error)) {
      RegistrationFailed("invalid registrar URI: " + error, false);
      return;
    }

    ++cseq;
    SipMessage request;
    request.is_request = true;
    request.method = SipMethod::kRegister;
    request.request_uri = registrar_uri;
    request.AddHeader("Via", "SIP/2.0/" + TransportName() + " " +
                                 FormatHostPort(local_ip, local_port) + ";rport;branch=z9hG4bK" +
                                 RandomHex(16));
    request.AddHeader("From", "<" + MakeUri(config.username, domain) + ">;tag=" + from_tag);
    request.AddHeader("To", "<" + MakeUri(config.username, domain) + ">");
    request.AddHeader("Call-ID", call_id);
    request.AddHeader("CSeq", std::to_string(cseq) + " REGISTER");
    request.AddHeader("Contact", "<" + ContactUri() + ">");
    request.AddHeader("Max-Forwards", "70");
    request.AddHeader("User-Agent", "UvNVR-GB28181/0.1");
    request.AddHeader("Expires", std::to_string(config.expires_seconds));
    if (with_auth && !AddAuthorization(&request, "REGISTER", FormatSipUri(registrar_uri), &error)) {
      RegistrationFailed("cannot build Digest Authorization: " + error, false);
      return;
    }

    Pending next;
    next.kind = PendingKind::kRegister;
    next.cseq = cseq;
    next.raw = request.Serialize();
    next.deadline = Now() + std::chrono::milliseconds(config.request_timeout_ms);
    next.retransmit_at = Now() + std::chrono::milliseconds(500);
    next.retransmit_ms = 500;
    next.auth_retried = auth_retried;
    pending = std::move(next);
    const std::string wire = pending->raw;

    SetState(SipRegistrationState::kRegistering,
             "REGISTER sent (CSeq " + std::to_string(cseq) + ", expires=" +
                 std::to_string(config.expires_seconds) + "s)");
    if (!running)
      return;
    if (!Emit(wire, &error)) {
      pending.reset();
      RegistrationFailed("REGISTER send failed: " + error, true);
    }
  }

  void SendKeepalive(bool with_auth, bool auth_retried) {
    if (!running)
      return;
    if (config.keepalive_seconds == 0)
      return;
    if (!transport || !transport->connected()) {
      RegistrationFailed("cannot send keepalive: transport is not connected", true);
      return;
    }
    UpdateLocalAddress();
    EnsureIdentity();

    SipUri registrar_uri;
    std::string error;
    if (!ParseSipUri(registrar, &registrar_uri, &error)) {
      RegistrationFailed("invalid registrar URI: " + error, false);
      return;
    }

    const std::string to_uri = config.server_id.empty() ? MakeUri("", domain)
                                                        : MakeUri(config.server_id, domain);
    ++cseq;
    const std::uint32_t sn = ++keepalive_sn;

    std::string body = "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n";
    body += "<Notify>\r\n";
    body += "<CmdType>Keepalive</CmdType>\r\n";
    body += "<SN>" + std::to_string(sn) + "</SN>\r\n";
    body += "<DeviceID>" + config.username + "</DeviceID>\r\n";
    body += "<Status>OK</Status>\r\n";
    body += "</Notify>\r\n";

    SipMessage request;
    request.is_request = true;
    request.method = SipMethod::kMessage;
    request.request_uri = registrar_uri;
    request.AddHeader("Via", "SIP/2.0/" + TransportName() + " " +
                                 FormatHostPort(local_ip, local_port) + ";rport;branch=z9hG4bK" +
                                 RandomHex(16));
    request.AddHeader("From", "<" + MakeUri(config.username, domain) + ">;tag=" + from_tag);
    request.AddHeader("To", "<" + to_uri + ">");
    request.AddHeader("Call-ID", call_id);
    request.AddHeader("CSeq", std::to_string(cseq) + " MESSAGE");
    request.AddHeader("Max-Forwards", "70");
    request.AddHeader("User-Agent", "UvNVR-GB28181/0.1");
    request.AddHeader("Content-Type", "Application/MANSCDP+xml");
    request.body = std::move(body);
    if (with_auth && !AddAuthorization(&request, "MESSAGE", FormatSipUri(registrar_uri), &error)) {
      RegistrationFailed("cannot build keepalive Digest Authorization: " + error, false);
      return;
    }

    Pending next;
    next.kind = PendingKind::kKeepalive;
    next.cseq = cseq;
    next.raw = request.Serialize();
    next.deadline = Now() + std::chrono::milliseconds(config.request_timeout_ms);
    next.retransmit_at = Now() + std::chrono::milliseconds(500);
    next.retransmit_ms = 500;
    next.auth_retried = auth_retried;
    pending = std::move(next);

    if (!Emit(pending->raw, &error)) {
      pending.reset();
      RegistrationFailed("keepalive send failed: " + error, true);
      return;
    }
    keepalive_at = Now() + std::chrono::seconds(config.keepalive_seconds);
    Event("keepalive sent (SN=" + std::to_string(sn) + ")");
  }

  bool SendMessage(std::string body, const std::string& content_type, std::string* error) {
    if (!running) {
      if (error)
        *error = "SIP user agent is stopped";
      return false;
    }
    if (!transport || !transport->connected()) {
      if (error)
        *error = "cannot send MESSAGE: transport is not connected";
      return false;
    }
    UpdateLocalAddress();
    EnsureIdentity();

    SipUri registrar_uri;
    std::string error_text;
    if (!ParseSipUri(registrar, &registrar_uri, &error_text)) {
      if (error)
        *error = "invalid registrar URI: " + error_text;
      return false;
    }
    const std::string to_uri = config.server_id.empty() ? MakeUri("", domain)
                                                        : MakeUri(config.server_id, domain);
    ++cseq;
    SipMessage request;
    request.is_request = true;
    request.method = SipMethod::kMessage;
    request.request_uri = registrar_uri;
    request.AddHeader("Via", "SIP/2.0/" + TransportName() + " " +
                                 FormatHostPort(local_ip, local_port) + ";rport;branch=z9hG4bK" +
                                 RandomHex(16));
    request.AddHeader("From", "<" + MakeUri(config.username, domain) + ">;tag=" + from_tag);
    request.AddHeader("To", "<" + to_uri + ">");
    request.AddHeader("Call-ID", call_id);
    request.AddHeader("CSeq", std::to_string(cseq) + " MESSAGE");
    request.AddHeader("Max-Forwards", "70");
    request.AddHeader("User-Agent", "UvNVR-GB28181/0.1");
    request.AddHeader("Content-Type", content_type);
    request.body = std::move(body);
    if (!AddAuthorization(&request, "MESSAGE", FormatSipUri(registrar_uri), &error_text)) {
      if (error)
        *error = "cannot build MESSAGE Digest Authorization: " + error_text;
      return false;
    }
    if (!Emit(request.Serialize(), error))
      return false;
    Event("MESSAGE sent (CSeq " + std::to_string(cseq) + ", " +
          std::to_string(request.body.size()) + " body bytes)");
    return true;
  }

  bool SendNotify(const SipNotifyRequest& notify, std::string* error) {
    if (!running) {
      if (error != nullptr)
        *error = "SIP user agent is stopped";
      return false;
    }
    if (!transport || !transport->connected()) {
      if (error != nullptr)
        *error = "cannot send NOTIFY: transport is not connected";
      return false;
    }
    if (notify.call_id.empty() || notify.from.empty() || notify.to.empty()) {
      if (error != nullptr)
        *error = "NOTIFY dialog is missing Call-ID, From or To";
      return false;
    }
    UpdateLocalAddress();
    EnsureIdentity();

    SipUri target;
    std::string target_text = notify.request_uri;
    if (target_text.empty())
      target_text = registrar;
    // A Contact header arrives as <sip:...>; strip the angle brackets before
    // the URI parser sees it.
    if (target_text.size() >= 2 && target_text.front() == '<' && target_text.back() == '>')
      target_text = target_text.substr(1, target_text.size() - 2);
    std::string parse_error;
    if (!ParseSipUri(target_text, &target, &parse_error)) {
      if (error != nullptr)
        *error = "invalid NOTIFY target URI: " + parse_error;
      return false;
    }

    std::uint32_t& dialog_cseq = notify_cseq[notify.call_id];
    ++dialog_cseq;

    SipMessage request;
    request.is_request = true;
    request.method = SipMethod::kNotify;
    request.request_uri = target;
    request.AddHeader("Via", "SIP/2.0/" + TransportName() + " " +
                                 FormatHostPort(local_ip, local_port) + ";rport;branch=z9hG4bK" +
                                 RandomHex(16));
    request.AddHeader("From", notify.from);
    request.AddHeader("To", notify.to);
    request.AddHeader("Call-ID", notify.call_id);
    request.AddHeader("CSeq", std::to_string(dialog_cseq) + " NOTIFY");
    request.AddHeader("Max-Forwards", "70");
    request.AddHeader("User-Agent", "UvNVR-GB28181/0.1");
    if (!config.username.empty() && !local_ip.empty())
      request.AddHeader("Contact", "<" + ContactUri() + ">");
    if (!notify.event.empty())
      request.AddHeader("Event", notify.event);
    std::string state = notify.subscription_state.empty() ? "active" : notify.subscription_state;
    if ((state == "active" || state == "pending") && notify.expires_seconds > 0)
      state += ";expires=" + std::to_string(notify.expires_seconds);
    request.AddHeader("Subscription-State", state);
    if (!notify.body.empty()) {
      request.AddHeader("Content-Type", notify.content_type.empty()
                                             ? std::string("Application/MANSCDP+xml")
                                             : notify.content_type);
      request.body = notify.body;
    }
    if (have_challenge && !config.password.empty()) {
      std::string auth_error;
      if (!AddAuthorization(&request, "NOTIFY", FormatSipUri(target), &auth_error)) {
        if (error != nullptr)
          *error = "cannot build NOTIFY Digest Authorization: " + auth_error;
        return false;
      }
    }
    if (!Emit(request.Serialize(), error))
      return false;
    Event("NOTIFY sent (" + notify.event + ", " + state + ", " +
          std::to_string(request.body.size()) + " body bytes)");
    return true;
  }

  bool Emit(std::string raw, std::string* error) {
    ++sent_messages;
    if (callbacks.on_outbound)
      callbacks.on_outbound(raw);
    if (!running)
      return false;
    if (!transport) {
      if (error)
        *error = "no active transport";
      return false;
    }
    return transport->Send(raw, error);
  }

  void RegistrationFailed(std::string reason, bool transport_lost) {
    if (!running)
      return;
    pending.reset();
    connect_pending = false;
    if (transport_lost)
      transport_dead = true;
    ++reg_failures;
    const std::uint32_t delay = ComputeRegistrationBackoffMs(
        reg_failures, config.initial_backoff_ms, config.max_backoff_ms);
    retry_at = Now() + std::chrono::milliseconds(delay);
    SetState(SipRegistrationState::kBackoff,
             reason + "; retry in " + std::to_string(delay) + " ms (attempt " +
                 std::to_string(reg_failures) + ")");
  }

  void OnRequestTimeout() {
    if (!pending)
      return;
    const bool was_register = pending->kind == PendingKind::kRegister;
    pending.reset();
    RegistrationFailed(std::string(was_register ? "REGISTER" : "keepalive") +
                           " timed out after " + std::to_string(config.request_timeout_ms) + " ms",
                       config.tcp);
  }

  void Tick() {
    const TimePoint now = Now();

    // Stop() called from a transport callback only marks the teardown; the
    // transport object must outlive the callback that is using it.
    if (stop_deferred && callback_depth == 0) {
      transport.reset();
      transport_dead = false;
      stop_deferred = false;
    }
    if (!running) {
      if (timer != nullptr)
        uv_timer_stop(timer);
      return;
    }

    if (transport_dead) {
      transport.reset();
      transport_dead = false;
    }

    if (connect_pending && now >= connect_deadline) {
      connect_pending = false;
      RegistrationFailed("TCP connect timed out after " +
                             std::to_string(config.connect_timeout_ms) + " ms",
                         true);
      return;
    }

    if (pending) {
      if (now >= pending->deadline) {
        OnRequestTimeout();
        return;
      }
      if (!config.tcp && now >= pending->retransmit_at) {
        pending->retransmit_ms = std::min<std::uint32_t>(pending->retransmit_ms * 2, 4000);
        pending->retransmit_at = now + std::chrono::milliseconds(pending->retransmit_ms);
        std::string error;
        if (!Emit(pending->raw, &error)) {
          pending.reset();
          RegistrationFailed("retransmission failed: " + error, true);
          return;
        }
      }
    }

    if (state == SipRegistrationState::kBackoff && now >= retry_at) {
      std::string error;
      if (!BeginAttempt(&error)) {
        RegistrationFailed("transport setup failed: " + error, true);
      }
      return;
    }

    if (state == SipRegistrationState::kRegistered && !pending) {
      if (now >= refresh_at) {
        SendRegister(have_challenge, false);
        return;
      }
      if (config.keepalive_seconds > 0 && now >= keepalive_at) {
        SendKeepalive(have_challenge, false);
      }
    }
  }

  bool Validate(std::string* error) const {
    if (loop == nullptr) {
      if (error)
        *error = "event loop is null";
      return false;
    }
    if (config.server_host.empty()) {
      if (error)
        *error = "server_host is empty";
      return false;
    }
    if (config.server_port == 0) {
      if (error)
        *error = "server_port is zero";
      return false;
    }
    if (config.username.empty()) {
      if (error)
        *error = "username is empty";
      return false;
    }
    if (config.request_timeout_ms == 0) {
      if (error)
        *error = "request_timeout_ms must be greater than zero";
      return false;
    }
    if (config.connect_timeout_ms == 0) {
      if (error)
        *error = "connect_timeout_ms must be greater than zero";
      return false;
    }
    SipUri parsed;
    std::string parse_error;
    if (!ParseSipUri(registrar, &parsed, &parse_error)) {
      if (error)
        *error = "invalid registrar URI: " + parse_error;
      return false;
    }
    return true;
  }

  bool Start(std::string* error) {
    if (running)
      return true;
    domain = config.domain.empty() ? config.server_host : config.domain;
    registrar = config.registrar_uri.empty()
                    ? MakeUri(config.server_id, domain)
                    : std::string(Trim(config.registrar_uri));
    if (!Validate(error))
      return false;
    if (config.keepalive_seconds > 0 && config.keepalive_seconds < 5)
      Event("keepalive interval is shorter than 5 s; this is suitable for lab tests only");
    if (!InitTimer(error))
      return false;

    running = true;
    transport_dead = false;
    stop_deferred = false;
    connect_pending = false;
    reg_failures = 0;
    interval_adjustments = 0;
    have_challenge = false;
    use_proxy_auth = false;
    nonce_count = 0;
    call_id.clear();
    from_tag.clear();
    local_ip.clear();
    local_port = 0;
    SetState(SipRegistrationState::kConnecting,
             "starting " + TransportName() + " transport toward " + config.server_host + ":" +
                 std::to_string(config.server_port));
    std::string setup_error;
    if (!BeginAttempt(&setup_error)) {
      running = false;
      SetState(SipRegistrationState::kStopped, "transport setup failed: " + setup_error);
      if (error)
        *error = setup_error;
      return false;
    }
    return true;
  }

  void Stop() {
    if (!running) {
      state = SipRegistrationState::kStopped;
      return;
    }
    running = false;
    pending.reset();
    connect_pending = false;
    if (transport) {
      if (callback_depth > 0) {
        // Stop() was called from a transport callback: defer the teardown to
        // Tick() so the callback does not return into freed state.
        stop_deferred = true;
        transport_dead = true;
      } else {
        transport.reset();
        transport_dead = false;
        local_port = 0;
      }
    }
    if (!stop_deferred && timer != nullptr)
      uv_timer_stop(timer);
    SetState(SipRegistrationState::kStopped, "stopped");
  }

  uvcpp::uvcpp_loop* loop;
  SipUaConfig config;
  SipUaCallbacks callbacks;
  uv_timer_t* timer = nullptr;
  std::unique_ptr<SipTransport> transport;

  bool running = false;
  SipRegistrationState state = SipRegistrationState::kStopped;
  bool transport_dead = false;
  bool stop_deferred = false;
  int callback_depth = 0;
  bool connect_pending = false;
  TimePoint connect_deadline{};

  std::string domain;
  std::string registrar;
  std::string local_ip;
  std::uint16_t local_port = 0;
  std::string call_id;
  std::string from_tag;
  std::uint32_t cseq = 0;
  std::uint32_t keepalive_sn = 0;
  std::map<std::string, std::uint32_t> notify_cseq;

  SipDigestChallenge challenge;
  bool have_challenge = false;
  bool use_proxy_auth = false;
  std::uint32_t nonce_count = 0;

  std::optional<Pending> pending;

  std::uint32_t reg_failures = 0;
  int interval_adjustments = 0;
  TimePoint retry_at{};
  TimePoint refresh_at{};
  TimePoint keepalive_at{};

  std::uint64_t sent_messages = 0;
  std::uint64_t received_messages = 0;
  std::mt19937_64 rng;
};

std::string SipDialogTag(std::string_view call_id) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char c : call_id) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;
  }
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}

SipUserAgent::SipUserAgent(uvcpp::uvcpp_loop* loop, SipUaConfig config)
    : impl_(std::make_unique<Impl>(loop, std::move(config))) {}

SipUserAgent::~SipUserAgent() = default;

void SipUserAgent::set_callbacks(SipUaCallbacks callbacks) {
  impl_->callbacks = std::move(callbacks);
}

bool SipUserAgent::Start(std::string* error) {
  return impl_->Start(error);
}

void SipUserAgent::Stop() {
  impl_->Stop();
}

bool SipUserAgent::SendSipMessage(std::string body, std::string content_type,
                               std::string* error) {
  return impl_->SendMessage(std::move(body), content_type, error);
}

bool SipUserAgent::SendNotify(const SipNotifyRequest& notify, std::string* error) {
  return impl_->SendNotify(notify, error);
}

bool SipUserAgent::SendResponse(
    const SipMessage& request, int status_code, const std::string& reason,
    const std::string& body, const std::string& content_type,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    const std::string& to_tag, std::string* error) {
  return impl_->SendResponse(request, status_code, reason, body, content_type,
                             extra_headers, to_tag, error);
}

SipRegistrationState SipUserAgent::state() const {
  return impl_->state;
}

const SipUaConfig& SipUserAgent::config() const {
  return impl_->config;
}

std::string SipUserAgent::local_ip() const {
  return impl_->local_ip;
}

std::uint16_t SipUserAgent::local_port() const {
  return impl_->local_port;
}

std::uint64_t SipUserAgent::sent_messages() const {
  return impl_->sent_messages;
}

std::uint64_t SipUserAgent::received_messages() const {
  return impl_->received_messages;
}

}  // namespace uvnvr::gb28181
