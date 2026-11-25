#pragma once

#include "config.h"
#include "media.h"
#include "ntp.h"
#include "onvif_events.h"
#include "playback.h"
#include "proto/proto_server.h"
#include "recording.h"
#include "rtsp.h"

#if defined(UVNVR_ENABLE_GB28181) && UVNVR_ENABLE_GB28181
#include "gb28181/service.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <db/uvcpp_db_pool.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <req/uvcpp_work.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <webapp/uvcpp_web_app.h>

#ifdef UVNVR_ENABLE_SECURE
#include "crypto/sm.h"
#endif

namespace uvnvr {

#if UVNVR_ENABLE_AI
class AiService;
#endif

using json = nlohmann::json;

struct ChannelRow {
  int64_t id = 0;
  int64_t group_id = 0;
  std::string name;
  std::string protocol;
  std::string host;
  int port = 554;
  std::string username;
  std::string password_enc;
  std::string main_url;
  std::string sub_url;
  bool sub_enabled = false;
  bool enabled = true;
  std::string brand;
  std::string media_json = "{}";
  std::string watermark_json = "{}";
  std::string last_status = "unknown";
  std::string last_error;
  int64_t last_online_at = 0;
  int64_t last_offline_at = 0;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};

struct StreamTicket {
  int64_t user_id = 0;
  int64_t channel_id = 0;
  std::string stream;
  int64_t expires_at = 0;
  bool playback = false;
  bool events = false;
  bool status = false;
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  double speed = 1.0;
};

struct UserContext {
  int64_t id = 0;
  std::string username;
  std::string role_name;
  std::vector<std::string> permissions;
  std::string session_hash;
};

struct AuthResult {
  bool ok = false;
  int status = 401;
  std::string code;
  std::string message;
  UserContext user;
  std::string csrf;
};

struct LoginResult {
  bool ok = false;
  int status = 400;
  std::string code;
  std::string message;
  int retry_after_seconds = 0;
  UserContext user;
  std::string csrf;
  std::string session_token;
  int64_t expires_at = 0;
};

// Download state shared between the dispatcher and its chunk pump. Defined in
// app.cpp; forward-declared here so the pump can be a real member function
// instead of a self-referencing lambda whose lifetime is hard to reason about.
struct ProtoDownloadState;

class App {
public:
  explicit App(Config config);
  ~App();
  int Run();
  // Thread-safe; makes Run() return. Used by the Windows service control
  // handler and by service registration transitions.
  void RequestShutdown();
  void SetOpenBrowser(bool open) {
    open_browser_ = open;
  }
  // Invoked once the web server, media pipeline and RTSP listener are ready.
  void SetStartedCallback(std::function<void()> callback) {
    started_callback_ = std::move(callback);
  }

private:
  bool Init(std::string* error);
  bool Migrate(std::string* error);
  bool EnsureSerial(std::string* error);
  bool EnsureSecretKey(std::string* error);
  void EnsureDefaults();
  std::string EncryptSecret(const std::string& plain, std::string* error);
  std::string DecryptSecret(const std::string& encoded, std::string* error);
  bool LoadChannel(int64_t id, ChannelRow* out, std::string* error);
  std::vector<ChannelRow> LoadChannels(std::string* error);
  json ChannelJson(const ChannelRow& channel) const;
  std::string BuildChannelUrl(const ChannelRow& channel, bool sub, std::string* error);
  void SyncMediaChannel(int64_t id);
  void SyncAllMedia();
  void StartMonitors();
  void StopMonitors();
  void MonitorRun();
  void RaiseAlert(int64_t channel_id, const std::string& type, const std::string& level,
                  const std::string& message, const std::string& snapshot_path);
  void RegisterAlertRoutes();
  void RegisterSystemRoutes();
  void RegisterChannelRoutes();
  void RegisterGroupRoutes();
  void RegisterLayoutRoutes();
  void RegisterOnvifRoutes();
  void RegisterStreamRoutes();
  void RegisterRecordingRoutes();
  void RegisterStorageRoutes();
  void RegisterPlanRoutes();
#if UVNVR_ENABLE_AI
  void RegisterAiRoutes();
#endif
  bool CreateStreamTicket(int64_t user_id, int64_t channel_id, const std::string& stream,
                          std::string* token, int64_t* expires_at, std::string* error);
  bool CreateEventsTicket(int64_t user_id, std::string* token, int64_t* expires_at,
                          std::string* error);
  bool CreateStatusTicket(int64_t user_id, std::string* token, int64_t* expires_at,
                          std::string* error);
  void BroadcastEvent(const json& event);
  void BroadcastStatus(const json& message);
  void StatusRun();
  void StartStatusPush();
  void StopStatusPush();
  bool ValidateStreamTicket(const std::string& token, StreamTicket* ticket);
  bool VerifyRtspCredentials(const std::string& username, const std::string& password);
  std::string RtspHa1ForUser(const std::string& username);
  bool ApplySystemTime(int64_t epoch_ms, std::string* error);
  json SyncCameraTimes(int64_t epoch_ms);
  bool CreatePlaybackTicket(int64_t user_id, int64_t channel_id, const std::string& stream,
                            int64_t start_ms, int64_t end_ms, double speed, std::string* token,
                            int64_t* expires_at, std::string* error);
  void RegisterMiddleware();
  void RegisterPublicRoutes();
  void RegisterCoreRoutes();
  void RegisterProtectedRoutes();
  void RegisterStatic();

