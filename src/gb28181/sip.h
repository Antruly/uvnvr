#pragma once

// Minimal GB/T 28181 SIP user agent (P6.2a).
//
// Scope of this stage:
//   * SIP message parsing and serialization (request line, status line,
//     Via/From/To/Call-ID/CSeq/Contact, compact header forms);
//   * SIP URI and address parsing;
//   * RFC 2617 / RFC 3261 Digest authentication (MD5 and MD5-sess);
//   * REGISTER with 401/407 challenge retry, Expires refresh and unregister;
//   * MANSCDP-style Keepalive MESSAGE heartbeats;
//   * UDP and TCP transports on the libuv event loop, with exponential
//     backoff reconnect after a transport or registration failure.
//
// The module deliberately depends only on libuv/libuvcpp and OpenSSL's MD5.
// It does not pull in any GPL/LGPL SIP stack, and it is compiled only when
// UVNVR_ENABLE_GB28181 is on (the default is off).
//
// Threading: every public method and callback runs on the event-loop thread
// that owns `uvcpp_loop`. The user agent must not be started or destroyed
// concurrently from another thread.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uvcpp {
class uvcpp_loop;
}  // namespace uvcpp

namespace uvnvr::gb28181 {

// ---------------------------------------------------------------------------
// SIP methods and message model
// ---------------------------------------------------------------------------

enum class SipMethod {
  kRegister,
  kInvite,
  kAck,
  kBye,
  kCancel,
  kOptions,
  kMessage,
  kInfo,
  kPrack,
  kSubscribe,
  kNotify,
  kRefer,
  kUnknown,
};

std::string_view SipMethodName(SipMethod method);
SipMethod ParseSipMethod(std::string_view name);

struct SipUri {
  std::string scheme = "sip";
  std::string user;
  std::string password;  // SIP userinfo "user:password" form; rarely used
  std::string user_parameters;  // parameters that belong to the userinfo part
  std::string host;
  std::uint16_t port = 0;  // 0 means "use the scheme default"
  std::vector<std::pair<std::string, std::string>> parameters;
  std::vector<std::pair<std::string, std::string>> headers;

  // Case-insensitive parameter lookup. Returns an empty string when absent.
  std::string Parameter(std::string_view name) const;
  // Case-insensitive header lookup (the part after '?').
  std::string Header(std::string_view name) const;
};

bool ParseSipUri(std::string_view text, SipUri* uri, std::string* error = nullptr);
std::string FormatSipUri(const SipUri& uri);

struct SipAddress {
  std::string display_name;
  SipUri uri;
  // Header parameters that follow the address (for example ";tag=...").
  std::vector<std::pair<std::string, std::string>> parameters;

  std::string Parameter(std::string_view name) const;
};

bool ParseSipAddress(std::string_view text, SipAddress* address, std::string* error = nullptr);
std::string FormatSipAddress(const SipAddress& address);

struct SipHeader {
  std::string name;
  std::string value;
};

// Case-insensitive comparison that also understands the RFC 3261 compact
// forms (v=Via, f=From, t=To, i=Call-ID, m=Contact, l=Content-Length, ...).
bool HeaderNameEquals(std::string_view a, std::string_view b);
// Maps a compact header name to its long form; other names are returned
// unchanged (case preserved).
std::string CanonicalHeaderName(std::string_view name);

enum class SipParseStatus {
  kComplete,
  kIncomplete,  // valid prefix; more bytes are needed (TCP framing)
  kInvalid,
};

class SipMessage {
 public:
  bool is_request = true;
  SipMethod method = SipMethod::kUnknown;
  SipUri request_uri;
  int status_code = 0;
  std::string reason_phrase;
  std::vector<SipHeader> headers;
  std::string body;

  // Parses one message. `consumed` receives the number of bytes the message
  // occupies, which lets a stream splitter handle coalesced messages.
  // A missing Content-Length is treated as a zero-length body. Extra bytes
  // after the body are left for the caller.
  static SipParseStatus Parse(std::string_view data, SipMessage* message,
                              std::size_t* consumed = nullptr,
                              std::string* error = nullptr);

  std::string Serialize() const;

  const SipHeader* FindHeader(std::string_view name) const;
  std::vector<const SipHeader*> FindHeaders(std::string_view name) const;
  std::string GetHeader(std::string_view name) const;
  void SetHeader(std::string name, std::string value);
  void AddHeader(std::string name, std::string value);
  void RemoveHeader(std::string_view name);

