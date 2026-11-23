#pragma once

#include "proto/proto_protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace uvcpp {
class uvcpp_loop;
class uvcpp_tcp_client;
class uvcpp_quic_connection;
} // namespace uvcpp

namespace uvnvr::proto {

using json = nlohmann::json;

// Result of credential verification, supplied by the recorder (App). The
// dispatcher owns the password check so the protocol layer never touches the
// user database.
struct AuthResult {
  bool ok = false;
  int status = 401;
  std::string code = "invalid_credentials";
  std::string message = "invalid username or password";
  int64_t user_id = 0;
  std::string username;
  std::string role;
  std::vector<std::string> permissions;
};

// Read-only view of an authenticated session, copied into each request.
struct SessionView {
  uint64_t session_id = 0;
  int64_t user_id = 0;
  std::string username;
  std::string role;
  std::vector<std::string> permissions;
};

class ProtoConnection;

struct ProtoRequest {
  Header header;
  std::string payload; // JSON text (or binary for streamed opcodes)
  std::string peer;
  uint64_t connection_id = 0;
  std::shared_ptr<ProtoConnection> connection;
  bool has_session = false;
  SessionView session;
};

struct ProtoReply {
  int status = 200;
  std::string code;
  std::string message;
  json data = json::object();
  // 0 = reply with the request opcode. LIVE_OPEN uses LIVE_ACK and
  // DOWNLOAD_OPEN uses DOWNLOAD_BEGIN instead.
  uint16_t opcode = 0;
  uint16_t extra_flags = 0;

  bool ok() const { return status >= 200 && status < 300; }
};

using ProtoCompletion = std::function<void(ProtoReply)>;
using ProtoRequestHandler = std::function<void(std::shared_ptr<ProtoRequest>, ProtoCompletion)>;
using ProtoAuthHandler = std::function<AuthResult(const std::string& username,
                                                  const std::string& password,
                                                  const std::string& peer)>;

// One client connection, abstracting the TCP and QUIC framing difference.
// All methods run on the server's event-loop thread.
class ProtoConnection : public std::enable_shared_from_this<ProtoConnection> {
public:
  using Ptr = std::shared_ptr<ProtoConnection>;

  ~ProtoConnection();

  ProtoConnection(const ProtoConnection&) = delete;
  ProtoConnection& operator=(const ProtoConnection&) = delete;

  uint64_t id() const { return id_; }
  const std::string& peer() const { return peer_; }
  bool alive() const { return alive_; }
  bool quic() const { return is_quic_; }

  void SetFrameHandler(std::function<void(Ptr, const Header&, std::string)> handler) {
    frame_handler_ = std::move(handler);
  }
  void AddCleanup(std::function<void()> cleanup);

  // Sends one framed control message. `payload` is copied into the frame.
  bool SendFrame(uint16_t opcode, uint16_t flags, uint64_t session_id, uint32_t request_id,
                 const void* payload, size_t payload_len);
  bool SendJson(uint16_t opcode, uint16_t flags, uint64_t session_id, uint32_t request_id,
                const json& payload);
  // Prefixes `stream_id` and sets STREAM before sending binary payload bytes.
  // codec/frame_flags/pts_ms fill the LIVE_FRAME header at offsets 4, 5 and 8;
  // download chunks pass the defaults and carry their own offset instead.
  bool SendStreamFrame(uint16_t opcode, uint32_t stream_id, const void* payload,
                       size_t payload_len, uint8_t codec = 0, uint8_t frame_flags = 0,
                       uint64_t pts_ms = 0);
  bool SendError(uint16_t request_opcode, uint64_t session_id, uint32_t request_id, int status,
                 const std::string& code, const std::string& message);

  void Close();

  // Transport entry points used by ProtoServer.
  void AttachTcp(uvcpp::uvcpp_loop* loop, uvcpp::uvcpp_tcp_client* client, std::string peer,
                 uint32_t idle_timeout_ms);
  void AttachQuic(uvcpp::uvcpp_quic_connection* connection, std::string peer);
  void OnTcpRead(const char* data, size_t len, bool closed, int error);
  void OnQuicRead(int64_t stream_id, const char* data, size_t len, bool closed, int error);
  void OnQuicStreamOpen(int64_t stream_id);
  void OnQuicClose();
  void OnIdleTimeout();

private:
  friend class ProtoServer;
  ProtoConnection(uint64_t id, bool is_quic);

