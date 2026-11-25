#pragma once

#include "uvnvr_sdk.h"

#include "proto/proto_protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <net/uvcpp_tcp_client.h>
#include <uvcpp/uvcpp_config.h>

#if defined(UVNVR_ENABLE_QUIC) && UVNVR_ENABLE_QUIC && defined(UVCPP_QUIC_ENABLE) && UVCPP_QUIC_ENABLE
#define UVNVR_SDK_HAVE_QUIC 1
#endif

#if defined(UVNVR_SDK_HAVE_QUIC)
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

#ifndef UVNVR_SDK_VERSION
#define UVNVR_SDK_VERSION "0.1.0"
#endif

namespace uvnvr::sdk {

using json = nlohmann::json;

struct Options {
  uint32_t connect_timeout_ms = 5000;
  uint32_t request_timeout_ms = 10000;
  uint32_t quic_probe_timeout_ms = 400;
  bool prefer_quic = true;
  bool verify_peer = false;
  std::string ca_file;
  uint32_t heartbeat_interval_ms = 15000;
  uint32_t session_renew_ms = 600000;
  bool disable_tcp = false;
};

struct Channel {
  int64_t id = 0;
  int64_t group_id = 0;
  int32_t port = 0;
  bool enabled = false;
  bool sub_enabled = false;
  bool online = false;
  bool has_keyframe = false;
  std::string name;
  std::string protocol;
  std::string host;
  std::string main_state;
  std::string main_error;
  std::string sub_state;
  std::string sub_error;
};

struct Recording {
  int64_t id = 0;
  int64_t channel_id = 0;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  int64_t duration_ms = 0;
  int64_t size_bytes = 0;
  bool active = false;
  std::string stream;
  std::string codec;
  std::string record_type;
  std::string path;
};

struct CallResult {
  int code = UVNVR_OK;
  std::string message;
  std::string server_code;
  json data = json::object();
};

struct AiEvent {
  int64_t id = 0;
  int32_t type = 0; // 1 = plate, 2 = face
  int64_t channel_id = 0;
  int64_t ts = 0;
  bool alert = false;
  double confidence = 0.0;
  double quality = 0.0;
  std::string channel_name;
  std::string plate;
  std::string plate_norm;
  std::string color;
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  std::string engine;
  int64_t matched_face_id = 0;
  double matched_score = 0.0;
  std::string matched_name;
  std::string image_url;
  std::string crop_url;
};

using FrameCallback =
    std::function<void(int handle, const uint8_t* data, size_t size, int codec, uint32_t flags,
                       int64_t pts_ms)>;
using AiEventCallback = std::function<void(int handle, const AiEvent& event)>;
using ProgressCallback = std::function<void(int64_t received, int64_t total)>;

// Internal client: owns one libuv loop thread plus one persistent transport.
// Every public method is called from an application thread and blocks until
// the loop thread has answered.
class Client {
public:
  explicit Client(const Options& options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void SetOwner(uvnvr_sdk_t* owner) { owner_ = owner; }

  int Connect(const std::string& host, uint16_t port, int timeout_ms, std::string* error);
  int Login(const std::string& username, const std::string& password, std::string* error);
  void Logout();
  int GetChannels(std::vector<Channel>* out, std::string* error);
  int GetDays(int64_t channel_id, const std::string& from, const std::string& to,
              std::vector<std::string>* out, std::string* error);
  int GetRecordings(int64_t channel_id, const std::string& stream, int64_t from_ms,
                    int64_t to_ms, std::vector<Recording>* out, std::string* error);
  int LiveOpen(int64_t channel_id, int stream, FrameCallback callback, std::string* error);
  int LiveClose(int handle);
  int AiSubscribe(int64_t channel_id, uint32_t types, AiEventCallback callback,
                  std::string* error);
  int AiUnsubscribe(int handle);
  int Download(int64_t recording_id, const std::string& path, ProgressCallback progress,
               std::string* error);
  void Cancel();

  uvnvr_transport_t Transport() const;

private:
  struct Pending {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int code = 0;
    std::string message;
    std::string server_code;
    json data = json::object();
  };

  struct LiveEntry {
    FrameCallback callback;
  };

  struct AiEntry {
    AiEventCallback callback;
  };

  struct DownloadState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int code = UVNVR_OK;
    std::string message;
    uint32_t download_id = 0;
    uint32_t request_id = 0;
    std::string path;
    std::shared_ptr<std::ofstream> file; // opened on the caller thread
    int64_t received = 0;
    int64_t total = 0;
    ProgressCallback progress;
  };

  // Loop-thread plumbing.
  void Post(std::function<void()> task);
  void DrainTasks();
  int StartTaskPump();
  void OnTaskPump();
  void ShutdownOnLoop();
  int WaitForConnect(const std::shared_ptr<Pending>& waiter, int timeout_ms,
                     std::string* error);

