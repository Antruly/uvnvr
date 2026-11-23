#include "proto/proto_server.h"

#include <uv.h>
#include <uvcpp/uvcpp_config.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <uvnvr/version.h>

#if defined(UVNVR_ENABLE_QUIC) && UVNVR_ENABLE_QUIC && defined(UVCPP_QUIC_ENABLE) && UVCPP_QUIC_ENABLE
#define UVNVR_HAVE_QUIC 1
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

#include <openssl/rand.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uvnvr::proto {
namespace {

bool TraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("UVNVR_PROTO_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

uint64_t RandomSessionId() {
  for (;;) {
    uint8_t bytes[8] = {};
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1)
      continue;
    uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    if (value != 0)
      return value;
  }
}

json ParseObject(const std::string& text) {
  if (text.empty())
    return json::object();
  json value = json::parse(text, nullptr, false);
  if (value.is_discarded() || !value.is_object())
    return json::object();
  return value;
}

std::string JsonString(const json& object, const char* key, const std::string& fallback = "") {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string())
    return fallback;
  return it->get<std::string>();
}

} // namespace

struct ProtoConnection::IdleTimer {
  uv_timer_t handle{};
  std::weak_ptr<ProtoConnection> owner;
};

ProtoConnection::ProtoConnection(uint64_t id, bool is_quic) : id_(id), is_quic_(is_quic) {}

ProtoConnection::~ProtoConnection() {
  MarkClosed();
  StopIdleTimer();
}

void ProtoConnection::MarkClosed() {
  alive_ = false;
  quic_connection_ = nullptr;
  if (cleanups_ran_)
    return;
  cleanups_ran_ = true;
  auto cleanups = std::move(cleanups_);
  cleanups_.clear();
  for (auto& cleanup : cleanups) {
    if (cleanup)
      cleanup();
  }
}

void ProtoConnection::AddCleanup(std::function<void()> cleanup) {
  if (!cleanup)
    return;
  if (cleanups_ran_)
    cleanup();
  else
    cleanups_.push_back(std::move(cleanup));
}

void ProtoConnection::AttachTcp(uvcpp::uvcpp_loop* loop, uvcpp::uvcpp_tcp_client* client,
                                std::string peer, uint32_t idle_timeout_ms) {
  loop_ = loop;
  tcp_ = client;
  peer_ = std::move(peer);
  idle_timeout_ms_ = idle_timeout_ms;
  StartIdleTimer();
  auto self = weak_from_this();
  const int read_rc = tcp_->read_start_events(
      [self](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& result) {
        auto connection = self.lock();
        if (!connection)
          return;
        if (result.event == uvcpp::net_read_event::DATA) {
          connection->OnTcpRead(result.data, result.size, false, 0);
        } else {
          const int error = result.event == uvcpp::net_read_event::READ_ERROR ? result.error : 0;
          connection->OnTcpRead(nullptr, 0, true, error);
        }
      });
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] tcp attach id=%llu peer=%s read_start_events=%d\n",
                 static_cast<unsigned long long>(id_), peer_.c_str(), read_rc);
}

void ProtoConnection::AttachQuic(uvcpp::uvcpp_quic_connection* connection, std::string peer) {
  is_quic_ = true;
  quic_connection_ = connection;
  peer_ = std::move(peer);
}

void ProtoConnection::StartIdleTimer() {
#if defined(UVNVR_HAVE_QUIC)
  (void)0;
#endif
  if (loop_ == nullptr || idle_ != nullptr || idle_timeout_ms_ == 0)
    return;
  idle_ = new IdleTimer();
  idle_->owner = weak_from_this();
  uv_loop_t* raw = reinterpret_cast<uv_loop_t*>(loop_->get_handle());
  if (uv_timer_init(raw, &idle_->handle) != 0) {
    delete idle_;
    idle_ = nullptr;
    return;
  }
  idle_->handle.data = idle_;
  uv_timer_start(
      &idle_->handle,
      [](uv_timer_t* handle) {
        auto* timer = static_cast<IdleTimer*>(handle->data);
        if (auto connection = timer->owner.lock())
          connection->OnIdleTimeout();
      },
      idle_timeout_ms_, idle_timeout_ms_);
}

