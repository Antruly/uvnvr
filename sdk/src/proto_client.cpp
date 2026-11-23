#include "sdk_internal.h"

#include <algorithm>
#include <cstring>

namespace uvnvr::sdk {
namespace {

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadLe64(const uint8_t* p) {
  return static_cast<uint64_t>(ReadLe32(p)) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32);
}

int MapStatus(int status) {
  switch (status) {
  case 400:
    return UVNVR_ERR_INVALID_ARGUMENT;
  case 401:
    return UVNVR_ERR_AUTH;
  case 403:
    return UVNVR_ERR_FORBIDDEN;
  case 404:
    return UVNVR_ERR_NOT_FOUND;
  case 409:
    return UVNVR_ERR_CONFLICT;
  case 429:
    return UVNVR_ERR_LOCKED;
  case 501:
    return UVNVR_ERR_UNSUPPORTED;
  case 503:
    return UVNVR_ERR_UNAVAILABLE;
  default:
    return status >= 500 ? UVNVR_ERR_SERVER : UVNVR_ERR_PROTOCOL;
  }
}

json ParseJson(const std::string& text) {
  if (text.empty())
    return json::object();
  json value = json::parse(text, nullptr, false);
  return value.is_discarded() ? json::object() : value;
}

} // namespace

Client::Client(const Options& options) : options_(options) {
  async_.init([this](uvcpp::uvcpp_async*) { DrainTasks(); }, &loop_);
  connect_timer_.init(&loop_);
  heartbeat_timer_.init(&loop_);
  renew_timer_.init(&loop_);
  thread_ = std::thread([this] { loop_.run(UV_RUN_DEFAULT); });
}

Client::~Client() {
  auto waiter = std::make_shared<Pending>();
  Post([this, waiter] {
    ShutdownOnLoop();
    CompletePending(waiter, UVNVR_OK, "", "", json::object());
  });
  {
    std::unique_lock<std::mutex> lock(waiter->mutex);
    waiter->cv.wait_for(lock, std::chrono::seconds(2), [&] { return waiter->done; });
  }
  running_ = false;
  // uvcpp_loop::stop() is only safe on the loop thread; ShutdownOnLoop calls
  // it there after draining every transport and timer.
  if (thread_.joinable())
    thread_.join();
}

void Client::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    if (!running_)
      return;
    tasks_.push_back(std::move(task));
  }
  async_.send();
}

void Client::DrainTasks() {
  std::deque<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    local.swap(tasks_);
  }
  for (auto& task : local) {
    if (task)
      task();
  }
}

void Client::ShutdownOnLoop() {
  StopConnectTimer();
  heartbeat_timer_.stop();
  renew_timer_.stop();
  CloseTransport();
  connected_ = false;
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
  FailAllPending(UVNVR_ERR_CLOSED, "the SDK is shutting down");
  FailDownloads(UVNVR_ERR_CLOSED, "the SDK is shutting down");
  if (connect_waiter_) {
    auto waiter = connect_waiter_;
    connect_waiter_.reset();
    CompletePending(waiter, UVNVR_ERR_CLOSED, "the SDK is shutting down", "", json::object());
  }
  live_.clear();
  if (running_)
    loop_.stop();
}

void Client::CompletePending(const std::shared_ptr<Pending>& pending, int code,
                             const std::string& message, const std::string& server_code,
                             const json& data) {
  if (!pending)
    return;
  std::lock_guard<std::mutex> lock(pending->mutex);
  pending->done = true;
  pending->code = code;
  pending->message = message;
  pending->server_code = server_code;
  pending->data = data;
  pending->cv.notify_all();
}

void Client::FailAllPending(int code, const std::string& message) {
  auto pending = std::move(pending_);
  pending_.clear();
  for (auto& [id, entry] : pending)
    CompletePending(entry, code, message, "", json::object());
}

void Client::FailDownload(const std::shared_ptr<DownloadState>& state, int code,
                          const std::string& message) {
  if (!state)
    return;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->done)
    return;
  state->done = true;
  state->code = code;
  state->message = message;
  state->cv.notify_all();
}

void Client::FailDownloads(int code, const std::string& message) {
  std::vector<std::shared_ptr<DownloadState>> states;
  {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    for (auto& [id, state] : downloads_)
      states.push_back(state);
  }
  for (auto& state : states)
    FailDownload(state, code, message);
}

std::shared_ptr<Client::DownloadState> Client::FindDownload(uint32_t download_id) {
  std::lock_guard<std::mutex> lock(downloads_mutex_);
  auto it = downloads_.find(download_id);
  return it == downloads_.end() ? nullptr : it->second;
}

