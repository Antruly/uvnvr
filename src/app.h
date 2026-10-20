#pragma once

#include <atomic>
#include <cstdint>
#include <db/uvcpp_db_pool.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <req/uvcpp_work.h>
#include <string>
#include <vector>
#include <webapp/uvcpp_web_app.h>

namespace uvnvr {

using json = nlohmann::json;

struct Config {
  std::string data_dir = "./data";
  std::string db_path;
  int port = 8080;
  bool tls_self_signed = false;
  std::string tls_cert;
  std::string tls_key;
  bool maintenance = false;
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
  int Run();

private:
  bool Init(std::string* error);
  bool Migrate(std::string* error);
  bool EnsureSerial(std::string* error);
  void EnsureDefaults();
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
  std::atomic<bool> setup_required_{true};
  std::string serial_;

  std::mutex metrics_mutex_;
  std::mutex audit_cleanup_mutex_;
  std::atomic<int64_t> last_audit_cleanup_{0};
  uint64_t last_cpu_total_ = 0;
  uint64_t last_cpu_idle_ = 0;
};

} // namespace uvnvr