  std::string CallId() const;
  // Returns 0 when the header is missing or malformed.
  std::uint32_t CSeqNumber() const;
  std::string CSeqMethodRaw() const;
  SipMethod CSeqMethod() const;
  std::string FromTag() const;
  std::string ToTag() const;
};

// ---------------------------------------------------------------------------
// Digest authentication
// ---------------------------------------------------------------------------

struct SipDigestChallenge {
  std::string realm;
  std::string nonce;
  std::string opaque;
  std::string algorithm = "MD5";
  std::vector<std::string> qop;
  bool stale = false;
};

struct SipDigestCredentials {
  std::string username;
  std::string password;
};

// Parses a WWW-Authenticate / Proxy-Authenticate value. Only the Digest
// scheme is accepted.
bool ParseSipDigestChallenge(std::string_view value, SipDigestChallenge* challenge,
                             std::string* error = nullptr);

// Builds the value of an Authorization / Proxy-Authorization header.
// `cnonce` and `nonce_count` are explicit so callers (and tests) control the
// replay-protection state.
bool BuildSipDigestAuthorization(std::string_view method, std::string_view uri,
                                 const SipDigestChallenge& challenge,
                                 const SipDigestCredentials& credentials,
                                 std::string_view cnonce, std::uint32_t nonce_count,
                                 std::string* authorization, std::string* error = nullptr);

// Lower-case hex MD5. Exposed because the digest response can be checked
// against `openssl dgst -md5` in the unit tests.
std::string SipDigestMd5Hex(std::string_view data);

// ---------------------------------------------------------------------------
// Registration user agent
// ---------------------------------------------------------------------------

enum class SipRegistrationState {
  kStopped,
  kConnecting,
  kRegistering,
  kRegistered,
  kBackoff,
};

std::string_view SipRegistrationStateName(SipRegistrationState state);

struct SipUaConfig {
  // Registrar/platform address.
  std::string server_host;
  std::uint16_t server_port = 5060;
  bool tcp = false;

  // Device credentials. GB/T 28181 uses the 20-digit device ID as the SIP
  // user name; `password` may be empty for a registrar without authentication.
  std::string username;
  std::string password;

  // Optional ID of the platform (To/Request-URI user part for MESSAGE).
  std::string server_id;

  // The SIP domain/realm. Defaults to server_host.
  std::string domain;
  // Full REGISTER/MESSAGE request URI. Defaults to sip:[server_id@]domain.
  std::string registrar_uri;

  // Transport binding. `local_ip` is advertised in Via/Contact and must be an
  // address the platform can reach. When empty, a numeric server_host is used
  // (correct for loopback and lab networks).
  std::string local_ip;
  std::uint16_t local_port = 0;  // 0 = ephemeral (UDP) / OS-assigned (TCP)
  std::string bind_ip;           // default 0.0.0.0 (or :: for IPv6)

  std::uint32_t expires_seconds = 3600;
  std::uint32_t keepalive_seconds = 60;  // 0 disables the heartbeat

  std::uint32_t request_timeout_ms = 32000;
  std::uint32_t connect_timeout_ms = 10000;
  std::uint32_t initial_backoff_ms = 1000;
  std::uint32_t max_backoff_ms = 60000;
};

struct SipUaCallbacks {
  // Called on every state transition and on every backoff retry.
  std::function<void(SipRegistrationState, const std::string&)> on_state;
  // Wire log: the exact bytes sent to the platform.
  std::function<void(const std::string& raw)> on_outbound;
  // Wire log: one complete inbound message and the peer it came from.
  std::function<void(const std::string& raw, const std::string& peer)> on_inbound;
  // Called after an inbound MESSAGE request has been acknowledged with
  // 200 OK. The callback runs on the event-loop thread and may call
  // SendMessage() to return a MANSCDP response. It must not block.
  std::function<void(const SipMessage&)> on_message;
  // Human-readable progress information (keepalive sent/acknowledged, ...).
  std::function<void(const std::string&)> on_event;
};

class SipUserAgent {
 public:
  SipUserAgent(uvcpp::uvcpp_loop* loop, SipUaConfig config);
  ~SipUserAgent();

  SipUserAgent(const SipUserAgent&) = delete;
  SipUserAgent& operator=(const SipUserAgent&) = delete;

  void set_callbacks(SipUaCallbacks callbacks);

  // Starts the transport and the initial REGISTER. Returns false only for a
  // configuration or immediate transport setup error; a refused connection or
  // an unanswered REGISTER is retried with exponential backoff.
  bool Start(std::string* error = nullptr);
  void Stop();

  // Sends a MANSCDP body to the registrar in a MESSAGE request. This is the
  // device -> platform direction used for Catalog/DeviceInfo/DeviceStatus
  // responses. The call is fire-and-forget: the request is authenticated with
  // the current Digest challenge (when one is available) but its final response
  // is not retransmitted or tracked, so it must not be used for REGISTER or
  // Keepalive. Returns false when the transport is not ready or no Digest
  // authorization can be built.
  bool SendMessage(std::string body,
                   std::string content_type = "Application/MANSCDP+xml",
                   std::string* error = nullptr);

  SipRegistrationState state() const;
  const SipUaConfig& config() const;

  std::string local_ip() const;
  std::uint16_t local_port() const;
  std::uint64_t sent_messages() const;
  std::uint64_t received_messages() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Exponential backoff used for registration retries. `attempt` starts at 1:
// attempt 1 gives initial_ms, attempt 2 gives 2*initial_ms, ... capped at
// max_ms. Exposed for tests and for operators reading the logs.
std::uint32_t ComputeRegistrationBackoffMs(std::uint32_t attempt,
                                           std::uint32_t initial_ms,
                                           std::uint32_t max_ms);

}  // namespace uvnvr::gb28181