void ProtoConnection::ResetIdleTimer() {
  if (idle_ != nullptr)
    uv_timer_again(&idle_->handle);
}

void ProtoConnection::StopIdleTimer() {
  if (idle_ == nullptr)
    return;
  IdleTimer* timer = idle_;
  idle_ = nullptr;
  uv_timer_stop(&timer->handle);
  uv_close(reinterpret_cast<uv_handle_t*>(&timer->handle), [](uv_handle_t* handle) {
    delete static_cast<IdleTimer*>(handle->data);
  });
}

void ProtoConnection::OnIdleTimeout() {
  Close();
}

void ProtoConnection::OnTcpRead(const char* data, size_t len, bool closed, int error) {
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] tcp read id=%llu len=%zu closed=%d error=%d\n",
                 static_cast<unsigned long long>(id_), len, closed ? 1 : 0, error);
  if (closed) {
    (void)error;
    MarkClosed();
    Close();
    return;
  }
  ResetIdleTimer();
  AppendInput(data, len);
}

void ProtoConnection::OnQuicRead(int64_t stream_id, const char* data, size_t len, bool closed,
                                 int error) {
  if (quic_control_stream_ < 0)
    quic_control_stream_ = stream_id;
  if (stream_id != quic_control_stream_) {
    // v1 uses a single control stream; ignore extra streams instead of
    // treating them as protocol errors (see docs/private-protocol.md §2.4).
    return;
  }
  if (closed) {
    (void)error;
    MarkClosed();
    Close();
    return;
  }
  AppendInput(data, len);
}

void ProtoConnection::OnQuicStreamOpen(int64_t stream_id) {
  if (quic_control_stream_ < 0) {
    quic_control_stream_ = stream_id;
    FlushQuicQueue();
  }
}

void ProtoConnection::OnQuicClose() {
  MarkClosed();
}

void ProtoConnection::AppendInput(const char* data, size_t len) {
  if (!alive_)
    return;
  const size_t limit = kMaxPayload + kHeaderSize + 1024u * 1024u;
  if (input_.size() + len > limit) {
    Close();
    return;
  }
  input_.append(data, len);
  ParseInput();
}

void ProtoConnection::ParseInput() {
  size_t offset = 0;
  while (alive_ && input_.size() - offset >= kHeaderSize) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(input_.data() + offset);
    Header header;
    std::string error;
    if (!DecodeHeader(base, input_.size() - offset, &header, &error)) {
      Close();
      return;
    }
    const size_t frame_size = kHeaderSize + static_cast<size_t>(header.payload_len);
    if (input_.size() - offset < frame_size)
      break;
    if (!VerifyFrame(base, frame_size, header)) {
      Close();
      return;
    }
    std::string payload(reinterpret_cast<const char*>(base + kHeaderSize), header.payload_len);
    offset += frame_size;
    Deliver(header, std::move(payload));
  }
  if (offset != 0)
    input_.erase(0, offset);
}

void ProtoConnection::Deliver(const Header& header, std::string payload) {
  if (frame_handler_)
    frame_handler_(shared_from_this(), header, std::move(payload));
}