  // GB/T 28181 device-side service (opt-in). It is started only when the
  // `gb28181.enabled` setting is on; the default build has the module off.
  void StartGb28181();
  void StopGb28181();

  // Sends one PTZ operation to an ONVIF camera on a worker thread. Shared by
  // the GB/T 28181 DeviceControl path and the private protocol's PTZ_CONTROL
  // opcode. `operation` uses the OnvifPtzOperation order: 0 continuous move,
  // 1 stop, 2 set preset, 3 goto preset, 4 remove preset. The completion runs
  // on the web loop.
  bool QueuePtz(int64_t channel_id, int operation, double pan, double tilt, double zoom,
                int preset, std::function<void(bool, const std::string&)> done);

  // Private protocol (docs/private-protocol.md). The dispatcher reuses the
  // same services and database rows as the REST handlers.
  void StartPrivateProtocol();
  void StartPrivateProtocolOnLoop();
  void StopPrivateProtocol();
  proto::AuthResult ProtoAuthenticate(const std::string& username,
                                      const std::string& password,
                                      const std::string& peer);
  void HandleProtoRequest(std::shared_ptr<proto::ProtoRequest> request,
                          std::function<void(proto::ProtoReply)> complete);
  void ProtoRemoveLive(uint64_t key);
  void ProtoPumpDownload(std::shared_ptr<ProtoDownloadState> state);
  UserContext ProtoUser(const proto::ProtoRequest& request) const;

  std::string Serial();
  std::string GetSetting(const std::string& key, const std::string& fallback);
  int64_t GetSettingInt(const std::string& key, int64_t fallback);
  bool SetSetting(const std::string& key, const std::string& value, std::string* error);
  // Uses the stored default camera credentials when `use_default` is set;
  // otherwise passes the supplied values through.
  bool ResolveCameraCredentials(bool use_default, const std::string& username,
                                const std::string& password, std::string* out_username,
                                std::string* out_password, std::string* error);
  bool Exec(const std::string& sql, const uvcpp::uvcpp_db_params& params, int64_t* affected,
            std::string* error);
  bool Query(const std::string& sql, const uvcpp::uvcpp_db_params& params,
             uvcpp::uvcpp_db_table* out, std::string* error);
  int64_t CountUsers();
  int64_t AdminRoleId();
  json BuildSummary();
  json SettingsJson();
  bool UpdateEventSettings(const json& body, std::string* error);
  bool UpdateMotionSettings(const json& body, std::string* error);
  std::string PerformanceTier() const;
  bool RequireMaintenance(const uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp);
  bool SetSerial(const std::string& serial, std::string* error);
  std::string EffectiveVersion();
  void Audit(const std::string& category, const std::string& level, const std::string& actor,
             const std::string& ip, const std::string& ua, const std::string& object_type,
             const std::string& object_id, const std::string& action, const std::string& result,
             const std::string& error_code, const std::string& detail);
  void CleanupAudit();
  void CleanupAuditLocked();

#ifdef UVNVR_ENABLE_SECURE
  // Loads the device certificate and derives the SM4 storage key. Missing or
  // malformed material disables encryption with a diagnostic instead of
  // preventing the service from starting.
  void InitializeSecureStorage();
  bool SealSnapshotBytes(const std::string& path, const std::string& plaintext,
                         std::string* stored, std::string* error);
  bool OpenSnapshotBytes(const std::string& path, const std::string& stored,
                         std::string* plaintext, std::string* error);
  bool WriteSnapshotFile(const std::string& path, const std::string& plaintext,
                         std::string* error);
  bool ReadSnapshotFile(const std::string& path, std::string* plaintext, std::string* error);
  // Re-reads the first/last chained hashes into system_meta after a delete.
  void RefreshAuditChainMeta();
  std::mutex audit_chain_mutex_;
  std::string device_cert_path_;
  gm::Bytes at_rest_key_;
  bool at_rest_key_ready_ = false;
#endif

