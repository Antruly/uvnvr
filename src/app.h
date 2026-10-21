#pragma once

#include "config.h"
#include "media.h"
#include "ntp.h"
#include "onvif_events.h"
#include "playback.h"
#include "recording.h"
#include "rtsp.h"

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

namespace uvnvr {

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

class App {
public:
  explicit App(Config config);
  ~App();
  int Run();

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
  bool CreateStreamTicket(int64_t user_id, int64_t channel_id, const std::string& stream,
                          std::string* token, int64_t* expires_at, std::string* error);
  bool ValidateStreamTicket(const std::string& token, StreamTicket* ticket);
  bool VerifyRtspCredentials(const std::string& username, const std::string& password);
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

  std::string Serial();
  std::string GetSetting(const std::string& key, const std::string& fallback);
  int64_t GetSettingInt(const std::string& key, int64_t fallback);
  bool SetSetting(const std::string& key, const std::string& value, std::string* error);
  bool Exec(const std::string& sql, const uvcpp::uvcpp_db_params& params, int64_t* affected,
            std::string* error);
  bool Query(const std::string& sql, const uvcpp::uvcpp_db_params& params,
             uvcpp::uvcpp_db_table* out, std::string* error);
  int64_t CountUsers();
  int64_t AdminRoleId();
  json BuildSummary();
  json SettingsJson();
  void Audit(const std::string& category, const std::string& level, const std::string& actor,
             const std::string& ip, const std::string& ua, const std::string& object_type,
             const std::string& object_id, const std::string& action, const std::string& result,
             const std::string& error_code, const std::string& detail);
  void CleanupAudit();
  void CleanupAuditLocked();

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
  std::thread monitor_thread_;
  std::atomic<bool> monitor_stop_{false};
  std::map<int64_t, std::string> last_channel_state_;
  std::map<int64_t, std::vector<uint8_t>> last_motion_frame_;
  std::map<int64_t, int64_t> motion_cooldown_;
  int64_t last_storage_alert_ms_ = 0;
  std::vector<unsigned char> secret_key_;
  std::mutex tickets_mutex_;
  std::unordered_map<std::string, StreamTicket> tickets_;
  std::atomic<bool> setup_required_{true};
  std::string serial_;

  std::mutex metrics_mutex_;
  std::mutex audit_cleanup_mutex_;
  std::atomic<int64_t> last_audit_cleanup_{0};
  uint64_t last_cpu_total_ = 0;
  uint64_t last_cpu_idle_ = 0;
};

} // namespace uvnvr