void ProtoConnection::PumpTcpWrites() {
  if (!alive_ || tcp_ == nullptr || tcp_write_in_flight_ || tcp_outbox_.empty())
    return;
  std::vector<uint8_t> frame = std::move(tcp_outbox_.front());
  tcp_outbox_.erase(tcp_outbox_.begin());
  tcp_outbox_bytes_ -= frame.size();
  tcp_write_in_flight_ = true;
  auto self = weak_from_this();
  // The buffer must outlive the async write, and its data/size must be read
  // *before* the buffer is handed to the completion callback: argument
  // evaluation order is unspecified, and moving `frame` into the lambda while
  // also calling frame.data() in the same expression can submit a 0-byte write.
  auto buffer = std::make_shared<std::vector<uint8_t>>(std::move(frame));
  const char* data = reinterpret_cast<const char*>(buffer->data());
  const size_t size = buffer->size();
  const int rc = tcp_->write(data, size,
                             [self, buffer](int status) {
                               auto connection = self.lock();
                               if (!connection)
                                 return;
                               if (TraceEnabled())
                                 std::fprintf(stderr, "[proto] tcp write done id=%llu status=%d\n",
                                              static_cast<unsigned long long>(connection->id()),
                                              status);
                               connection->tcp_write_in_flight_ = false;
                               if (status < 0) {
                                 connection->Close();
                                 return;
                               }
                               connection->PumpTcpWrites();
                             });
  if (rc < 0) {
    tcp_write_in_flight_ = false;
    if (rc != UV_EALREADY)
      Close();
  }
}

void ProtoConnection::FlushQuicQueue() {
  if (!alive_ || quic_connection_ == nullptr || quic_control_stream_ < 0)
    return;
  auto queue = std::move(quic_pending_);
  quic_pending_.clear();
  for (const auto& frame : queue) {
    if (quic_connection_->write_stream(quic_control_stream_,
                                       reinterpret_cast<const char*>(frame.data()), frame.size(),
                                       false) < 0) {
      Close();
      return;
    }
  }
}

bool ProtoConnection::SendFrame(uint16_t opcode, uint16_t flags, uint64_t session_id,
                                uint32_t request_id, const void* payload, size_t payload_len) {
  if (!alive_ || payload_len > kMaxPayload)
    return false;
  Header header;
  header.opcode = opcode;
  header.flags = flags;
  header.session_id = session_id;
  header.request_id = request_id;
  header.payload_len = static_cast<uint32_t>(payload_len);
  std::vector<uint8_t> frame = EncodeFrame(header, payload, payload_len);
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] send id=%llu %s op=0x%04x flags=0x%04x bytes=%zu quic=%d\n",
                 static_cast<unsigned long long>(id_), OpcodeName(opcode), opcode, flags,
                 frame.size(), is_quic_ ? 1 : 0);

  if (is_quic_) {
    if (quic_connection_ == nullptr)
      return false;
    if (quic_control_stream_ < 0) {
      quic_pending_.push_back(std::move(frame));
      return true;
    }
    if (quic_connection_->write_stream(quic_control_stream_,
                                       reinterpret_cast<const char*>(frame.data()), frame.size(),
                                       false) < 0) {
      Close();
      return false;
    }
    return true;
  }

  if (tcp_ == nullptr)
    return false;
  if (tcp_outbox_bytes_ + frame.size() > max_outbox_bytes_)
    return false;
  tcp_outbox_bytes_ += frame.size();
  tcp_outbox_.push_back(std::move(frame));
  PumpTcpWrites();
  return true;
}

bool ProtoConnection::SendJson(uint16_t opcode, uint16_t flags, uint64_t session_id,
                               uint32_t request_id, const json& payload) {
  const std::string text = payload.is_discarded() ? std::string("{}") : payload.dump();
  return SendFrame(opcode, flags, session_id, request_id, text.data(), text.size());
}

bool ProtoConnection::SendStreamFrame(uint16_t opcode, uint32_t stream_id, const void* payload,
                                      size_t payload_len, uint8_t codec, uint8_t frame_flags,
                                      uint64_t pts_ms) {
  if (payload_len > kMaxPayload - 16)
    return false;
  std::vector<uint8_t> buffer(16 + payload_len);
  for (int i = 0; i < 4; ++i)
    buffer[i] = static_cast<uint8_t>((stream_id >> (8 * i)) & 0xff);
  buffer[4] = codec;
  buffer[5] = frame_flags;
  buffer[6] = buffer[7] = 0;
  for (int i = 0; i < 8; ++i)
    buffer[8 + i] = static_cast<uint8_t>((pts_ms >> (8 * i)) & 0xff);
  if (payload_len != 0 && payload != nullptr)
    std::memcpy(buffer.data() + 16, payload, payload_len);
  return SendFrame(opcode, kFlagStream, 0, 0, buffer.data(), buffer.size());
}