void Client::CloseTransport() {
  // Destroy the wrapper rather than calling close() and then destroying it:
  // the destructor initiates uv_close and hands the wrapper to the loop's
  // close completion, whereas close()-then-destruct leaves the wrapper
  // unreclaimed because the close callback slot is already taken.
  tcp_.reset();
#if defined(UVNVR_SDK_HAVE_QUIC)
  if (quic_) {
    std::unique_ptr<uvcpp::uvcpp_quic_client> client = std::move(quic_);
    client->close();
  }
  quic_context_.reset();
  quic_pending_.clear();
  quic_stream_ = -1;
#endif
  tcp_outbox_.clear();
  tcp_outbox_bytes_ = 0;
  tcp_write_in_flight_ = false;
  input_.clear();
}

int Client::Connect(const std::string& host, uint16_t port, int timeout_ms,
                    std::string* error) {
  if (host.empty()) {
    if (error)
      *error = "host must not be empty";
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  if (timeout_ms <= 0)
    timeout_ms = static_cast<int>(options_.connect_timeout_ms);
  auto waiter = std::make_shared<Pending>();
  Post([this, host, port, timeout_ms, waiter] {
    BeginConnect(host, port, timeout_ms, waiter);
  });
  std::unique_lock<std::mutex> lock(waiter->mutex);
  if (!waiter->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms + 2000),
                           [&] { return waiter->done; })) {
    Post([this, waiter] {
      CompletePending(waiter, UVNVR_ERR_TIMEOUT, "connection attempt timed out", "",
                      json::object());
    });
    if (error)
      *error = "connection attempt timed out";
    return UVNVR_ERR_TIMEOUT;
  }
  if (waiter->code != UVNVR_OK) {
    if (error)
      *error = waiter->message;
    return waiter->code;
  }
  return UVNVR_OK;
}

void Client::BeginConnect(const std::string& host, uint16_t port, int timeout_ms,
                          std::shared_ptr<Pending> waiter) {
  CloseTransport();
  connected_ = false;
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
  connecting_ = true;
  connect_waiter_ = std::move(waiter);
  connect_started_ = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    host_ = host;
    port_ = port;
  }
  const uint64_t epoch = generation_.fetch_add(1) + 1;
  current_epoch_ = epoch;

  bool started = false;
#if defined(UVNVR_SDK_HAVE_QUIC)
  if (options_.prefer_quic && options_.quic_probe_timeout_ms > 0)
    started = StartQuic(epoch);
#endif
  if (!started) {
    StartTcp(epoch);
    StartConnectTimer(static_cast<uint32_t>(std::max(timeout_ms, 1)), epoch, false);
  } else {
    StartConnectTimer(options_.quic_probe_timeout_ms, epoch, true);
  }
}

void Client::StartConnectTimer(uint32_t timeout_ms, uint64_t epoch, bool quic_phase) {
  timer_epoch_ = epoch;
  quic_attempt_active_ = quic_phase;
  connect_timer_.stop();
  connect_timer_.start([this](uvcpp::uvcpp_timer*) { OnConnectTimeout(); }, timeout_ms, 0);
}

void Client::StopConnectTimer() {
  connect_timer_.stop();
}

void Client::OnConnectTimeout() {
  if (timer_epoch_ != current_epoch_ || !connecting_)
    return;
  if (quic_attempt_active_ || transport_.load() == UVNVR_TRANSPORT_QUIC) {
    BeginTcpFallback(current_epoch_);
    return;
  }
  FailConnect(current_epoch_, UVNVR_ERR_TIMEOUT, "connection or handshake timed out");
}

void Client::FailConnect(uint64_t epoch, int code, const std::string& message) {
  if (epoch != current_epoch_)
    return;
  connecting_ = false;
  quic_attempt_active_ = false;
  StopConnectTimer();
  CloseTransport();
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
  if (connect_waiter_) {
    auto waiter = connect_waiter_;
    connect_waiter_.reset();
    CompletePending(waiter, code, message, "", json::object());
  }
}