  void AppendInput(const char* data, size_t len);
  void ParseInput();
  void Deliver(const Header& header, std::string payload);
  void PumpTcpWrites();
  void FlushQuicQueue();
  void MarkClosed();
  void StartIdleTimer();
  void StopIdleTimer();
  void ResetIdleTimer();

  uint64_t id_ = 0;
  bool is_quic_ = false;
  bool alive_ = true;
  bool closing_ = false;
  std::string peer_;
  std::string input_;
  std::vector<uint8_t> pending_output_;
  std::function<void(Ptr, const Header&, std::string)> frame_handler_;
  std::vector<std::function<void()>> cleanups_;
  bool cleanups_ran_ = false;

  // TCP
  uvcpp::uvcpp_loop* loop_ = nullptr;
  uvcpp::uvcpp_tcp_client* tcp_ = nullptr;
  struct IdleTimer;
  IdleTimer* idle_ = nullptr;
  std::vector<std::vector<uint8_t>> tcp_outbox_;
  size_t tcp_outbox_bytes_ = 0;
  bool tcp_write_in_flight_ = false;
  uint32_t idle_timeout_ms_ = 45000;

  // QUIC (only touched when is_quic_)
  uvcpp::uvcpp_quic_connection* quic_connection_ = nullptr;
  int64_t quic_control_stream_ = -1;
  std::vector<std::vector<uint8_t>> quic_pending_;

  // Caps a slow consumer without letting a live stream grow memory without
  // bound. Downloads stop rather than silently drop chunks.
  size_t max_outbox_bytes_ = 64u * 1024u * 1024u;
};

class ProtoServer {
public:
  struct Options {
    uint16_t port = 1515;
    bool tcp = true;
    bool quic = true;
    std::string bind_ip = "0.0.0.0";
    std::string tls_cert; // empty = generate an in-memory self-signed cert
    std::string tls_key;
    uint32_t session_ttl_seconds = 3600;
    uint32_t heartbeat_ms = 15000;
    uint32_t idle_timeout_ms = 45000;
  };

  ProtoServer(uvcpp::uvcpp_loop* loop, ProtoAuthHandler auth_handler,
              ProtoRequestHandler request_handler);
  ~ProtoServer();

  ProtoServer(const ProtoServer&) = delete;
  ProtoServer& operator=(const ProtoServer&) = delete;

  int Start(const Options& options, std::string* error);
  void Stop();

  bool tcp_enabled() const { return tcp_enabled_; }
  bool quic_enabled() const { return quic_enabled_; }
  uint16_t port() const { return options_.port; }

  // Used by App::HandleProtoRequest to answer asynchronously.
  void Reply(const ProtoConnection::Ptr& connection, const Header& request,
             const ProtoReply& reply);

private:
  void OnTcpClient(uvcpp::uvcpp_tcp_client* client);
  void OnQuicConnection(uvcpp::uvcpp_quic_connection* connection);
  void OnFrame(const ProtoConnection::Ptr& connection, const Header& header, std::string payload);
  void OnConnectionClosed(uint64_t connection_id);
  void HandleHello(const ProtoConnection::Ptr& connection, const Header& header,
                   const json& payload);
  void HandleAuth(const ProtoConnection::Ptr& connection, const Header& header,
                  const json& payload);
  void SendReply(const ProtoConnection::Ptr& connection, const Header& request, int status,
                 const std::string& code, const std::string& message, const json& data);
  void SendError(const ProtoConnection::Ptr& connection, const Header& request, int status,
                 const std::string& code, const std::string& message);
  SessionView* FindSession(uint64_t session_id);

  uvcpp::uvcpp_loop* loop_;
  ProtoAuthHandler auth_handler_;
  ProtoRequestHandler request_handler_;
  Options options_;
  bool tcp_enabled_ = false;
  bool quic_enabled_ = false;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unordered_map<uint64_t, ProtoConnection::Ptr> connections_;
  uint64_t next_connection_id_ = 1;

  struct Session {
    SessionView view;
    // steady_clock so wall-clock adjustments cannot extend a session.
    std::chrono::steady_clock::time_point expires;
  };
  std::unordered_map<uint64_t, Session> sessions_;
};

} // namespace uvnvr::proto