bool ProtoConnection::SendError(uint16_t request_opcode, uint64_t session_id,
                                uint32_t request_id, int status, const std::string& code,
                                const std::string& message) {
  const json body = {{"status", status}, {"code", code}, {"message", message},
                     {"details", json::object()}};
  return SendJson(request_opcode, kFlagResponse | kFlagError, session_id, request_id, body);
}

void ProtoConnection::Close() {
  if (!alive_ && closing_)
    return;
  closing_ = true;
  MarkClosed();
  if (is_quic_) {
    if (quic_connection_ != nullptr)
      quic_connection_->close();
  } else if (tcp_ != nullptr) {
    tcp_->close();
  }
}

struct ProtoServer::Impl {
  std::unique_ptr<uvcpp::uvcpp_tcp_server> tcp_server;
#if defined(UVNVR_HAVE_QUIC)
  std::unique_ptr<uvcpp::uvcpp_ssl_context> quic_context;
  std::unique_ptr<uvcpp::uvcpp_quic_server> quic_server;
#endif
};

ProtoServer::ProtoServer(uvcpp::uvcpp_loop* loop, ProtoAuthHandler auth_handler,
                         ProtoRequestHandler request_handler)
    : loop_(loop), auth_handler_(std::move(auth_handler)),
      request_handler_(std::move(request_handler)), impl_(std::make_unique<Impl>()) {}

ProtoServer::~ProtoServer() {
  Stop();
}

int ProtoServer::Start(const Options& options, std::string* error) {
  options_ = options;
  if (options_.port == 0) {
    if (error)
      *error = "privatePort must be between 1 and 65535";
    return -1;
  }

  if (options_.tcp) {
    impl_->tcp_server = std::make_unique<uvcpp::uvcpp_tcp_server>(loop_);
    int rc = impl_->tcp_server->bind(options_.bind_ip.empty() ? nullptr : options_.bind_ip.c_str(),
                                     options_.port);
    if (rc != 0) {
      if (error)
        *error = "cannot bind TCP port " + std::to_string(options_.port) + ": libuv error " +
                 std::to_string(rc);
      return rc;
    }
    rc = impl_->tcp_server->listen([this](uvcpp::uvcpp_tcp_client* client) { OnTcpClient(client); });
    if (rc != 0) {
      if (error)
        *error = "cannot listen on TCP port " + std::to_string(options_.port) + ": libuv error " +
                 std::to_string(rc);
      return rc;
    }
    tcp_enabled_ = true;
    if (TraceEnabled())
      std::fprintf(stderr, "[proto] tcp listener ready on %s:%u\n",
                   options_.bind_ip.c_str(), options_.port);
  }

  if (options_.quic) {
#if defined(UVNVR_HAVE_QUIC)
    static std::once_flag crypto_once;
    std::call_once(crypto_once, [] { uvcpp::quic_crypto_backend_init(); });

    impl_->quic_context =
        std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::SERVER,
                                                   uvcpp::tls_version::TLS_1_3);
    bool tls_ok = false;
    if (!options_.tls_cert.empty() && !options_.tls_key.empty()) {
      tls_ok = impl_->quic_context->load_certificate_file(options_.tls_cert) &&
               impl_->quic_context->load_private_key_file(options_.tls_key) &&
               impl_->quic_context->check_private_key();
    } else {
      tls_ok = impl_->quic_context->generate_self_signed("uvnvr.local");
    }
    if (!tls_ok) {
      if (error)
        *error = "QUIC listener has no usable TLS certificate; falling back to TCP only";
      if (tcp_enabled_)
        return 0;
      return -1;
    }

    impl_->quic_server = std::make_unique<uvcpp::uvcpp_quic_server>(loop_);
    impl_->quic_server->set_ssl_context(impl_->quic_context.get());
    impl_->quic_server->set_alpn_select_protos({std::string(kAlpn)});
    impl_->quic_server->set_idle_timeout(options_.idle_timeout_ms);
    int rc = impl_->quic_server->bind(
        options_.bind_ip.empty() ? nullptr : options_.bind_ip.c_str(), options_.port);
    if (rc != 0) {
      if (error)
        *error = "cannot bind QUIC port " + std::to_string(options_.port) + ": error " +
                 std::to_string(rc);
      impl_->quic_server.reset();
      impl_->quic_context.reset();
      if (tcp_enabled_)
        return 0;
      return rc;
    }
    rc = impl_->quic_server->listen(
        [this](uvcpp::uvcpp_quic_connection* connection) { OnQuicConnection(connection); });
    if (rc != 0) {
      if (error)
        *error = "cannot listen on QUIC port " + std::to_string(options_.port) + ": error " +
                 std::to_string(rc);
      impl_->quic_server.reset();
      impl_->quic_context.reset();
      if (tcp_enabled_)
        return 0;
      return rc;
    }
    quic_enabled_ = true;
#else
    if (error)
      *error = "QUIC was requested but this libuvcpp build has no QUIC support";
#endif
  }

  if (!tcp_enabled_ && !quic_enabled_) {
    if (error)
      *error = "both private transports are disabled";
    return -1;
  }
  return 0;
}