#if defined(UVNVR_SDK_HAVE_QUIC)
bool Client::StartQuic(uint64_t epoch) {
  quic_context_ = std::make_unique<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::CLIENT,
                                                             uvcpp::tls_version::TLS_1_3);
  if (options_.verify_peer) {
    if (!options_.ca_file.empty())
      quic_context_->load_ca_file(options_.ca_file);
    quic_context_->set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
  } else {
    quic_context_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
  }
  quic_ = std::make_unique<uvcpp::uvcpp_quic_client>(&loop_);
  quic_->set_ssl_context(quic_context_.get());
  quic_->set_alpn_protos({std::string(proto::kAlpn)});
  quic_->set_idle_timeout(std::max<uint32_t>(options_.heartbeat_interval_ms * 3, 5000));

  std::string host;
  uint16_t port = 0;
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    host = host_;
    port = port_;
  }
  const int rc = quic_->connect(host.c_str(), static_cast<int>(port),
                                [this, epoch](int status) { OnQuicHandshake(epoch, status); });
  if (rc != 0) {
    quic_.reset();
    quic_context_.reset();
    return false;
  }
  auto* connection = quic_->connection();
  if (connection == nullptr) {
    quic_.reset();
    quic_context_.reset();
    return false;
  }
  uvcpp::uvcpp_quic_connection::callbacks callbacks;
  callbacks.on_read = [this, epoch](uvcpp::uvcpp_quic_connection&, int64_t stream_id,
                                    const uvcpp::net_read_result& result) {
    if (epoch != current_epoch_ || stream_id != quic_stream_)
      return;
    if (result.event == uvcpp::net_read_event::DATA)
      OnBytes(result.data, result.size);
    else
      OnTransportClosed(epoch);
  };
  callbacks.on_close = [this, epoch](uvcpp::uvcpp_quic_connection&, int) {
    OnTransportClosed(epoch);
  };
  connection->set_callbacks(callbacks);
  return true;
}
#else
bool Client::StartQuic(uint64_t) {
  return false;
}
#endif

void Client::OnQuicHandshake(uint64_t epoch, int status) {
  if (epoch != current_epoch_)
    return;
  if (status != 0) {
    BeginTcpFallback(epoch);
    return;
  }
#if defined(UVNVR_SDK_HAVE_QUIC)
  auto* connection = quic_ ? quic_->connection() : nullptr;
  if (connection == nullptr) {
    BeginTcpFallback(epoch);
    return;
  }
  quic_stream_ = connection->open_stream(true);
  if (quic_stream_ < 0) {
    BeginTcpFallback(epoch);
    return;
  }
  quic_attempt_active_ = false;
  transport_ = UVNVR_TRANSPORT_QUIC;
  BeginHandshake(epoch);
#else
  BeginTcpFallback(epoch);
#endif
}

void Client::BeginTcpFallback(uint64_t epoch) {
  if (epoch != current_epoch_)
    return;
  if (options_.disable_tcp) {
    FailConnect(epoch, UVNVR_ERR_NETWORK, "QUIC failed and TCP fallback is disabled");
    return;
  }
  const uint64_t next = generation_.fetch_add(1) + 1;
  current_epoch_ = next;
  quic_attempt_active_ = false;
  connected_ = false;
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
#if defined(UVNVR_SDK_HAVE_QUIC)
  struct QuicCleanup {
    std::unique_ptr<uvcpp::uvcpp_quic_client> quic;
    std::unique_ptr<uvcpp::uvcpp_ssl_context> context;
  };
  auto cleanup = std::make_shared<QuicCleanup>();
  cleanup->quic = std::move(quic_);
  cleanup->context = std::move(quic_context_);
  quic_stream_ = -1;
  quic_pending_.clear();
  // Do not destroy the QUIC endpoint from inside its own callback; hand it to
  // the next loop turn, then start TCP.
  Post([this, next, cleanup] {
    if (cleanup->quic)
      cleanup->quic->close();
    cleanup->quic.reset();
    cleanup->context.reset();
    if (!connecting_ || next != current_epoch_)
      return;
    StartTcp(next);
    StartConnectTimer(options_.connect_timeout_ms, next, false);
  });
#else
  StartTcp(next);
  StartConnectTimer(options_.connect_timeout_ms, next, false);
#endif
}

void Client::StartTcp(uint64_t epoch) {
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
  tcp_ = std::make_unique<uvcpp::uvcpp_tcp_client>(&loop_);
  std::string host;
  uint16_t port = 0;
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    host = host_;
    port = port_;
  }
  const int rc = tcp_->connect(host.c_str(), static_cast<int>(port),
                               [this, epoch](int status) { OnTcpConnected(epoch, status); });
  if (rc != 0) {
    auto client = std::move(tcp_);
    if (client)
      client->close();
    FailConnect(epoch, UVNVR_ERR_NETWORK, "TCP connect could not be started");
  }
}