  // Connections.
  void BeginConnect(const std::string& host, uint16_t port, int timeout_ms,
                    std::shared_ptr<Pending> waiter);
  bool StartQuic(uint64_t epoch);
  void StartTcp(uint64_t epoch);
  void BeginTcpFallback(uint64_t epoch);
  void BeginHandshake(uint64_t epoch);
  void OnTcpConnected(uint64_t epoch, int status);
  void OnQuicHandshake(uint64_t epoch, int status);
  void OnTransportClosed(uint64_t epoch);
  void FailConnect(uint64_t epoch, int code, const std::string& message);
  void StartConnectTimer(uint32_t timeout_ms, uint64_t epoch, bool quic_phase);
  void StopConnectTimer();
  void StartKeepalive();
  void OnConnectTimeout();

  // Framing.
  bool SendFrame(uint16_t opcode, uint16_t flags, uint64_t session_id, uint32_t request_id,
                 const void* payload, size_t payload_len);
  bool SendJsonFrame(uint16_t opcode, uint64_t session_id, uint32_t request_id,
                     const json& payload);
  void PumpTcpWrites();
  void OnBytes(const char* data, size_t len);
  void ParseInput();
  void HandleFrame(const proto::Header& header, std::string payload);
  void HandleResponse(const proto::Header& header, const std::string& payload);
  void HandleStreamFrame(const proto::Header& header, const std::string& payload);
  void HandleAiEvent(const std::string& payload);
  void HandleError(const proto::Header& header, const std::string& payload);

  // Requests.
  CallResult Request(uint16_t opcode, const json& payload, bool with_session, int timeout_ms,
                     uint32_t* used_request_id = nullptr);
  CallResult RequestWithAuth(uint16_t opcode, const json& payload, int timeout_ms,
                             uint32_t* used_request_id = nullptr);
  int EnsureReady(std::string* error);
  int LoginInternal(std::string* error);
  void CompletePending(const std::shared_ptr<Pending>& pending, int code,
                       const std::string& message, const std::string& server_code,
                       const json& data);
  void FailAllPending(int code, const std::string& message);
  void FailDownloads(int code, const std::string& message);
  void FailDownload(const std::shared_ptr<DownloadState>& state, int code,
                    const std::string& message);
  std::shared_ptr<DownloadState> FindDownload(uint32_t download_id);
  void CloseTransport();

  Options options_;
  uvnvr_sdk_t* owner_ = nullptr;

  // Member order matters: loop first, then handles bound to it, then thread.
  uvcpp::uvcpp_loop loop_;
  uvcpp::uvcpp_timer task_timer_;
  uvcpp::uvcpp_timer connect_timer_;
  uvcpp::uvcpp_timer heartbeat_timer_;
  uvcpp::uvcpp_timer renew_timer_;
  std::thread thread_;
  // Loop-thread only. This SDK never calls uv_async_send(): the transports and
  // the loop belong to libuvcpp's statically linked libuv, and resolving that
  // symbol from a second libuv (the one the platform package provides) is not
  // valid across libuv versions. The pump timer is created through libuvcpp,
  // so every libuv call stays inside the same instance.
  uint32_t task_pump_interval_ms_ = 0;

  std::mutex task_mutex_;
  std::deque<std::function<void()>> tasks_;
  std::atomic<bool> running_{true};

  std::mutex meta_mutex_;
  std::string host_;
  uint16_t port_ = 1515;
  std::string username_;
  std::string password_;

  std::atomic<bool> connected_{false};
  std::atomic<int> transport_{UVNVR_TRANSPORT_UNKNOWN};
  std::atomic<uint64_t> session_id_{0};
  std::atomic<uint32_t> next_request_id_{1};
  std::atomic<uint32_t> next_live_handle_{1};
  std::atomic<uint32_t> next_ai_handle_{1};
  std::atomic<uint32_t> next_download_id_{1};
  std::atomic<uint64_t> generation_{0};

  // All fields below are touched only on the loop thread unless noted.
  uint64_t current_epoch_ = 0;
  uint64_t timer_epoch_ = 0;
  bool connecting_ = false;
  bool quic_attempt_active_ = false;
  std::shared_ptr<Pending> connect_waiter_;
  uint32_t hello_request_id_ = 0;
  bool hello_pending_ = false;
  uint32_t connect_timeout_ms_ = 5000;
  std::chrono::steady_clock::time_point connect_started_;

  std::unique_ptr<uvcpp::uvcpp_tcp_client> tcp_;
  std::vector<std::vector<uint8_t>> tcp_outbox_;
  size_t tcp_outbox_bytes_ = 0;
  bool tcp_write_in_flight_ = false;

#if defined(UVNVR_SDK_HAVE_QUIC)
  std::unique_ptr<uvcpp::uvcpp_ssl_context> quic_context_;
  std::unique_ptr<uvcpp::uvcpp_quic_client> quic_;
  int64_t quic_stream_ = -1;
  std::vector<std::vector<uint8_t>> quic_pending_;
#endif

  std::vector<uint8_t> input_;
  std::unordered_map<uint32_t, std::shared_ptr<Pending>> pending_;
  std::unordered_map<int, LiveEntry> live_;
  std::unordered_map<int, AiEntry> ai_subscriptions_;

  std::mutex downloads_mutex_;
  std::unordered_map<uint32_t, std::shared_ptr<DownloadState>> downloads_;
};

} // namespace uvnvr::sdk