void ProtoServer::Stop() {
  sessions_.clear();
  connections_.clear();
#if defined(UVNVR_HAVE_QUIC)
  impl_->quic_server.reset();
  impl_->quic_context.reset();
#endif
  impl_->tcp_server.reset();
  tcp_enabled_ = false;
  quic_enabled_ = false;
}

void ProtoServer::OnTcpClient(uvcpp::uvcpp_tcp_client* client) {
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] tcp accept callback client=%p\n", static_cast<void*>(client));
  if (client == nullptr)
    return;
  std::string ip;
  int port = 0;
  client->getPeerAddrs(ip, port);
  const uint64_t id = next_connection_id_++;
  auto connection = ProtoConnection::Ptr(new ProtoConnection(id, false));
  connection->SetFrameHandler(
      [this](ProtoConnection::Ptr source, const Header& header, std::string payload) {
        OnFrame(source, header, std::move(payload));
      });
  connections_[id] = connection;

  std::weak_ptr<ProtoConnection> weak = connection;
  client->set_on_close([this, weak, id] {
    auto closed = weak.lock();
    if (closed) {
      closed->MarkClosed();
      closed->StopIdleTimer();
    }
    OnConnectionClosed(id);
  });
  connection->AttachTcp(loop_, client, ip.empty() ? "tcp" : ip, options_.idle_timeout_ms);
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] tcp accepted id=%llu peer=%s:%d\n",
                 static_cast<unsigned long long>(id), ip.c_str(), port);
}

#if defined(UVNVR_HAVE_QUIC)
void ProtoServer::OnQuicConnection(uvcpp::uvcpp_quic_connection* quic_connection) {
  if (quic_connection == nullptr)
    return;
  const uint64_t id = next_connection_id_++;
  auto connection = ProtoConnection::Ptr(new ProtoConnection(id, true));
  connection->SetFrameHandler(
      [this](ProtoConnection::Ptr source, const Header& header, std::string payload) {
        OnFrame(source, header, std::move(payload));
      });
  connections_[id] = connection;
  connection->AttachQuic(quic_connection, "quic");

  std::weak_ptr<ProtoConnection> weak = connection;
  uvcpp::uvcpp_quic_connection::callbacks callbacks;
  callbacks.on_stream_open = [weak](uvcpp::uvcpp_quic_connection&, int64_t stream_id) {
    if (auto source = weak.lock())
      source->OnQuicStreamOpen(stream_id);
  };
  callbacks.on_read = [weak](uvcpp::uvcpp_quic_connection&, int64_t stream_id,
                             const uvcpp::net_read_result& result) {
    auto source = weak.lock();
    if (!source)
      return;
    if (result.event == uvcpp::net_read_event::DATA) {
      source->OnQuicRead(stream_id, result.data, result.size, false, 0);
    } else {
      const int error = result.event == uvcpp::net_read_event::READ_ERROR ? result.error : 0;
      source->OnQuicRead(stream_id, nullptr, 0, true, error);
    }
  };
  callbacks.on_close = [this, weak, id](uvcpp::uvcpp_quic_connection&, int) {
    if (auto closed = weak.lock())
      closed->MarkClosed();
    OnConnectionClosed(id);
  };
  quic_connection->set_callbacks(callbacks);
}
#endif