void Client::OnTcpConnected(uint64_t epoch, int status) {
  if (epoch != current_epoch_)
    return;
  if (status < 0) {
    FailConnect(epoch, UVNVR_ERR_NETWORK, "TCP connection was refused or unreachable");
    return;
  }
  transport_ = UVNVR_TRANSPORT_TCP;
  const int rc = tcp_->read_start_events(
      [this, epoch](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& result) {
        if (epoch != current_epoch_)
          return;
        if (result.event == uvcpp::net_read_event::DATA)
          OnBytes(result.data, result.size);
        else
          OnTransportClosed(epoch);
      });
  if (rc != 0) {
    FailConnect(epoch, UVNVR_ERR_NETWORK, "could not start reading from the TCP connection");
    return;
  }
  BeginHandshake(epoch);
}

void Client::BeginHandshake(uint64_t epoch) {
  if (epoch != current_epoch_)
    return;
  hello_request_id_ = next_request_id_.fetch_add(1);
  hello_pending_ = true;
  StartConnectTimer(options_.connect_timeout_ms, epoch, false);
  const json hello = {{"protocol", static_cast<int>(proto::kVersion)},
                      {"client", "uvnvr-sdk/" UVNVR_SDK_VERSION},
                      {"transports", json::array({"quic", "tcp"})},
                      {"features", json::array({"live", "download", "days"})},
                      {"resumeSessionId", session_id_.load()}};
  SendJsonFrame(proto::kOpHello, 0, hello_request_id_, hello);
}

void Client::OnTransportClosed(uint64_t epoch) {
  if (epoch != current_epoch_)
    return;
  connected_ = false;
  connecting_ = false;
  quic_attempt_active_ = false;
  transport_ = UVNVR_TRANSPORT_UNKNOWN;
  heartbeat_timer_.stop();
  renew_timer_.stop();
  StopConnectTimer();
  FailAllPending(UVNVR_ERR_NETWORK, "the connection was lost");
  FailDownloads(UVNVR_ERR_NETWORK, "the connection was lost");
  live_.clear();
  if (connect_waiter_) {
    auto waiter = connect_waiter_;
    connect_waiter_.reset();
    CompletePending(waiter, UVNVR_ERR_NETWORK, "the connection was lost", "", json::object());
  }
  if (hello_pending_) {
    hello_pending_ = false;
  }
  // Release transport objects outside the transport's own close callback.
  Post([this] { CloseTransport(); });
}

void Client::StartKeepalive() {
  heartbeat_timer_.stop();
  renew_timer_.stop();
  if (options_.heartbeat_interval_ms > 0) {
    heartbeat_timer_.start(
        [this](uvcpp::uvcpp_timer*) {
          if (!connected_)
            return;
          SendFrame(proto::kOpPing, 0, session_id_.load(), 0, nullptr, 0);
        },
        options_.heartbeat_interval_ms, options_.heartbeat_interval_ms);
  }
  if (options_.session_renew_ms > 0) {
    renew_timer_.start(
        [this](uvcpp::uvcpp_timer*) {
          if (!connected_ || session_id_.load() == 0)
            return;
          SendFrame(proto::kOpSessionRenew, 0, session_id_.load(),
                    next_request_id_.fetch_add(1), nullptr, 0);
        },
        options_.session_renew_ms, options_.session_renew_ms);
  }
}

bool Client::SendJsonFrame(uint16_t opcode, uint64_t session_id, uint32_t request_id,
                           const json& payload) {
  const std::string text = payload.dump();
  return SendFrame(opcode, 0, session_id, request_id, text.data(), text.size());
}

bool Client::SendFrame(uint16_t opcode, uint16_t flags, uint64_t session_id,
                       uint32_t request_id, const void* payload, size_t payload_len) {
  proto::Header header;
  header.opcode = opcode;
  header.flags = flags;
  header.session_id = session_id;
  header.request_id = request_id;
  header.payload_len = static_cast<uint32_t>(payload_len);
  std::vector<uint8_t> frame = proto::EncodeFrame(header, payload, payload_len);

  if (transport_ == UVNVR_TRANSPORT_QUIC) {
#if defined(UVNVR_SDK_HAVE_QUIC)
    if (!quic_ || quic_stream_ < 0)
      return false;
    auto* connection = quic_->connection();
    if (connection == nullptr)
      return false;
    return connection->write_stream(quic_stream_, reinterpret_cast<const char*>(frame.data()),
                                    frame.size(), false) >= 0;
#else
    return false;
#endif
  }
  if (!tcp_)
    return false;
  if (tcp_outbox_bytes_ + frame.size() > 64u * 1024u * 1024u)
    return false;
  tcp_outbox_bytes_ += frame.size();
  tcp_outbox_.push_back(std::move(frame));
  PumpTcpWrites();
  return true;
}