  LoginResult Login(const std::string& username, const std::string& password, const std::string& ip,
                    const std::string& ua);
  LoginResult Setup(const std::string& password, const std::string& timezone, const std::string& ip,
                    const std::string& ua);
  AuthResult Authenticate(const std::string& cookie_token, const std::string& bearer_token,
                          const std::string& csrf_header, bool mutating, const std::string& ip,
                          const std::string& ua);
  UserContext LoadUserBySession(const std::string& token_hash);
  bool CreateSession(int64_t user_id, const std::string& ip, const std::string& ua,
                     std::string* token, std::string* csrf, int64_t* expires_at);
  bool DeleteSession(const std::string& token_hash);
  bool RecordLoginFailure(const std::string& username, const std::string& ip);
  bool IsWebLocked(int64_t* retry_after);
  void ClearWebFailures();
  bool IsApiLocked(int64_t* retry_after);
  bool RecordApiFailure(const std::string& ip);
  void ClearApiFailures();

  bool PasswordValid(const std::string& password, std::string* message);
  bool HasPermission(const UserContext& user, const std::string& permission) const;
  json UserJson(const UserContext& user) const;

  uvcpp::uvcpp_web_handler Protected(uvcpp::uvcpp_web_handler handler,
                                     const std::string& permission = "");

  template<typename F> void RunWork(F work, std::function<void()> done) {
    auto work_fn = std::make_shared<std::function<void()>>(std::move(work));
    auto done_fn = std::make_shared<std::function<void()>>(std::move(done));
    auto* w = new uvcpp::uvcpp_work();
    w->init();
    const int rc = w->queue_work(
        web_.loop(), [work_fn](uvcpp::uvcpp_work*) { (*work_fn)(); },
        [done_fn, w](uvcpp::uvcpp_work*, int) {
          (*done_fn)();
          delete w;
        });
    if (rc != 0) {
      (*done_fn)();
      delete w;
    }
  }

  Config config_;
  uvcpp::uvcpp_web_app web_;
  uvcpp::uvcpp_db_pool db_;
  std::unique_ptr<MediaService> media_;
  std::unique_ptr<OnvifEventService> onvif_events_;
  std::unique_ptr<RecordingService> recording_;
  std::unique_ptr<PlaybackService> playback_;
  std::unique_ptr<RtspServer> rtsp_;
  std::unique_ptr<proto::ProtoServer> proto_;
#if defined(UVNVR_ENABLE_GB28181) && UVNVR_ENABLE_GB28181
  std::unique_ptr<gb28181::Gb28181Service> gb28181_;
  // Alarm sources (AI workers, the monitor thread) run concurrently with
  // shutdown; this guards the pointer and keeps ReportAlarm alive until the
  // queue accepts the alarm.
  mutable std::mutex gb28181_mutex_;
#endif
#if UVNVR_ENABLE_AI
  std::unique_ptr<AiService> ai_;
#endif
  struct ProtoLiveSubscription {
    int64_t channel_id = 0;
    std::string stream;
    uint64_t media_subscription = 0;
  };
  std::mutex proto_live_mutex_;
  std::unordered_map<uint64_t, ProtoLiveSubscription> proto_live_;
  std::thread monitor_thread_;
  std::atomic<bool> monitor_stop_{false};
  std::map<int64_t, std::string> last_channel_state_;
  std::map<int64_t, std::vector<uint8_t>> last_motion_frame_;
  std::map<int64_t, int64_t> motion_cooldown_;
  int64_t last_storage_alert_ms_ = 0;
  int64_t last_disk_error_alert_at_ = 0;
  int64_t last_service_error_alert_at_ = 0;
  std::vector<unsigned char> secret_key_;
  std::mutex tickets_mutex_;
  std::unordered_map<std::string, StreamTicket> tickets_;

  struct EventClient {
    uint64_t id = 0;
    std::atomic<uvcpp::uvcpp_ws_connection*> connection{nullptr};
  };
  struct EventHub {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<EventClient>> clients;
    std::atomic<uint64_t> next_id{1};
  };
  std::shared_ptr<EventHub> event_hub_ = std::make_shared<EventHub>();
  // /ws/status: one changed-item push per second. Kept separate from the
  // alert socket because viewers without alert.view still need channel state.
  std::shared_ptr<EventHub> status_hub_ = std::make_shared<EventHub>();
  std::thread status_thread_;
  std::atomic<bool> status_stop_{false};
  // Serialised changed items from the previous tick; the next push contains
  // only rows whose JSON differs.
  std::map<int64_t, std::string> last_status_snapshot_;
  std::atomic<bool> setup_required_{true};
  std::mutex serial_mutex_;
  std::string serial_;

  std::mutex metrics_mutex_;
  std::mutex audit_cleanup_mutex_;
  std::atomic<int64_t> last_audit_cleanup_{0};
  uint64_t last_cpu_total_ = 0;
  uint64_t last_cpu_idle_ = 0;
  bool open_browser_ = false;
  std::function<void()> started_callback_;
};

} // namespace uvnvr