void ProtoServer::OnConnectionClosed(uint64_t connection_id) {
  connections_.erase(connection_id);
}

SessionView* ProtoServer::FindSession(uint64_t session_id) {
  if (session_id == 0)
    return nullptr;
  auto it = sessions_.find(session_id);
  if (it == sessions_.end())
    return nullptr;
  if (it->second.expires <= std::chrono::steady_clock::now()) {
    sessions_.erase(it);
    return nullptr;
  }
  return &it->second.view;
}

void ProtoServer::OnFrame(const ProtoConnection::Ptr& connection, const Header& header,
                          std::string payload) {
  if (TraceEnabled())
    std::fprintf(stderr, "[proto] frame id=%llu %s op=0x%04x bytes=%zu\n",
                 static_cast<unsigned long long>(connection ? connection->id() : 0),
                 OpcodeName(header.opcode), header.opcode, payload.size());
  if (connection == nullptr || !connection->alive())
    return;

  if (header.opcode == kOpPing) {
    connection->SendFrame(kOpPong, kFlagResponse, header.session_id, header.request_id, nullptr, 0);
    return;
  }

  if (header.opcode == kOpHello) {
    HandleHello(connection, header, ParseObject(payload));
    return;
  }

  if (header.opcode == kOpAuth) {
    HandleAuth(connection, header, ParseObject(payload));
    return;
  }

  // SYSTEM_INFO mirrors the public subset of /api/v1/system/info and is
  // available before AUTH so a client can decide whether to log in.
  if (header.opcode == kOpSystemInfo) {
    json transports = json::array();
    if (tcp_enabled_)
      transports.push_back("tcp");
    if (quic_enabled_)
      transports.push_back("quic");
    const json data = {
        {"version", UVNVR_VERSION},
        {"commit", UVNVR_GIT_SHA},
        {"protocol", kVersion},
        {"transports", transports},
    };
    connection->SendJson(kOpSystemInfo, kFlagResponse, header.session_id, header.request_id, data);
    return;
  }

  SessionView* session = FindSession(header.session_id);
  if (session == nullptr) {
    SendError(connection, header, 401, "session_expired",
              "the communication ID is missing or has expired; authenticate again");
    return;
  }

  if (header.opcode == kOpLogout) {
    sessions_.erase(header.session_id);
    connection->SendJson(kOpLogout, kFlagResponse, header.session_id, header.request_id,
                         json{{"ok", true}});
    return;
  }

  if (header.opcode == kOpSessionRenew) {
    auto it = sessions_.find(header.session_id);
    it->second.expires = std::chrono::steady_clock::now() +
                         std::chrono::seconds(options_.session_ttl_seconds);
    connection->SendJson(kOpSessionRenew, kFlagResponse, header.session_id, header.request_id,
                         json{{"expiresIn", options_.session_ttl_seconds}});
    return;
  }

  auto request = std::make_shared<ProtoRequest>();
  request->header = header;
  request->payload = std::move(payload);
  request->peer = connection->peer();
  request->connection_id = connection->id();
  request->connection = connection;
  request->has_session = true;
  request->session = *session;
  if (!request_handler_) {
    SendError(connection, header, 501, "unsupported_opcode",
              "no handler is registered for this opcode");
    return;
  }
  std::weak_ptr<ProtoConnection> weak = connection;
  const Header request_header = header;
  request_handler_(std::move(request),
                   [this, weak, request_header](ProtoReply reply) {
                     auto target = weak.lock();
                     if (!target || !target->alive())
                       return;
                     Reply(target, request_header, reply);
                   });
}