void Client::PumpTcpWrites() {
  if (!tcp_ || tcp_write_in_flight_ || tcp_outbox_.empty())
    return;
  std::vector<uint8_t> frame = std::move(tcp_outbox_.front());
  tcp_outbox_.erase(tcp_outbox_.begin());
  tcp_outbox_bytes_ -= frame.size();
  tcp_write_in_flight_ = true;
  const uint64_t epoch = current_epoch_;
  // Keep the bytes alive across the async write without reading frame.data()
  // in an expression that also moves `frame`: argument evaluation order is
  // unspecified, and the move can win, submitting a 0-byte write.
  auto buffer = std::make_shared<std::vector<uint8_t>>(std::move(frame));
  const int rc = tcp_->write(
      reinterpret_cast<const char*>(buffer->data()), buffer->size(),
      [this, epoch, buffer](int status) {
        if (epoch != current_epoch_)
          return;
        tcp_write_in_flight_ = false;
        if (status < 0) {
          OnTransportClosed(epoch);
          return;
        }
        PumpTcpWrites();
      });
  if (rc < 0) {
    tcp_write_in_flight_ = false;
    if (rc != UV_EALREADY)
      OnTransportClosed(current_epoch_);
  }
}

void Client::OnBytes(const char* data, size_t len) {
  if (input_.size() + len > proto::kMaxPayload + proto::kHeaderSize + 1024u * 1024u)
    return;
  input_.insert(input_.end(), reinterpret_cast<const uint8_t*>(data),
                reinterpret_cast<const uint8_t*>(data) + len);
  ParseInput();
}

void Client::ParseInput() {
  size_t offset = 0;
  while (input_.size() - offset >= proto::kHeaderSize) {
    const uint8_t* base = input_.data() + offset;
    proto::Header header;
    std::string error;
    if (!proto::DecodeHeader(base, input_.size() - offset, &header, &error))
      return;
    const size_t frame_size = proto::kHeaderSize + static_cast<size_t>(header.payload_len);
    if (input_.size() - offset < frame_size)
      break;
    if (!proto::VerifyFrame(base, frame_size, header))
      return;
    std::string payload(reinterpret_cast<const char*>(base + proto::kHeaderSize),
                        header.payload_len);
    offset += frame_size;
    HandleFrame(header, std::move(payload));
  }
  if (offset != 0)
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void Client::HandleFrame(const proto::Header& header, std::string payload) {
  if ((header.flags & proto::kFlagError) != 0) {
    HandleError(header, payload);
    return;
  }
  if ((header.flags & proto::kFlagResponse) != 0) {
    HandleResponse(header, payload);
    return;
  }
  switch (header.opcode) {
  case proto::kOpPing:
    SendFrame(proto::kOpPong, proto::kFlagResponse, header.session_id, header.request_id,
              nullptr, 0);
    return;
  case proto::kOpLiveFrame:
  case proto::kOpDownloadChunk:
  case proto::kOpDownloadDone:
    HandleStreamFrame(header, payload);
    return;
  default:
    return;
  }
}

void Client::HandleResponse(const proto::Header& header, const std::string& payload) {
  if (header.opcode == proto::kOpHelloAck && hello_pending_ &&
      header.request_id == hello_request_id_) {
    hello_pending_ = false;
    connecting_ = false;
    quic_attempt_active_ = false;
    connected_ = true;
    StopConnectTimer();
    StartKeepalive();
    if (connect_waiter_) {
      auto waiter = connect_waiter_;
      connect_waiter_.reset();
      CompletePending(waiter, UVNVR_OK, "", "", json::object());
    }
    return;
  }

  const json data = ParseJson(payload);
  if (header.opcode == proto::kOpDownloadDone) {
    const uint32_t download_id = data.value("downloadId", 0u);
    auto state = FindDownload(download_id);
    if (state) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->received = data.value("sizeBytes", state->received);
      state->done = true;
      state->code = UVNVR_OK;
      state->cv.notify_all();
    }
    return;
  }

  auto it = pending_.find(header.request_id);
  if (it == pending_.end())
    return;
  auto pending = it->second;
  pending_.erase(it);
  CompletePending(pending, UVNVR_OK, "", "", data);
}

void Client::HandleError(const proto::Header& header, const std::string& payload) {
  const json error = ParseJson(payload);
  const int status = error.value("status", 500);
  const int code = MapStatus(status);
  const std::string message = error.value("message", std::string("server error"));
  const std::string server_code = error.value("code", std::string());

  if (hello_pending_ && header.request_id == hello_request_id_) {
    hello_pending_ = false;
    FailConnect(current_epoch_, code, message);
    return;
  }

  auto it = pending_.find(header.request_id);
  if (it != pending_.end()) {
    auto pending = it->second;
    pending_.erase(it);
    CompletePending(pending, code, message, server_code, error);
    return;
  }

  // A download that already returned DOWNLOAD_BEGIN reports mid-stream errors
  // against the original request id.
  std::shared_ptr<DownloadState> failed;
  {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    for (auto& [id, state] : downloads_) {
      if (state->request_id == header.request_id) {
        failed = state;
        break;
      }
    }
  }
  if (failed)
    FailDownload(failed, code, message);
}

void Client::HandleStreamFrame(const proto::Header& header, const std::string& payload) {
  if (payload.size() < 16)
    return;
  const uint8_t* base = reinterpret_cast<const uint8_t*>(payload.data());
  const uint32_t stream_id = ReadLe32(base);

  if (header.opcode == proto::kOpLiveFrame) {
    auto it = live_.find(static_cast<int>(stream_id));
    if (it == live_.end() || !it->second.callback)
      return;
    const int codec = payload[4];
    const uint32_t flags = payload[5] & 0x01;
    const int64_t pts = static_cast<int64_t>(ReadLe64(base + 8));
    it->second.callback(static_cast<int>(stream_id), base + 16, payload.size() - 16, codec, flags,
                        pts);
    return;
  }

  if (header.opcode == proto::kOpDownloadChunk) {
    auto state = FindDownload(stream_id);
    if (!state)
      return;
    const uint64_t offset = ReadLe64(base + 8);
    int64_t received = 0;
    int64_t total = 0;
    ProgressCallback progress;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->done)
        return;
      if (static_cast<uint64_t>(state->received) != offset || !state->file) {
        state->done = true;
        state->code = UVNVR_ERR_PROTOCOL;
        state->message = "download chunk arrived out of order";
        state->cv.notify_all();
        return;
      }
      state->file->write(reinterpret_cast<const char*>(base + 16),
                         static_cast<std::streamsize>(payload.size() - 16));
      if (!*state->file) {
        state->done = true;
        state->code = UVNVR_ERR_PROTOCOL;
        state->message = "could not write the downloaded data";
        state->cv.notify_all();
        return;
      }
      state->received += static_cast<int64_t>(payload.size() - 16);
      received = state->received;
      total = state->total;
      progress = state->progress;
    }
    // Progress callbacks never run under DownloadState::mutex, so they may
    // call live_close or cancel without deadlocking.
    if (progress)
      progress(received, total);
    return;
  }
}

CallResult Client::Request(uint16_t opcode, const json& payload, bool with_session,
                           int timeout_ms, uint32_t* used_request_id) {
  CallResult result;
  auto pending = std::make_shared<Pending>();
  const uint32_t request_id = next_request_id_.fetch_add(1);
  if (used_request_id)
    *used_request_id = request_id;
  const uint64_t session = with_session ? session_id_.load() : 0;
  Post([this, opcode, payload, request_id, pending, session] {
    if (!connected_) {
      CompletePending(pending, UVNVR_ERR_NETWORK, "not connected", "", json::object());
      return;
    }
    pending_[request_id] = pending;
    if (!SendJsonFrame(opcode, session, request_id, payload)) {
      pending_.erase(request_id);
      CompletePending(pending, UVNVR_ERR_NETWORK, "could not send the request", "",
                      json::object());
    }
  });

  std::unique_lock<std::mutex> lock(pending->mutex);
  if (!pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&] { return pending->done; })) {
    Post([this, request_id] { pending_.erase(request_id); });
    result.code = UVNVR_ERR_TIMEOUT;
    result.message = "the request timed out";
    return result;
  }
  result.code = pending->code;
  result.message = pending->message;
  result.server_code = pending->server_code;
  result.data = pending->data;
  return result;
}

CallResult Client::RequestWithAuth(uint16_t opcode, const json& payload, int timeout_ms,
                                   uint32_t* used_request_id) {
  CallResult result = Request(opcode, payload, true, timeout_ms, used_request_id);
  if (result.code != UVNVR_ERR_AUTH)
    return result;
  std::string error;
  if (LoginInternal(&error) != UVNVR_OK)
    return result;
  return Request(opcode, payload, true, timeout_ms, used_request_id);
}