void ProtoServer::HandleHello(const ProtoConnection::Ptr& connection, const Header& header,
                              const json& payload) {
  (void)payload;
  json transports = json::array();
  if (tcp_enabled_)
    transports.push_back("tcp");
  if (quic_enabled_)
    transports.push_back("quic");
  const json reply = {
      {"protocol", kVersion},
      {"serverVersion", UVNVR_VERSION},
      {"maxFrame", kMaxPayload},
      {"transports", transports},
      {"alpn", kAlpn},
      {"compression", "none"},
      {"encryption", {{"quic", "tls1.3"}, {"tcp", "none"}}},
      {"heartbeatMs", options_.heartbeat_ms},
      {"sessionTtl", options_.session_ttl_seconds},
  };
  connection->SendJson(kOpHelloAck, kFlagResponse, header.session_id, header.request_id, reply);
}

void ProtoServer::HandleAuth(const ProtoConnection::Ptr& connection, const Header& header,
                             const json& payload) {
  const std::string username = JsonString(payload, "username");
  const std::string password = JsonString(payload, "password");
  if (username.empty() || password.empty()) {
    SendError(connection, header, 400, "invalid_credentials",
              "username and password are required");
    return;
  }
  if (!auth_handler_) {
    SendError(connection, header, 500, "auth_unavailable", "authentication is not available");
    return;
  }
  const AuthResult result = auth_handler_(username, password, connection->peer());
  if (!result.ok) {
    SendError(connection, header, result.status, result.code, result.message);
    return;
  }

  const uint64_t session_id = RandomSessionId();
  Session session;
  session.view.session_id = session_id;
  session.view.user_id = result.user_id;
  session.view.username = result.username.empty() ? username : result.username;
  session.view.role = result.role;
  session.view.permissions = result.permissions;
  session.expires = std::chrono::steady_clock::now() +
                    std::chrono::seconds(options_.session_ttl_seconds);
  sessions_[session_id] = session;

  json permissions = json::array();
  for (const auto& permission : result.permissions)
    permissions.push_back(permission);
  const json reply = {
      {"sessionId", session_id},
      {"expiresIn", options_.session_ttl_seconds},
      {"permissions", permissions},
      {"user", {{"id", result.user_id}, {"username", session.view.username},
                {"role", result.role}}},
  };
  connection->SendJson(kOpAuthAck, kFlagResponse, session_id, header.request_id, reply);
}

void ProtoServer::SendReply(const ProtoConnection::Ptr& connection, const Header& request,
                            int status, const std::string& code, const std::string& message,
                            const json& data) {
  if (status >= 200 && status < 300) {
    const json body = data.is_discarded() || data.is_null() ? json::object() : data;
    connection->SendJson(request.opcode, kFlagResponse, request.session_id, request.request_id,
                         body);
  } else {
    SendError(connection, request, status, code, message);
  }
}

void ProtoServer::SendError(const ProtoConnection::Ptr& connection, const Header& request,
                            int status, const std::string& code, const std::string& message) {
  connection->SendError(request.opcode, request.session_id, request.request_id, status, code,
                        message);
}

void ProtoServer::Reply(const ProtoConnection::Ptr& connection, const Header& request,
                        const ProtoReply& reply) {
  if (reply.ok()) {
    const json body = reply.data.is_discarded() || reply.data.is_null() ? json::object()
                                                                       : reply.data;
    connection->SendJson(reply.opcode != 0 ? reply.opcode : request.opcode,
                         static_cast<uint16_t>(kFlagResponse | reply.extra_flags),
                         request.session_id, request.request_id, body);
    return;
  }
  connection->SendError(reply.opcode != 0 ? reply.opcode : request.opcode, request.session_id,
                        request.request_id, reply.status, reply.code, reply.message);
}

} // namespace uvnvr::proto