int Client::EnsureReady(std::string* error) {
  if (connected_.load())
    return UVNVR_OK;
  std::string host;
  uint16_t port = 0;
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    host = host_;
    port = port_;
  }
  if (host.empty()) {
    if (error)
      *error = "not connected; call uvnvr_connect first";
    return UVNVR_ERR_NOT_CONNECTED;
  }
  auto waiter = std::make_shared<Pending>();
  Post([this, host, port, waiter] {
    BeginConnect(host, port, static_cast<int>(options_.connect_timeout_ms), waiter);
  });
  std::unique_lock<std::mutex> lock(waiter->mutex);
  if (!waiter->cv.wait_for(lock,
                           std::chrono::milliseconds(options_.connect_timeout_ms + 2000),
                           [&] { return waiter->done; })) {
    if (error)
      *error = "reconnect timed out";
    return UVNVR_ERR_TIMEOUT;
  }
  if (waiter->code != UVNVR_OK) {
    if (error)
      *error = waiter->message;
    return waiter->code;
  }
  return UVNVR_OK;
}

int Client::Login(const std::string& username, const std::string& password,
                  std::string* error) {
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    username_ = username;
    password_ = password;
  }
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;
  return LoginInternal(error);
}

int Client::LoginInternal(std::string* error) {
  std::string username;
  std::string password;
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    username = username_;
    password = password_;
  }
  if (username.empty()) {
    if (error)
      *error = "no credentials are available for re-authentication";
    return UVNVR_ERR_AUTH;
  }
  const json payload = {{"username", username},
                        {"password", password},
                        {"clientName", "uvnvr-sdk"},
                        {"resumeSessionId", session_id_.load()}};
  const CallResult result =
      Request(proto::kOpAuth, payload, false, static_cast<int>(options_.request_timeout_ms));
  if (result.code != UVNVR_OK) {
    if (error)
      *error = result.message;
    return result.code;
  }
  session_id_ = result.data.value("sessionId", 0ull);
  return UVNVR_OK;
}

void Client::Logout() {
  if (connected_.load() && session_id_.load() != 0)
    Request(proto::kOpLogout, json::object(), true, 2000);
  session_id_ = 0;
  {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    username_.clear();
    password_.clear();
  }
}

int Client::GetChannels(std::vector<Channel>* out, std::string* error) {
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;
  const CallResult result = RequestWithAuth(
      proto::kOpChannelList, json::object(), static_cast<int>(options_.request_timeout_ms));
  if (result.code != UVNVR_OK) {
    if (error)
      *error = result.message;
    return result.code;
  }
  if (!result.data.is_array()) {
    if (error)
      *error = "the server returned a malformed channel list";
    return UVNVR_ERR_PROTOCOL;
  }
  out->clear();
  for (const auto& item : result.data) {
    Channel channel;
    channel.id = item.value("id", int64_t{0});
    channel.group_id = item.value("groupId", int64_t{0});
    channel.port = item.value("port", 0);
    channel.enabled = item.value("enabled", false);
    channel.sub_enabled = item.value("subEnabled", false);
    channel.name = item.value("name", std::string());
    channel.protocol = item.value("protocol", std::string());
    channel.host = item.value("host", std::string());
    channel.main_state = item.value("mainState", std::string());
    channel.main_error = item.value("mainError", std::string());
    channel.sub_state = item.value("subState", std::string());
    channel.sub_error = item.value("subError", std::string());
    channel.online = channel.main_state == "online";
    channel.has_keyframe = item.value("mainHasKeyframe", false);
    out->push_back(std::move(channel));
  }
  return UVNVR_OK;
}

int Client::GetDays(int64_t channel_id, const std::string& from, const std::string& to,
                    std::vector<std::string>* out, std::string* error) {
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;
  const json payload = {{"channelId", channel_id}, {"from", from}, {"to", to}};
  const CallResult result = RequestWithAuth(
      proto::kOpRecordingDays, payload, static_cast<int>(options_.request_timeout_ms));
  if (result.code != UVNVR_OK) {
    if (error)
      *error = result.message;
    return result.code;
  }
  out->clear();
  const auto days = result.data.find("days");
  if (days != result.data.end() && days->is_array()) {
    for (const auto& day : *days) {
      if (day.is_string())
        out->push_back(day.get<std::string>());
    }
  }
  return UVNVR_OK;
}

int Client::GetRecordings(int64_t channel_id, const std::string& stream, int64_t from_ms,
                          int64_t to_ms, std::vector<Recording>* out, std::string* error) {
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;
  json payload = {{"channelId", channel_id}, {"fromMs", from_ms}, {"toMs", to_ms}};
  if (!stream.empty())
    payload["stream"] = stream;
  const CallResult result = RequestWithAuth(
      proto::kOpRecordingList, payload, static_cast<int>(options_.request_timeout_ms));
  if (result.code != UVNVR_OK) {
    if (error)
      *error = result.message;
    return result.code;
  }
  out->clear();
  const auto items = result.data.find("recordings");
  if (items == result.data.end() || !items->is_array())
    return UVNVR_OK;
  for (const auto& item : *items) {
    Recording recording;
    recording.id = item.value("id", int64_t{0});
    recording.channel_id = item.value("channelId", int64_t{0});
    recording.start_ms = item.value("startMs", int64_t{0});
    recording.end_ms = item.value("endMs", int64_t{0});
    recording.duration_ms = item.value("durationMs", int64_t{0});
    recording.size_bytes = item.value("sizeBytes", int64_t{0});
    recording.active = item.value("active", false);
    recording.stream = item.value("stream", std::string());
    recording.codec = item.value("codec", std::string());
    recording.record_type = item.value("recordType", std::string());
    recording.path = item.value("path", std::string());
    out->push_back(std::move(recording));
  }
  return UVNVR_OK;
}

int Client::LiveOpen(int64_t channel_id, int stream, FrameCallback callback,
                     std::string* error) {
  if (channel_id <= 0 || (stream != 0 && stream != 1) || !callback) {
    if (error)
      *error = "channelId, stream and callback are required";
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;
  const int handle = static_cast<int>(next_live_handle_.fetch_add(1));
  Post([this, handle, callback = std::move(callback)] {
    live_[handle] = LiveEntry{callback};
  });
  const json payload = {{"channelId", channel_id},
                        {"stream", stream == 0 ? "main" : "sub"},
                        {"streamId", handle}};
  const CallResult result = RequestWithAuth(
      proto::kOpLiveOpen, payload, static_cast<int>(options_.request_timeout_ms));
  if (result.code != UVNVR_OK) {
    Post([this, handle] { live_.erase(handle); });
    if (error)
      *error = result.message;
    return -result.code;
  }
  return handle;
}

int Client::LiveClose(int handle) {
  if (handle <= 0)
    return UVNVR_ERR_INVALID_ARGUMENT;
  Post([this, handle] {
    auto it = live_.find(handle);
    if (it != live_.end())
      live_.erase(it);
    if (connected_.load()) {
      const json payload = {{"streamId", handle}};
      SendJsonFrame(proto::kOpLiveClose, session_id_.load(), next_request_id_.fetch_add(1),
                    payload);
    }
  });
  return UVNVR_OK;
}

int Client::Download(int64_t recording_id, const std::string& path,
                     ProgressCallback progress, std::string* error) {
  if (recording_id <= 0 || path.empty()) {
    if (error)
      *error = "recordingId and path are required";
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  const int ready = EnsureReady(error);
  if (ready != UVNVR_OK)
    return ready;

  auto state = std::make_shared<DownloadState>();
  state->download_id = next_download_id_.fetch_add(1);
  state->path = path;
  state->progress = std::move(progress);
  state->file = std::make_shared<std::ofstream>(path, std::ios::binary | std::ios::trunc);
  if (!*state->file) {
    if (error)
      *error = "cannot open " + path + " for writing";
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    downloads_[state->download_id] = state;
  }

  uint32_t request_id = 0;
  const json payload = {{"recordingId", recording_id}, {"downloadId", state->download_id}};
  const CallResult begin =
      RequestWithAuth(proto::kOpDownloadOpen, payload,
                      static_cast<int>(options_.request_timeout_ms), &request_id);
  state->request_id = request_id;
  if (begin.code != UVNVR_OK) {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    downloads_.erase(state->download_id);
    if (error)
      *error = begin.message;
    return begin.code;
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->total = begin.data.value("sizeBytes", int64_t{0});
  }
  if (state->progress)
    state->progress(0, state->total);

  int code = UVNVR_OK;
  std::string message;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->done) {
      if (state->cv.wait_for(lock, std::chrono::seconds(30)) == std::cv_status::timeout &&
          !state->done) {
        state->done = true;
        state->code = UVNVR_ERR_TIMEOUT;
        state->message = "the download stalled";
      }
    }
    code = state->code;
    message = state->message;
  }
  state->file->flush();
  state->file->close();
  {
    std::lock_guard<std::mutex> lock(downloads_mutex_);
    downloads_.erase(state->download_id);
  }
  if (code != UVNVR_OK && error)
    *error = message;
  return code;
}

void Client::Cancel() {
  Post([this] {
    FailAllPending(UVNVR_ERR_CANCELLED, "cancelled by the application");
    FailDownloads(UVNVR_ERR_CANCELLED, "cancelled by the application");
  });
}

uvnvr_transport_t Client::Transport() const {
  return static_cast<uvnvr_transport_t>(transport_.load());
}

} // namespace uvnvr::sdk
