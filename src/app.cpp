#include "app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <db/uvcpp_db.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <uv.h>
#include <uvcpp/uvcpp_threadpool.h>
#include <uvnvr/version.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_middleware.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace uvnvr {
namespace {

constexpr int64_t kSessionTtlSeconds = 24 * 60 * 60;
constexpr int kScryptN = 1 << 15;
constexpr int kScryptR = 8;
constexpr int kScryptP = 1;
constexpr size_t kMaxAuditPageSize = 200;

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string HexEncode(const unsigned char* data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kHex[(data[i] >> 4) & 0x0f]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

bool HexDecode(std::string_view text, std::vector<unsigned char>* out) {
  if (text.size() % 2 != 0)
    return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  out->clear();
  out->reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out->push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

std::string Sha256Hex(std::string_view data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  return HexEncode(digest, sizeof(digest));
}

std::string RandomHex(size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
    return {};
  }
  return HexEncode(buffer.data(), buffer.size());
}

bool HashPassword(const std::string& password, std::string* out) {
  unsigned char salt[16];
  if (RAND_bytes(salt, sizeof(salt)) != 1)
    return false;
  unsigned char hash[32];
  if (EVP_PBE_scrypt(password.data(), password.size(), salt, sizeof(salt), kScryptN, kScryptR,
                     kScryptP, 64 * 1024 * 1024, hash, sizeof(hash)) != 1) {
    return false;
  }
  *out = "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" +
         std::to_string(kScryptP) + "$" + HexEncode(salt, sizeof(salt)) + "$" +
         HexEncode(hash, sizeof(hash));
  return true;
}

bool VerifyPassword(const std::string& password, const std::string& encoded) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= encoded.size()) {
    const size_t end = encoded.find('$', start);
    parts.push_back(
        encoded.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  if (parts.size() != 6 || parts[0] != "scrypt")
    return false;
  uint64_t n = 0;
  uint64_t r = 0;
  uint64_t p = 0;
  auto parse = [](const std::string& s, uint64_t* v) {
    const auto result = std::from_chars(s.data(), s.data() + s.size(), *v);
    return result.ec == std::errc() && result.ptr == s.data() + s.size();
  };
  if (!parse(parts[1], &n) || !parse(parts[2], &r) || !parse(parts[3], &p))
    return false;
  std::vector<unsigned char> salt;
  std::vector<unsigned char> expected;
  if (!HexDecode(parts[4], &salt) || !HexDecode(parts[5], &expected))
    return false;
  std::vector<unsigned char> actual(expected.size());
  if (EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(), n, r, p,
                     64 * 1024 * 1024, actual.data(), actual.size()) != 1) {
    return false;
  }
  return actual.size() == expected.size() &&
         CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(separator, start);
    out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return out;
}

std::string Join(const std::vector<std::string>& values, const std::string& separator) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0)
      out += separator;
    out += values[i];
  }
  return out;
}

std::string TableText(const uvcpp::uvcpp_db_table& table, size_t row, const std::string& column) {
  if (row >= table.rows().size())
    return {};
  return table.at(row, column).to_text();
}

int64_t TableInt(const uvcpp::uvcpp_db_table& table, size_t row, const std::string& column) {
  if (row >= table.rows().size())
    return 0;
  return table.at(row, column).to_int64();
}

std::vector<std::string> ParsePermissions(const std::string& text) {
  std::vector<std::string> out;
  const json parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array())
    return out;
  for (const auto& item : parsed) {
    if (item.is_string())
      out.push_back(item.get<std::string>());
  }
  return out;
}

void SendJson(uvcpp::uvcpp_web_response& resp, int status, const json& body) {
  resp.status(status).json_str(body.dump());
  resp.end();
}

void SendError(uvcpp::uvcpp_web_response& resp, int status, const std::string& code,
               const std::string& message) {
  SendJson(resp, status, json{{"error", code}, {"message", message}});
}

bool ParseIntParam(const std::string* value, int64_t* out) {
  if (value == nullptr || value->empty())
    return false;
  const auto result = std::from_chars(value->data(), value->data() + value->size(), *out);
  return result.ec == std::errc() && result.ptr == value->data() + value->size();
}

const std::vector<std::string>& AllPermissions() {
  static const std::vector<std::string> kPermissions = {
      "dashboard.view", "preview.view",  "playback.view", "playback.download", "channel.view",
      "channel.manage", "plan.view",     "plan.manage",   "storage.view",      "storage.manage",
      "alert.view",     "alert.manage",  "user.view",     "user.manage",       "log.view",
      "system.view",    "system.manage", "api.access",    "api.snapshot",      "api.stream",
      "api.download",   "api.alerts",    "rtsp.access",   "maintenance",
  };
  return kPermissions;
}

bool IsKnownPermission(const std::string& permission) {
  if (permission == "*")
    return true;
  const auto& all = AllPermissions();
  return std::find(all.begin(), all.end(), permission) != all.end();
}

const char* kOperatorPermissions =
    R"(["dashboard.view","preview.view","playback.view","playback.download","channel.view","channel.manage","plan.view","plan.manage","storage.view","alert.view","alert.manage","log.view","api.access","api.snapshot","api.stream","api.download","api.alerts","rtsp.access"])";
const char* kViewerPermissions =
    R"(["dashboard.view","preview.view","playback.view","alert.view"])";

} // namespace

App::App(Config config) : config_(std::move(config)) {
  if (config_.db_path.empty()) {
    config_.db_path = (std::filesystem::path(config_.data_dir) / "uvnvr.db").string();
  }
}

bool App::Exec(const std::string& sql, const uvcpp::uvcpp_db_params& params, int64_t* affected,
               std::string* error) {
  const auto status = db_.execute(sql, params, affected);
  if (!uvcpp::uvcpp_db_ok(status)) {
    if (error != nullptr)
      *error = db_.last_error();
    return false;
  }
  return true;
}

bool App::Query(const std::string& sql, const uvcpp::uvcpp_db_params& params,
                uvcpp::uvcpp_db_table* out, std::string* error) {
  const auto status = db_.query(sql, params, out);
  if (!uvcpp::uvcpp_db_ok(status)) {
    if (error != nullptr)
      *error = db_.last_error();
    return false;
  }
  return true;
}

std::string App::GetSetting(const std::string& key, const std::string& fallback) {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(key);
  std::string error;
  if (!Query("SELECT value FROM settings WHERE key = ?", params, &table, &error) || table.empty()) {
    return fallback;
  }
  return TableText(table, 0, "value");
}

int64_t App::GetSettingInt(const std::string& key, int64_t fallback) {
  const std::string value = GetSetting(key, std::to_string(fallback));
  int64_t parsed = fallback;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    return fallback;
  return parsed;
}

bool App::SetSetting(const std::string& key, const std::string& value, std::string* error) {
  uvcpp::uvcpp_db_params params;
  params.add(key);
  params.add(value);
  params.add(NowSeconds());
  return Exec(
      "INSERT INTO settings(key, value, updated_at) VALUES(?, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
      params, nullptr, error);
}

int64_t App::CountUsers() {
  uvcpp::uvcpp_db_table table;
  std::string error;
  if (!Query("SELECT COUNT(*) AS n FROM users", {}, &table, &error) || table.empty())
    return 0;
  return TableInt(table, 0, "n");
}

int64_t App::AdminRoleId() {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add("admin");
  std::string error;
  if (!Query("SELECT id FROM roles WHERE name = ?", params, &table, &error) || table.empty()) {
    return 0;
  }
  return TableInt(table, 0, "id");
}

bool App::Migrate(std::string* error) {
  const std::vector<std::string> statements = {
      "PRAGMA journal_mode=WAL",
      "PRAGMA synchronous=NORMAL",
      "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at "
      "INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS roles ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL UNIQUE,"
      "display_name TEXT NOT NULL DEFAULT '',"
      "permissions TEXT NOT NULL DEFAULT '[]',"
      "builtin INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS users ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
      "password_hash TEXT NOT NULL,"
      "role_id INTEGER NOT NULL,"
      "enabled INTEGER NOT NULL DEFAULT 1,"
      "created_at INTEGER NOT NULL,"
      "last_login_at INTEGER)",
      "CREATE TABLE IF NOT EXISTS sessions ("
      "token_hash TEXT PRIMARY KEY,"
      "user_id INTEGER NOT NULL,"
      "csrf_token TEXT NOT NULL,"
      "ip TEXT,"
      "user_agent TEXT,"
      "created_at INTEGER NOT NULL,"
      "expires_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS api_tokens ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "user_id INTEGER NOT NULL,"
      "name TEXT NOT NULL,"
      "token_hash TEXT NOT NULL UNIQUE,"
      "created_at INTEGER NOT NULL,"
      "expires_at INTEGER,"
      "last_used_at INTEGER)",
      "CREATE TABLE IF NOT EXISTS login_attempts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "kind TEXT NOT NULL,"
      "username TEXT,"
      "ip TEXT,"
      "success INTEGER NOT NULL,"
      "at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS settings ("
      "key TEXT PRIMARY KEY,"
      "value TEXT NOT NULL,"
      "updated_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS audit_log ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts INTEGER NOT NULL,"
      "category TEXT NOT NULL,"
      "level TEXT NOT NULL,"
      "actor TEXT,"
      "ip TEXT,"
      "user_agent TEXT,"
      "object_type TEXT,"
      "object_id TEXT,"
      "action TEXT NOT NULL,"
      "result TEXT NOT NULL,"
      "error_code TEXT,"
      "detail TEXT,"
      "request_id TEXT)",
      "CREATE TABLE IF NOT EXISTS system_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
      "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id)",
      "CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at)",
      "CREATE INDEX IF NOT EXISTS idx_attempts_at ON login_attempts(kind, at)",
      "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts)",
      "CREATE INDEX IF NOT EXISTS idx_audit_category ON audit_log(category, ts)",
      "CREATE INDEX IF NOT EXISTS idx_users_role ON users(role_id)",
  };
  for (const auto& sql : statements) {
    if (!Exec(sql, {}, nullptr, error))
      return false;
  }

  const int64_t now = NowSeconds();
  const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> defaults = {
      {"admin", {"Administrator", R"(["*"])"}},
      {"operator", {"Operator", kOperatorPermissions}},
      {"viewer", {"Viewer", kViewerPermissions}},
  };
  for (const auto& [name, meta] : defaults) {
    uvcpp::uvcpp_db_params params;
    params.add(name);
    params.add(meta.first);
    params.add(meta.second);
    params.add(1);
    params.add(now);
    if (!Exec("INSERT OR IGNORE INTO roles(name, display_name, permissions, builtin, created_at) "
              "VALUES(?, ?, ?, ?, ?)",
              params, nullptr, error)) {
      return false;
    }
  }

  const std::vector<std::pair<std::string, std::string>> settings = {
      {"security.web_max_failures", "10"},  {"security.web_lock_minutes", "10"},
      {"security.api_max_failures", "100"}, {"security.api_lock_minutes", "10"},
      {"security.api_locked_until", "0"},   {"audit.retention_days", "180"},
      {"system.timezone", "UTC"},           {"system.language", "auto"},
  };
  for (const auto& [key, value] : settings) {
    uvcpp::uvcpp_db_params params;
    params.add(key);
    params.add(value);
    params.add(now);
    if (!Exec("INSERT OR IGNORE INTO settings(key, value, updated_at) VALUES(?, ?, ?)", params,
              nullptr, error)) {
      return false;
    }
  }

  uvcpp::uvcpp_db_params version;
  version.add(1);
  version.add(now);
  if (!Exec("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(?, ?)", version,
            nullptr, error)) {
    return false;
  }
  return true;
}

bool App::EnsureSerial(std::string* error) {
  uvcpp::uvcpp_db_table table;
  if (!Query("SELECT value FROM system_meta WHERE key = 'serial'", {}, &table, error))
    return false;
  if (!table.empty()) {
    serial_ = TableText(table, 0, "value");
    return !serial_.empty();
  }
  serial_ = "UVNVR-" + RandomHex(6);
  uvcpp::uvcpp_db_params params;
  params.add(serial_);
  if (!Exec("INSERT INTO system_meta(key, value) VALUES('serial', ?)", params, nullptr, error)) {
    return false;
  }
  return true;
}

bool App::Init(std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(config_.data_dir, ec);
  if (ec) {
    if (error != nullptr)
      *error = "cannot create data directory: " + ec.message();
    return false;
  }

  const std::string url = "sqlite://" + config_.db_path;
  const auto status = db_.init(url, 1, 4);
  if (!uvcpp::uvcpp_db_ok(status)) {
    if (error != nullptr) {
      *error = "cannot open " + config_.db_path + ": " + db_.last_error();
    }
    return false;
  }
  db_.set_timeout_ms(5000);

  if (!Migrate(error)) {
    if (error != nullptr)
      *error = "database migration failed: " + *error;
    return false;
  }
  if (!EnsureSerial(error)) {
    if (error != nullptr)
      *error = "cannot initialise serial number: " + *error;
    return false;
  }

  setup_required_ = CountUsers() == 0;
  CleanupAudit();
  return true;
}

void App::CleanupAudit() {
  const int64_t retention_days = std::max<int64_t>(1, GetSettingInt("audit.retention_days", 180));
  const int64_t cutoff = NowSeconds() - retention_days * 86400;
  uvcpp::uvcpp_db_params params;
  params.add(cutoff);
  std::string error;
  if (!Exec("DELETE FROM audit_log WHERE ts < ?", params, nullptr, &error)) {
    std::fprintf(stderr, "audit cleanup failed: %s\n", error.c_str());
  }
}

void App::Audit(const std::string& category, const std::string& level, const std::string& actor,
                const std::string& ip, const std::string& ua, const std::string& object_type,
                const std::string& object_id, const std::string& action, const std::string& result,
                const std::string& error_code, const std::string& detail) {
  uvcpp::uvcpp_db_params params;
  params.add(NowSeconds());
  params.add(category);
  params.add(level);
  params.add(actor);
  params.add(ip);
  params.add(ua);
  params.add(object_type);
  params.add(object_id);
  params.add(action);
  params.add(result);
  params.add(error_code);
  params.add(detail.empty() ? "{}" : detail);
  params.add("");
  std::string error;
  if (!Exec("INSERT INTO audit_log(ts, category, level, actor, ip, user_agent, object_type, "
            "object_id, action, result, error_code, detail, request_id) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            params, nullptr, &error)) {
    std::fprintf(stderr, "audit write failed: %s\n", error.c_str());
  }
}

bool App::PasswordValid(const std::string& password, std::string* message) {
  if (password.size() < 10) {
    if (message != nullptr)
      *message = "password must be at least 10 characters";
    return false;
  }
  if (password.size() > 256) {
    if (message != nullptr)
      *message = "password is too long";
    return false;
  }
  bool lower = false;
  bool upper = false;
  bool digit = false;
  bool symbol = false;
  for (const unsigned char c : password) {
    lower = lower || std::islower(c);
    upper = upper || std::isupper(c);
    digit = digit || std::isdigit(c);
    symbol = symbol || !std::isalnum(c);
  }
  const int classes = static_cast<int>(lower) + static_cast<int>(upper) + static_cast<int>(digit) +
                      static_cast<int>(symbol);
  if (classes < 3) {
    if (message != nullptr) {
      *message = "password must contain at least three of: lower-case, upper-case, digit, symbol";
    }
    return false;
  }
  return true;
}

bool App::HasPermission(const UserContext& user, const std::string& permission) const {
  for (const auto& item : user.permissions) {
    if (item == "*" || item == permission)
      return true;
  }
  return false;
}

json App::UserJson(const UserContext& user) const {
  json permissions = json::array();
  for (const auto& permission : user.permissions)
    permissions.push_back(permission);
  return json{{"id", user.id},
              {"username", user.username},
              {"role", user.role_name},
              {"permissions", permissions}};
}

bool App::CreateSession(int64_t user_id, const std::string& ip, const std::string& ua,
                        std::string* token, std::string* csrf, int64_t* expires_at) {
  *token = RandomHex(32);
  *csrf = RandomHex(32);
  if (token->empty() || csrf->empty())
    return false;
  *expires_at = NowSeconds() + kSessionTtlSeconds;
  const std::string token_hash = Sha256Hex(*token);

  uvcpp::uvcpp_db_params cleanup;
  cleanup.add(NowSeconds());
  std::string error;
  Exec("DELETE FROM sessions WHERE expires_at <= ?", cleanup, nullptr, &error);

  uvcpp::uvcpp_db_params params;
  params.add(token_hash);
  params.add(user_id);
  params.add(*csrf);
  params.add(ip);
  params.add(ua);
  params.add(NowSeconds());
  params.add(*expires_at);
  if (!Exec("INSERT INTO sessions(token_hash, user_id, csrf_token, ip, user_agent, created_at, "
            "expires_at) VALUES(?, ?, ?, ?, ?, ?, ?)",
            params, nullptr, &error)) {
    return false;
  }
  return true;
}

bool App::DeleteSession(const std::string& token_hash) {
  uvcpp::uvcpp_db_params params;
  params.add(token_hash);
  std::string error;
  return Exec("DELETE FROM sessions WHERE token_hash = ?", params, nullptr, &error);
}

bool App::IsWebLocked(int64_t* retry_after) {
  const int64_t until = GetSettingInt("security.web_locked_until", 0);
  const int64_t now = NowSeconds();
  if (until > now) {
    if (retry_after != nullptr)
      *retry_after = until - now;
    return true;
  }
  if (until != 0) {
    std::string error;
    SetSetting("security.web_locked_until", "0", &error);
  }
  return false;
}

void App::ClearWebFailures() {
  std::string error;
  Exec("DELETE FROM login_attempts WHERE kind = 'web' AND success = 0", {}, nullptr, &error);
  SetSetting("security.web_locked_until", "0", &error);
}

bool App::IsApiLocked(int64_t* retry_after) {
  const int64_t until = GetSettingInt("security.api_locked_until", 0);
  const int64_t now = NowSeconds();
  if (until > now) {
    if (retry_after != nullptr)
      *retry_after = until - now;
    return true;
  }
  if (until != 0) {
    std::string error;
    SetSetting("security.api_locked_until", "0", &error);
  }
  return false;
}

bool App::RecordApiFailure(const std::string& ip) {
  uvcpp::uvcpp_db_params attempt;
  attempt.add("api");
  attempt.add("");
  attempt.add(ip);
  attempt.add(0);
  attempt.add(NowSeconds());
  std::string error;
  if (!Exec("INSERT INTO login_attempts(kind, username, ip, success, at) VALUES(?, ?, ?, ?, ?)",
            attempt, nullptr, &error)) {
    return false;
  }
  const int64_t max_failures =
      std::max<int64_t>(1, GetSettingInt("security.api_max_failures", 100));
  const int64_t lock_minutes = std::max<int64_t>(1, GetSettingInt("security.api_lock_minutes", 10));
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params count_params;
  count_params.add(NowSeconds() - lock_minutes * 60);
  if (!Query("SELECT COUNT(*) AS n FROM login_attempts "
             "WHERE kind = 'api' AND success = 0 AND at >= ?",
             count_params, &table, &error)) {
    return false;
  }
  const int64_t failures = table.empty() ? 0 : TableInt(table, 0, "n");
  if (failures >= max_failures) {
    SetSetting("security.api_locked_until", std::to_string(NowSeconds() + lock_minutes * 60),
               &error);
    return true;
  }
  return false;
}

void App::ClearApiFailures() {
  std::string error;
  Exec("DELETE FROM login_attempts WHERE kind = 'api' AND success = 0", {}, nullptr, &error);
  SetSetting("security.api_locked_until", "0", &error);
}

bool App::RecordLoginFailure(const std::string& username, const std::string& ip) {
  uvcpp::uvcpp_db_params attempt;
  attempt.add("web");
  attempt.add(username);
  attempt.add(ip);
  attempt.add(0);
  attempt.add(NowSeconds());
  std::string error;
  if (!Exec("INSERT INTO login_attempts(kind, username, ip, success, at) VALUES(?, ?, ?, ?, ?)",
            attempt, nullptr, &error)) {
    return false;
  }

  const int64_t max_failures = std::max<int64_t>(1, GetSettingInt("security.web_max_failures", 10));
  const int64_t lock_minutes = std::max<int64_t>(1, GetSettingInt("security.web_lock_minutes", 10));
  const int64_t window_start = NowSeconds() - lock_minutes * 60;

  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params count_params;
  count_params.add(window_start);
  if (!Query("SELECT COUNT(*) AS n FROM login_attempts "
             "WHERE kind = 'web' AND success = 0 AND at >= ?",
             count_params, &table, &error)) {
    return false;
  }
  const int64_t failures = table.empty() ? 0 : TableInt(table, 0, "n");
  if (failures >= max_failures) {
    SetSetting("security.web_locked_until", std::to_string(NowSeconds() + lock_minutes * 60),
               &error);
    return true;
  }
  return false;
}

LoginResult App::Login(const std::string& username, const std::string& password,
                       const std::string& ip, const std::string& ua) {
  LoginResult result;
  int64_t retry_after = 0;
  if (IsWebLocked(&retry_after)) {
    result.status = 429;
    result.code = "locked";
    result.message = "too many failed sign-in attempts; the recorder is temporarily locked";
    result.retry_after_seconds = static_cast<int>(retry_after);
    return result;
  }

  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(username);
  std::string error;
  const bool queried =
      Query("SELECT u.id, u.username, u.password_hash, u.enabled, r.name AS role_name, "
            "r.permissions AS permissions FROM users u JOIN roles r ON r.id = u.role_id "
            "WHERE u.username = ? COLLATE NOCASE",
            params, &table, &error);

  if (!queried || table.empty()) {
    const bool locked = RecordLoginFailure(username, ip);
    Audit("auth", "warning", username, ip, ua, "session", "", "login", "failure",
          "invalid_credentials", locked ? R"({"locked":true})" : "{}");
    result.status = 401;
    result.code = "invalid_credentials";
    result.message = "invalid username or password";
    return result;
  }

  const bool enabled = TableInt(table, 0, "enabled") != 0;
  const std::string stored_hash = TableText(table, 0, "password_hash");
  if (!enabled) {
    Audit("auth", "warning", username, ip, ua, "session", "", "login", "failure",
          "account_disabled", "{}");
    result.status = 403;
    result.code = "account_disabled";
    result.message = "this account is disabled";
    return result;
  }
  if (!VerifyPassword(password, stored_hash)) {
    const bool locked = RecordLoginFailure(username, ip);
    Audit("auth", "warning", username, ip, ua, "session", "", "login", "failure",
          "invalid_credentials", locked ? R"({"locked":true})" : "{}");
    result.status = 401;
    result.code = "invalid_credentials";
    result.message = "invalid username or password";
    return result;
  }

  ClearWebFailures();
  UserContext user;
  user.id = TableInt(table, 0, "id");
  user.username = TableText(table, 0, "username");
  user.role_name = TableText(table, 0, "role_name");
  user.permissions = ParsePermissions(TableText(table, 0, "permissions"));

  std::string token;
  std::string csrf;
  int64_t expires_at = 0;
  if (!CreateSession(user.id, ip, ua, &token, &csrf, &expires_at)) {
    result.status = 500;
    result.code = "session_failed";
    result.message = "could not create a session";
    Audit("auth", "error", user.username, ip, ua, "session", "", "login", "failure",
          "session_failed", "{}");
    return result;
  }

  uvcpp::uvcpp_db_params update;
  update.add(NowSeconds());
  update.add(user.id);
  Exec("UPDATE users SET last_login_at = ? WHERE id = ?", update, nullptr, &error);

  Audit("auth", "info", user.username, ip, ua, "session", "", "login", "success", "",
        json{{"role", user.role_name}}.dump());

  result.ok = true;
  result.status = 200;
  result.user = std::move(user);
  result.csrf = std::move(csrf);
  result.session_token = std::move(token);
  result.expires_at = expires_at;
  return result;
}

LoginResult App::Setup(const std::string& password, const std::string& timezone,
                       const std::string& ip, const std::string& ua) {
  LoginResult result;
  if (CountUsers() != 0) {
    result.status = 409;
    result.code = "already_setup";
    result.message = "the recorder has already been set up";
    return result;
  }

  std::string password_error;
  if (!PasswordValid(password, &password_error)) {
    result.status = 400;
    result.code = "weak_password";
    result.message = password_error;
    return result;
  }

  std::string password_hash;
  if (!HashPassword(password, &password_hash)) {
    result.status = 500;
    result.code = "hash_failed";
    result.message = "could not hash the password";
    return result;
  }

  const int64_t role_id = AdminRoleId();
  if (role_id == 0) {
    result.status = 500;
    result.code = "missing_role";
    result.message = "the built-in administrator role is missing";
    return result;
  }

  uvcpp::uvcpp_db_client* client = db_.acquire();
  if (client == nullptr) {
    result.status = 503;
    result.code = "database_busy";
    result.message = "the database is busy";
    return result;
  }

  const int64_t now = NowSeconds();
  uvcpp::uvcpp_db_value new_id;
  uvcpp::uvcpp_db_params user_params;
  user_params.add("admin");
  user_params.add(password_hash);
  user_params.add(role_id);
  user_params.add(1);
  user_params.add(now);

  std::string error;
  bool ok = uvcpp::uvcpp_db_ok(client->begin());
  uvcpp::uvcpp_db_status status;
  if (ok) {
    status =
        client->insert("INSERT INTO users(username, password_hash, role_id, enabled, created_at) "
                       "VALUES(?, ?, ?, ?, ?)",
                       user_params, &new_id);
    ok = uvcpp::uvcpp_db_ok(status);
    if (!ok)
      error = client->last_error();
  }
  if (ok) {
    uvcpp::uvcpp_db_params tz;
    tz.add(timezone.empty() ? "UTC" : timezone);
    tz.add(now);
    status = client->execute(
        "INSERT INTO settings(key, value, updated_at) VALUES('system.timezone', ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
        tz);
    ok = uvcpp::uvcpp_db_ok(status);
    if (!ok)
      error = client->last_error();
  }
  if (ok) {
    status = client->commit();
    ok = uvcpp::uvcpp_db_ok(status);
    if (!ok)
      error = client->last_error();
  }
  if (!ok) {
    client->rollback();
    db_.release(client);
    result.status = 500;
    result.code = "setup_failed";
    result.message = "could not create the administrator account";
    Audit("auth", "error", "admin", ip, ua, "user", "", "setup", "failure", "setup_failed",
          json{{"error", error}}.dump());
    return result;
  }
  db_.release(client);
  setup_required_ = false;

  const int64_t user_id = new_id.to_int64();
  UserContext user;
  user.id = user_id;
  user.username = "admin";
  user.role_name = "admin";
  user.permissions = {"*"};

  std::string token;
  std::string csrf;
  int64_t expires_at = 0;
  if (!CreateSession(user_id, ip, ua, &token, &csrf, &expires_at)) {
    result.status = 500;
    result.code = "session_failed";
    result.message = "administrator created, but a session could not be created";
    return result;
  }

  Audit("auth", "info", "admin", ip, ua, "user", std::to_string(user_id), "setup", "success", "",
        json{{"timezone", timezone.empty() ? "UTC" : timezone}}.dump());

  result.ok = true;
  result.status = 200;
  result.user = std::move(user);
  result.csrf = std::move(csrf);
  result.session_token = std::move(token);
  result.expires_at = expires_at;
  return result;
}

AuthResult App::Authenticate(const std::string& cookie_token, const std::string& bearer_token,
                             const std::string& csrf_header, bool mutating, const std::string& ip,
                             const std::string& ua) {
  AuthResult result;
  result.status = 401;
  result.code = "unauthorized";
  result.message = "authentication required";

  if (!bearer_token.empty()) {
    int64_t retry_after = 0;
    if (IsApiLocked(&retry_after)) {
      result.status = 429;
      result.code = "locked";
      result.message = "too many failed API authentication attempts; the API is temporarily locked";
      return result;
    }
    const std::string token_hash = Sha256Hex(bearer_token);
    uvcpp::uvcpp_db_table table;
    uvcpp::uvcpp_db_params params;
    params.add(token_hash);
    params.add(NowSeconds());
    std::string error;
    const bool queried = Query(
        "SELECT u.id, u.username, u.enabled, r.name AS role_name, r.permissions AS permissions, "
        "t.id AS token_id FROM api_tokens t JOIN users u ON u.id = t.user_id "
        "JOIN roles r ON r.id = u.role_id WHERE t.token_hash = ? "
        "AND (t.expires_at IS NULL OR t.expires_at > ?)",
        params, &table, &error);
    if (!queried || table.empty() || TableInt(table, 0, "enabled") == 0) {
      RecordApiFailure(ip);
      return result;
    }
    UserContext user;
    user.id = TableInt(table, 0, "id");
    user.username = TableText(table, 0, "username");
    user.role_name = TableText(table, 0, "role_name");
    user.permissions = ParsePermissions(TableText(table, 0, "permissions"));
    if (!HasPermission(user, "api.access")) {
      result.status = 403;
      result.code = "forbidden";
      result.message = "this token does not have api.access";
      return result;
    }
    ClearApiFailures();
    uvcpp::uvcpp_db_params touch;
    touch.add(NowSeconds());
    touch.add(TableInt(table, 0, "token_id"));
    Exec("UPDATE api_tokens SET last_used_at = ? WHERE id = ?", touch, nullptr, &error);
    result.ok = true;
    result.status = 200;
    result.user = std::move(user);
    return result;
  }

  if (cookie_token.empty())
    return result;
  const std::string token_hash = Sha256Hex(cookie_token);
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(token_hash);
  params.add(NowSeconds());
  std::string error;
  const bool queried = Query(
      "SELECT u.id, u.username, u.enabled, r.name AS role_name, r.permissions AS permissions, "
      "s.csrf_token FROM sessions s JOIN users u ON u.id = s.user_id "
      "JOIN roles r ON r.id = u.role_id WHERE s.token_hash = ? AND s.expires_at > ?",
      params, &table, &error);
  if (!queried || table.empty() || TableInt(table, 0, "enabled") == 0) {
    return result;
  }

  const std::string csrf = TableText(table, 0, "csrf_token");
  if (mutating && (csrf_header.empty() || csrf_header != csrf)) {
    result.status = 403;
    result.code = "csrf_failed";
    result.message = "missing or invalid CSRF token";
    return result;
  }

  UserContext user;
  user.id = TableInt(table, 0, "id");
  user.username = TableText(table, 0, "username");
  user.role_name = TableText(table, 0, "role_name");
  user.permissions = ParsePermissions(TableText(table, 0, "permissions"));
  user.session_hash = token_hash;
  result.ok = true;
  result.status = 200;
  result.user = std::move(user);
  result.csrf = csrf;
  (void)ip;
  (void)ua;
  return result;
}

json App::BuildSummary() {
  json result;

  char hostname[256] = {0};
  size_t hostname_len = sizeof(hostname);
  if (uv_os_gethostname(hostname, &hostname_len) != 0)
    hostname[0] = '\0';

  uv_utsname_t uname{};
  std::string os_name = "unknown";
  if (uv_os_uname(&uname) == 0) {
    os_name = std::string(uname.sysname) + " " + uname.release;
  }

  double uptime = 0;
  uv_uptime(&uptime);

  const uint64_t total_memory = uv_get_total_memory();
  const uint64_t free_memory = uv_get_free_memory();
  const uint64_t used_memory = total_memory > free_memory ? total_memory - free_memory : 0;
  const double memory_percent =
      total_memory == 0 ? 0.0 : (100.0 * static_cast<double>(used_memory) / total_memory);

  uv_cpu_info_t* cpu_info = nullptr;
  int cpu_count = 0;
  uint64_t total_ticks = 0;
  uint64_t idle_ticks = 0;
  if (uv_cpu_info(&cpu_info, &cpu_count) == 0) {
    for (int i = 0; i < cpu_count; ++i) {
      const auto& times = cpu_info[i].cpu_times;
      total_ticks += times.user + times.nice + times.sys + times.idle + times.irq;
      idle_ticks += times.idle;
    }
    uv_free_cpu_info(cpu_info, cpu_count);
  }

  double cpu_percent = 0.0;
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    if (last_cpu_total_ == 0 && total_ticks > 0) {
      // First sample: report the average since boot rather than a misleading 0%.
      cpu_percent = 100.0 * (1.0 - static_cast<double>(idle_ticks) / total_ticks);
    } else if (last_cpu_total_ != 0 && total_ticks > last_cpu_total_) {
      const uint64_t delta_total = total_ticks - last_cpu_total_;
      const uint64_t delta_idle = idle_ticks >= last_cpu_idle_ ? idle_ticks - last_cpu_idle_ : 0;
      cpu_percent = 100.0 * (1.0 - static_cast<double>(delta_idle) / delta_total);
    }
    cpu_percent = std::clamp(cpu_percent, 0.0, 100.0);
    last_cpu_total_ = total_ticks;
    last_cpu_idle_ = idle_ticks;
  }

  uint64_t disk_total = 0;
  uint64_t disk_free = 0;
#ifdef _WIN32
  ULARGE_INTEGER free_bytes{};
  ULARGE_INTEGER total_bytes{};
  if (GetDiskFreeSpaceExA(config_.data_dir.c_str(), &free_bytes, &total_bytes, nullptr)) {
    disk_total = total_bytes.QuadPart;
    disk_free = free_bytes.QuadPart;
  }
#else
  struct statvfs stat {};
  if (statvfs(config_.data_dir.c_str(), &stat) == 0) {
    disk_total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
    disk_free = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
  }
#endif
  const uint64_t disk_used = disk_total > disk_free ? disk_total - disk_free : 0;
  const double disk_percent =
      disk_total == 0 ? 0.0 : (100.0 * static_cast<double>(disk_used) / disk_total);

  uvcpp::uvcpp_db_table users;
  uvcpp::uvcpp_db_table sessions;
  uvcpp::uvcpp_db_table audit;
  std::string error;
  Query("SELECT COUNT(*) AS n FROM users", {}, &users, &error);
  uvcpp::uvcpp_db_params session_params;
  session_params.add(NowSeconds());
  Query("SELECT COUNT(*) AS n FROM sessions WHERE expires_at > ?", session_params, &sessions,
        &error);
  Query("SELECT COUNT(*) AS n FROM audit_log", {}, &audit, &error);

  result["version"] = UVNVR_VERSION;
  result["commit"] = UVNVR_GIT_SHA;
  result["serial"] = serial_;
  result["hostname"] = hostname;
  result["os"] = os_name;
  result["uptimeSeconds"] = static_cast<int64_t>(uptime);
  result["setupRequired"] = setup_required_.load();
  result["maintenance"] = config_.maintenance;
  result["cpu"] = json{
      {"cores", cpu_count > 0 ? cpu_count : static_cast<int>(std::thread::hardware_concurrency())},
      {"usagePercent", cpu_percent}};
  result["memory"] = json{{"totalBytes", total_memory},
                          {"usedBytes", used_memory},
                          {"freeBytes", free_memory},
                          {"usagePercent", memory_percent}};
  result["disk"] = json{{"path", config_.data_dir},
                        {"totalBytes", disk_total},
                        {"usedBytes", disk_used},
                        {"freeBytes", disk_free},
                        {"usagePercent", disk_percent}};
  result["counts"] = json{{"users", users.empty() ? 0 : TableInt(users, 0, "n")},
                          {"sessions", sessions.empty() ? 0 : TableInt(sessions, 0, "n")},
                          {"audit", audit.empty() ? 0 : TableInt(audit, 0, "n")},
                          {"channels", 0},
                          {"recordings", 0}};
  return result;
}

void App::RegisterMiddleware() {
  web_.use(
      [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp, uvcpp::uvcpp_web_next next) {
        resp.set_header("X-Content-Type-Options", "nosniff");
        resp.set_header("X-Frame-Options", "DENY");
        resp.set_header("Referrer-Policy", "no-referrer");
        resp.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
        resp.set_header("Content-Security-Policy",
                        "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                        "img-src 'self' data:; font-src 'self'; connect-src 'self' ws: wss:; "
                        "object-src 'none'; base-uri 'self'; frame-ancestors 'none'");
        next();
      });
  web_.use(uvcpp::web_middleware_request_id("X-Request-Id", false));
  web_.use(uvcpp::web_middleware_access_log());
}

void App::RegisterStatic() {
  std::ifstream entry("./frontend/dist/index.html");
  if (entry.good()) {
    uvcpp::uvcpp_web_static_options options;
    options.spa_fallback = true;
    web_.serve_static("/", "./frontend/dist", options);
  } else {
    web_.get("/",
             [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp, uvcpp::uvcpp_web_next) {
               resp.status(200).body(
                   "UvNVR is running. The frontend has not been built yet.\n"
                   "Run `npm ci && npm run build` in frontend/, then restart the service.\n",
                   "text/plain; charset=utf-8");
               resp.end();
             });
  }
}

namespace {

UserContext UserFromRequest(const uvcpp::uvcpp_web_request& req) {
  UserContext user;
  if (const std::string* id = req.param("__uvnvr_user_id")) {
    const auto result = std::from_chars(id->data(), id->data() + id->size(), user.id);
    if (result.ec != std::errc())
      user.id = 0;
  }
  if (const std::string* name = req.param("__uvnvr_username"))
    user.username = *name;
  if (const std::string* role = req.param("__uvnvr_role"))
    user.role_name = *role;
  if (const std::string* session = req.param("__uvnvr_session"))
    user.session_hash = *session;
  if (const std::string* permissions = req.param("__uvnvr_permissions")) {
    user.permissions = Split(*permissions, ',');
  }
  return user;
}

std::string RequestCsrf(const uvcpp::uvcpp_web_request& req) {
  if (const std::string* csrf = req.param("__uvnvr_csrf"))
    return *csrf;
  return {};
}

void SetSessionCookie(uvcpp::uvcpp_web_response& resp, const std::string& token, int64_t expires_at,
                      bool secure) {
  const int64_t max_age = std::max<int64_t>(0, expires_at - NowSeconds());
  resp.set_cookie("uvnvr_session", token, "/", static_cast<long>(max_age), true, secure, "Strict");
}

json LoginJson(const LoginResult& result) {
  json user = json{
      {"id", result.user.id}, {"username", result.user.username}, {"role", result.user.role_name}};
  json permissions = json::array();
  for (const auto& permission : result.user.permissions)
    permissions.push_back(permission);
  user["permissions"] = permissions;
  return json{{"user", user}, {"csrfToken", result.csrf}, {"expiresAt", result.expires_at}};
}

} // namespace

uvcpp::uvcpp_web_handler App::Protected(uvcpp::uvcpp_web_handler handler,
                                        const std::string& permission) {
  return [this, handler = std::move(handler), permission](uvcpp::uvcpp_web_request& req,
                                                          uvcpp::uvcpp_web_response& resp,
                                                          uvcpp::uvcpp_web_next next) {
    std::string cookie_token;
    if (const std::string* cookie = req.cookie("uvnvr_session"))
      cookie_token = *cookie;
    std::string bearer_token;
    const std::string authorization = req.header("Authorization", "");
    if (authorization.rfind("Bearer ", 0) == 0)
      bearer_token = authorization.substr(7);
    const std::string csrf = req.header("X-CSRF-Token", "");
    const bool mutating = req.method() != uvcpp::http_method::HTTP_GET &&
                          req.method() != uvcpp::http_method::HTTP_HEAD &&
                          req.method() != uvcpp::http_method::HTTP_OPTIONS;
    const std::string ip = req.peer_ip();
    const std::string ua = req.header("User-Agent", "");
    auto result = std::make_shared<AuthResult>();

    RunWork(
        [this, cookie_token, bearer_token, csrf, mutating, ip, ua, result] {
          *result = Authenticate(cookie_token, bearer_token, csrf, mutating, ip, ua);
        },
        [this, &req, &resp, next, handler, permission, result] {
          if (!result->ok) {
            SendError(resp, result->status, result->code, result->message);
            next();
            return;
          }
          if (!permission.empty() && !HasPermission(result->user, permission)) {
            SendError(resp, 403, "forbidden", "this account does not have the required permission");
            next();
            return;
          }
          req.set_param("__uvnvr_user_id", std::to_string(result->user.id));
          req.set_param("__uvnvr_username", result->user.username);
          req.set_param("__uvnvr_role", result->user.role_name);
          req.set_param("__uvnvr_session", result->user.session_hash);
          req.set_param("__uvnvr_permissions", Join(result->user.permissions, ","));
          req.set_param("__uvnvr_csrf", result->csrf);
          handler(req, resp, next);
        });
  };
}

int App::Run() {
  std::string error;
  if (!Init(&error)) {
    std::fprintf(stderr, "UvNVR init failed: %s\n", error.c_str());
    return 1;
  }

  web_.set_port(config_.port);
  web_.set_h2c_enabled(true);
  bool tls_enabled = false;
  if (!config_.tls_cert.empty() && !config_.tls_key.empty()) {
    web_.enable_ssl(config_.tls_cert, config_.tls_key);
    if (!web_.ssl_error().empty()) {
      std::fprintf(stderr, "cannot enable TLS: %s\n", web_.ssl_error().c_str());
      return 1;
    }
    tls_enabled = true;
  } else if (config_.tls_self_signed) {
    web_.enable_self_signed("localhost");
    if (!web_.ssl_error().empty()) {
      std::fprintf(stderr, "cannot enable the generated certificate: %s\n",
                   web_.ssl_error().c_str());
      return 1;
    }
    tls_enabled = true;
  }

  RegisterMiddleware();
  RegisterPublicRoutes();
  RegisterProtectedRoutes();
  RegisterStatic();

  const int rc = web_.start();
  if (rc != 0) {
    std::fprintf(stderr, "failed to start on port %d: libuv error %d\n", config_.port, rc);
    return 1;
  }

  std::printf("UvNVR %s (%s) listening on %s://127.0.0.1:%d\n", UVNVR_VERSION, UVNVR_GIT_SHA,
              tls_enabled ? "https" : "http", web_.bound_port());
  std::printf("setup required: %s\n", setup_required_.load() ? "yes" : "no");
  std::fflush(stdout);
  web_.join();
  return 0;
}

void App::RegisterPublicRoutes() {
  web_.get("/healthz", [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                              uvcpp::uvcpp_web_next next) {
    SendJson(resp, 200,
             json{{"status", "ok"},
                  {"name", UVNVR_PRODUCT_NAME},
                  {"version", UVNVR_VERSION},
                  {"commit", UVNVR_GIT_SHA}});
    next();
  });

  web_.get("/api/v1/system/info", [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                                         uvcpp::uvcpp_web_next next) {
    SendJson(resp, 200,
             json{{"name", UVNVR_PRODUCT_NAME},
                  {"version", UVNVR_VERSION},
                  {"commit", UVNVR_GIT_SHA},
                  {"status", "ok"}});
    next();
  });

  web_.get("/api/v1/setup/status",
           [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                  uvcpp::uvcpp_web_next next) {
             SendJson(resp, 200,
                      json{{"setupRequired", setup_required_.load()},
                           {"version", UVNVR_VERSION},
                           {"serial", serial_}});
             next();
           });

  web_.post("/api/v1/setup", [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                                    uvcpp::uvcpp_web_next next) {
    const json body = json::parse(req.body_str(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
      SendError(resp, 400, "invalid_json", "request body must be a JSON object");
      next();
      return;
    }
    const std::string password = body.value("password", "");
    const std::string confirm = body.value("confirmPassword", "");
    const std::string timezone = body.value("timezone", "UTC");
    if (password.empty()) {
      SendError(resp, 400, "missing_password", "password is required");
      next();
      return;
    }
    if (password != confirm) {
      SendError(resp, 400, "password_mismatch", "the two passwords do not match");
      next();
      return;
    }
    const std::string ip = req.peer_ip();
    const std::string ua = req.header("User-Agent", "");
    auto result = std::make_shared<LoginResult>();
    RunWork(
        [this, password, timezone, ip, ua, result] { *result = Setup(password, timezone, ip, ua); },
        [this, &resp, next, result] {
          if (!result->ok) {
            SendError(resp, result->status, result->code, result->message);
            next();
            return;
          }
          SetSessionCookie(resp, result->session_token, result->expires_at,
                           config_.tls_self_signed || !config_.tls_cert.empty());
          SendJson(resp, 200, LoginJson(*result));
          next();
        });
  });

  web_.post("/api/v1/auth/login", [this](uvcpp::uvcpp_web_request& req,
                                         uvcpp::uvcpp_web_response& resp,
                                         uvcpp::uvcpp_web_next next) {
    const json body = json::parse(req.body_str(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
      SendError(resp, 400, "invalid_json", "request body must be a JSON object");
      next();
      return;
    }
    const std::string username = body.value("username", "");
    const std::string password = body.value("password", "");
    if (username.empty() || password.empty()) {
      SendError(resp, 400, "missing_credentials", "username and password are required");
      next();
      return;
    }
    const std::string ip = req.peer_ip();
    const std::string ua = req.header("User-Agent", "");
    auto result = std::make_shared<LoginResult>();
    RunWork(
        [this, username, password, ip, ua, result] { *result = Login(username, password, ip, ua); },
        [this, &resp, next, result] {
          if (!result->ok) {
            if (result->retry_after_seconds > 0) {
              resp.set_header("Retry-After", std::to_string(result->retry_after_seconds));
            }
            SendError(resp, result->status, result->code, result->message);
            next();
            return;
          }
          SetSessionCookie(resp, result->session_token, result->expires_at,
                           config_.tls_self_signed || !config_.tls_cert.empty());
          SendJson(resp, 200, LoginJson(*result));
          next();
        });
  });
}

namespace {

std::string JsonString(const json& body, const char* key, const std::string& fallback = {}) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_string())
    return fallback;
  return it->get<std::string>();
}

int64_t JsonInt(const json& body, const char* key, int64_t fallback = 0) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_number_integer())
    return fallback;
  return it->get<int64_t>();
}

bool JsonBool(const json& body, const char* key, bool fallback) {
  const auto it = body.find(key);
  if (it == body.end() || !it->is_boolean())
    return fallback;
  return it->get<bool>();
}

bool ValidUsername(const std::string& name) {
  static const std::regex pattern(R"(^[A-Za-z0-9_.-]{3,32}$)");
  return std::regex_match(name, pattern);
}

bool ValidRoleName(const std::string& name) {
  static const std::regex pattern(R"(^[a-z][a-z0-9_.-]{1,31}$)");
  return std::regex_match(name, pattern);
}

bool IsUniqueError(const std::string& error) {
  return error.find("UNIQUE") != std::string::npos || error.find("unique") != std::string::npos;
}

bool RoleExists(uvcpp::uvcpp_db_pool& db, int64_t role_id, std::string* name = nullptr) {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(role_id);
  std::string error;
  const auto status = db.query("SELECT name FROM roles WHERE id = ?", params, &table);
  if (!uvcpp::uvcpp_db_ok(status) || table.empty())
    return false;
  if (name != nullptr)
    *name = table.at(0, "name").to_text();
  return true;
}

int64_t EnabledAdminCount(uvcpp::uvcpp_db_pool& db) {
  uvcpp::uvcpp_db_table table;
  std::string error;
  const auto status = db.query("SELECT COUNT(*) AS n FROM users u JOIN roles r ON r.id = u.role_id "
                               "WHERE r.name = 'admin' AND u.enabled = 1",
                               {}, &table);
  if (!uvcpp::uvcpp_db_ok(status) || table.empty())
    return 0;
  return table.at(0, "n").to_int64();
}

} // namespace

void App::RegisterCoreRoutes() {
  web_.get("/api/v1/auth/me",
           Protected([this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                            uvcpp::uvcpp_web_next next) {
             const UserContext user = UserFromRequest(req);
             SendJson(resp, 200, json{{"user", UserJson(user)}, {"csrfToken", RequestCsrf(req)}});
             next();
           }));

  web_.post("/api/v1/auth/logout",
            Protected([this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                             uvcpp::uvcpp_web_next next) {
              const UserContext user = UserFromRequest(req);
              const std::string ip = req.peer_ip();
              const std::string ua = req.header("User-Agent", "");
              const std::string session_hash = user.session_hash;
              RunWork(
                  [this, user, ip, ua, session_hash] {
                    if (!session_hash.empty())
                      DeleteSession(session_hash);
                    Audit("auth", "info", user.username, ip, ua, "session", "", "logout", "success",
                          "", "{}");
                  },
                  [&resp, next] {
                    resp.clear_cookie("uvnvr_session");
                    SendJson(resp, 200, json{{"ok", true}});
                    next();
                  });
            }));

  web_.get("/api/v1/dashboard/summary",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto summary = std::make_shared<json>();
                 RunWork([this, summary] { *summary = BuildSummary(); },
                         [summary, &resp, next] {
                           SendJson(resp, 200, *summary);
                           next();
                         });
               },
               "dashboard.view"));

  web_.get("/api/v1/system/overview",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto summary = std::make_shared<json>();
                 RunWork([this, summary] { *summary = BuildSummary(); },
                         [summary, &resp, next] {
                           SendJson(resp, 200, *summary);
                           next();
                         });
               },
               "system.view"));

  web_.get("/api/v1/settings",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto settings = std::make_shared<json>();
                 RunWork([this, settings] { *settings = SettingsJson(); },
                         [settings, &resp, next] {
                           SendJson(resp, 200, *settings);
                           next();
                         });
               },
               "system.view"));

  web_.put("/api/v1/settings",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const json body = json::parse(req.body_str(), nullptr, false);
                 if (body.is_discarded() || !body.is_object()) {
                   SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                   next();
                   return;
                 }
                 const UserContext user = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto error = std::make_shared<std::string>();
                 auto settings = std::make_shared<json>();
                 RunWork(
                     [this, body, user, ip, ua, error, settings] {
                       auto update_int = [&](const char* group, const char* key, int64_t min,
                                             int64_t max, const char* setting_key) -> bool {
                         const auto group_it = body.find(group);
                         if (group_it == body.end() || !group_it->is_object())
                           return true;
                         const auto it = group_it->find(key);
                         if (it == group_it->end())
                           return true;
                         if (!it->is_number_integer()) {
                           *error = std::string(key) + " must be an integer";
                           return false;
                         }
                         const int64_t value = it->get<int64_t>();
                         if (value < min || value > max) {
                           *error = std::string(key) + " is out of range";
                           return false;
                         }
                         std::string db_error;
                         if (!SetSetting(setting_key, std::to_string(value), &db_error)) {
                           *error = db_error;
                           return false;
                         }
                         return true;
                       };
                       if (!update_int("security", "webMaxFailures", 1, 1000,
                                       "security.web_max_failures") ||
                           !update_int("security", "webLockMinutes", 1, 1440,
                                       "security.web_lock_minutes") ||
                           !update_int("security", "apiMaxFailures", 1, 100000,
                                       "security.api_max_failures") ||
                           !update_int("security", "apiLockMinutes", 1, 1440,
                                       "security.api_lock_minutes") ||
                           !update_int("audit", "retentionDays", 1, 3650, "audit.retention_days")) {
                         return;
                       }

                       if (body.contains("system") && body["system"].is_object()) {
                         const auto& system = body["system"];
                         if (system.contains("timezone")) {
                           if (!system["timezone"].is_string() ||
                               system["timezone"].get<std::string>().size() > 64) {
                             *error = "timezone must be a string of at most 64 characters";
                             return;
                           }
                           std::string db_error;
                           if (!SetSetting("system.timezone", system["timezone"].get<std::string>(),
                                           &db_error)) {
                             *error = db_error;
                             return;
                           }
                         }
                         if (system.contains("language")) {
                           const std::string language = system["language"].is_string()
                                                            ? system["language"].get<std::string>()
                                                            : "";
                           if (language != "auto" && language != "en-US" && language != "zh-CN") {
                             *error = "language must be auto, en-US or zh-CN";
                             return;
                           }
                           std::string db_error;
                           if (!SetSetting("system.language", language, &db_error)) {
                             *error = db_error;
                             return;
                           }
                         }
                       }
                       *settings = SettingsJson();
                       Audit("system", "info", user.username, ip, ua, "settings", "", "update",
                             "success", "", body.dump());
                     },
                     [error, settings, &resp, next] {
                       if (!error->empty()) {
                         SendError(resp, 400, "invalid_settings", *error);
                       } else {
                         SendJson(resp, 200, *settings);
                       }
                       next();
                     });
               },
               "system.manage"));
}

json App::SettingsJson() {
  return json{
      {"security", json{{"webMaxFailures", GetSettingInt("security.web_max_failures", 10)},
                        {"webLockMinutes", GetSettingInt("security.web_lock_minutes", 10)},
                        {"apiMaxFailures", GetSettingInt("security.api_max_failures", 100)},
                        {"apiLockMinutes", GetSettingInt("security.api_lock_minutes", 10)}}},
      {"audit", json{{"retentionDays", GetSettingInt("audit.retention_days", 180)}}},
      {"system", json{{"timezone", GetSetting("system.timezone", "UTC")},
                      {"language", GetSetting("system.language", "auto")}}}};
}

namespace {

struct OpResult {
  bool ok = true;
  int status = 200;
  std::string code;
  std::string message;
  json data = json::object();
};

void SendOp(uvcpp::uvcpp_web_response& resp, const OpResult& result) {
  if (result.ok) {
    SendJson(resp, result.status, result.data);
  } else {
    SendError(resp, result.status, result.code, result.message);
  }
}

} // namespace

void App::RegisterProtectedRoutes() {
  RegisterCoreRoutes();

  // Users -----------------------------------------------------------------
  web_.get(
      "/api/v1/users",
      Protected(
          [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            auto items = std::make_shared<json>(json::array());
            RunWork(
                [this, items] {
                  uvcpp::uvcpp_db_table table;
                  std::string error;
                  if (!Query("SELECT u.id, u.username, u.role_id, r.name AS role_name, u.enabled, "
                             "u.created_at, u.last_login_at FROM users u "
                             "JOIN roles r ON r.id = u.role_id ORDER BY u.id",
                             {}, &table, &error)) {
                    return;
                  }
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    items->push_back(json{
                        {"id", TableInt(table, i, "id")},
                        {"username", TableText(table, i, "username")},
                        {"roleId", TableInt(table, i, "role_id")},
                        {"role", TableText(table, i, "role_name")},
                        {"enabled", TableInt(table, i, "enabled") != 0},
                        {"createdAt", TableInt(table, i, "created_at")},
                        {"lastLoginAt", TableInt(table, i, "last_login_at")},
                    });
                  }
                },
                [items, &resp, next] {
                  SendJson(resp, 200, *items);
                  next();
                });
          },
          "user.view"));

  web_.post(
      "/api/v1/users",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const std::string username = JsonString(body, "username");
            const std::string password = JsonString(body, "password");
            const int64_t role_id = JsonInt(body, "roleId", 0);
            const bool enabled = JsonBool(body, "enabled", true);
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            if (!ValidUsername(username)) {
              SendError(
                  resp, 400, "invalid_username",
                  "username must be 3-32 characters: letters, digits, dot, dash or underscore");
              next();
              return;
            }
            std::string password_error;
            if (!PasswordValid(password, &password_error)) {
              SendError(resp, 400, "weak_password", password_error);
              next();
              return;
            }
            if (role_id <= 0) {
              SendError(resp, 400, "invalid_role", "roleId is required");
              next();
              return;
            }
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, username, password, role_id, enabled, actor, ip, ua, op] {
                  std::string role_name;
                  if (!RoleExists(db_, role_id, &role_name)) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_role";
                    op->message = "the selected role does not exist";
                    return;
                  }
                  std::string password_hash;
                  if (!HashPassword(password, &password_hash)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "hash_failed";
                    op->message = "could not hash the password";
                    return;
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(username);
                  params.add(password_hash);
                  params.add(role_id);
                  params.add(enabled ? 1 : 0);
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert("INSERT INTO users(username, password_hash, "
                                                     "role_id, enabled, created_at) "
                                                     "VALUES(?, ?, ?, ?, ?)",
                                                     params, &new_id))) {
                    op->ok = false;
                    op->status = IsUniqueError(db_.last_error()) ? 409 : 500;
                    op->code = op->status == 409 ? "username_taken" : "database_error";
                    op->message = op->status == 409 ? "that username is already in use"
                                                    : "the user could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("user", "info", actor.username, ip, ua, "user", std::to_string(id),
                        "create", "success", "",
                        json{{"username", username}, {"role", role_name}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id},
                                  {"username", username},
                                  {"roleId", role_id},
                                  {"role", role_name},
                                  {"enabled", enabled}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "user.manage"));

  web_.put(
      "/api/v1/users/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t user_id = 0;
            if (!ParseIntParam(id_param, &user_id) || user_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid user id");
              next();
              return;
            }
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const bool has_role = body.contains("roleId");
            const bool has_enabled = body.contains("enabled");
            const bool has_password = body.contains("password");
            const int64_t role_id = JsonInt(body, "roleId", 0);
            const bool enabled = JsonBool(body, "enabled", true);
            const std::string password = JsonString(body, "password");
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            std::string password_error;
            if (has_password && !PasswordValid(password, &password_error)) {
              SendError(resp, 400, "weak_password", password_error);
              next();
              return;
            }
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, user_id, has_role, has_enabled, has_password, role_id, enabled, password,
                 actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table target;
                  uvcpp::uvcpp_db_params id_params;
                  id_params.add(user_id);
                  std::string error;
                  if (!Query("SELECT u.id, u.username, u.role_id, u.enabled, r.name AS role_name "
                             "FROM users u JOIN roles r ON r.id = u.role_id WHERE u.id = ?",
                             id_params, &target, &error) ||
                      target.empty()) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "user not found";
                    return;
                  }
                  const std::string target_name = TableText(target, 0, "username");
                  const int64_t old_role = TableInt(target, 0, "role_id");
                  const bool old_enabled = TableInt(target, 0, "enabled") != 0;
                  const std::string old_role_name = TableText(target, 0, "role_name");
                  const int64_t new_role = has_role ? role_id : old_role;
                  const bool new_enabled = has_enabled ? enabled : old_enabled;
                  if (new_role <= 0) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_role";
                    op->message = "roleId is invalid";
                    return;
                  }
                  std::string new_role_name;
                  if (!RoleExists(db_, new_role, &new_role_name)) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_role";
                    op->message = "the selected role does not exist";
                    return;
                  }
                  if (old_role_name == "admin" && ((!new_enabled) || new_role_name != "admin") &&
                      EnabledAdminCount(db_) <= 1) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "last_admin";
                    op->message = "the last enabled administrator cannot be disabled or demoted";
                    return;
                  }
                  std::string password_hash;
                  if (has_password && !HashPassword(password, &password_hash)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "hash_failed";
                    op->message = "could not hash the password";
                    return;
                  }
                  if (has_password) {
                    uvcpp::uvcpp_db_params params;
                    params.add(new_role);
                    params.add(new_enabled ? 1 : 0);
                    params.add(password_hash);
                    params.add(user_id);
                    if (!Exec("UPDATE users SET role_id = ?, enabled = ?, password_hash = ? "
                              "WHERE id = ?",
                              params, nullptr, &error)) {
                      op->ok = false;
                      op->status = 500;
                      op->code = "database_error";
                      op->message = "the user could not be updated";
                      return;
                    }
                  } else {
                    uvcpp::uvcpp_db_params params;
                    params.add(new_role);
                    params.add(new_enabled ? 1 : 0);
                    params.add(user_id);
                    if (!Exec("UPDATE users SET role_id = ?, enabled = ? WHERE id = ?", params,
                              nullptr, &error)) {
                      op->ok = false;
                      op->status = 500;
                      op->code = "database_error";
                      op->message = "the user could not be updated";
                      return;
                    }
                  }
                  if (!new_enabled) {
                    uvcpp::uvcpp_db_params sessions;
                    sessions.add(user_id);
                    Exec("DELETE FROM sessions WHERE user_id = ?", sessions, nullptr, &error);
                  }
                  Audit("user", "info", actor.username, ip, ua, "user", std::to_string(user_id),
                        "update", "success", "",
                        json{{"enabled", new_enabled},
                             {"role", new_role_name},
                             {"passwordChanged", has_password}}
                            .dump());
                  op->data = json{{"id", user_id},
                                  {"username", target_name},
                                  {"roleId", new_role},
                                  {"role", new_role_name},
                                  {"enabled", new_enabled}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "user.manage"));

  web_.del(
      "/api/v1/users/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t user_id = 0;
            if (!ParseIntParam(id_param, &user_id) || user_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid user id");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            if (actor.id == user_id) {
              SendError(resp, 400, "cannot_delete_self", "you cannot delete your own account");
              next();
              return;
            }
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, user_id, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table target;
                  uvcpp::uvcpp_db_params params;
                  params.add(user_id);
                  std::string error;
                  if (!Query("SELECT u.username, r.name AS role_name FROM users u "
                             "JOIN roles r ON r.id = u.role_id WHERE u.id = ?",
                             params, &target, &error) ||
                      target.empty()) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "user not found";
                    return;
                  }
                  const std::string username = TableText(target, 0, "username");
                  if (TableText(target, 0, "role_name") == "admin" && EnabledAdminCount(db_) <= 1) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "last_admin";
                    op->message = "the last administrator cannot be deleted";
                    return;
                  }
                  Exec("DELETE FROM sessions WHERE user_id = ?", params, nullptr, &error);
                  Exec("DELETE FROM api_tokens WHERE user_id = ?", params, nullptr, &error);
                  if (!Exec("DELETE FROM users WHERE id = ?", params, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the user could not be deleted";
                    return;
                  }
                  Audit("user", "warning", actor.username, ip, ua, "user", std::to_string(user_id),
                        "delete", "success", "", json{{"username", username}}.dump());
                  op->data = json{{"ok", true}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "user.manage"));

  // Roles -----------------------------------------------------------------
  web_.get("/api/v1/roles",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto items = std::make_shared<json>(json::array());
                 RunWork(
                     [this, items] {
                       uvcpp::uvcpp_db_table table;
                       std::string error;
                       if (!Query("SELECT id, name, display_name, permissions, builtin FROM roles "
                                  "ORDER BY id",
                                  {}, &table, &error)) {
                         return;
                       }
                       for (size_t i = 0; i < table.rows().size(); ++i) {
                         json permissions = json::array();
                         for (const auto& permission :
                              ParsePermissions(TableText(table, i, "permissions"))) {
                           permissions.push_back(permission);
                         }
                         items->push_back(json{
                             {"id", TableInt(table, i, "id")},
                             {"name", TableText(table, i, "name")},
                             {"displayName", TableText(table, i, "display_name")},
                             {"permissions", permissions},
                             {"builtin", TableInt(table, i, "builtin") != 0},
                         });
                       }
                     },
                     [items, &resp, next] {
                       SendJson(resp, 200, *items);
                       next();
                     });
               },
               "user.view"));

  web_.post(
      "/api/v1/roles",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const std::string name = JsonString(body, "name");
            const std::string display_name = JsonString(body, "displayName", name);
            std::vector<std::string> permissions;
            if (body.contains("permissions") && body["permissions"].is_array()) {
              for (const auto& item : body["permissions"]) {
                if (!item.is_string()) {
                  SendError(resp, 400, "invalid_permissions", "permissions must be strings");
                  next();
                  return;
                }
                permissions.push_back(item.get<std::string>());
              }
            }
            if (!ValidRoleName(name)) {
              SendError(resp, 400, "invalid_name",
                        "role name must start with a letter and contain lowercase letters, digits, "
                        "dot, dash or underscore");
              next();
              return;
            }
            for (const auto& permission : permissions) {
              if (!IsKnownPermission(permission) || permission == "*") {
                SendError(resp, 400, "invalid_permission", "unknown permission: " + permission);
                next();
                return;
              }
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, name, display_name, permissions, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_params params;
                  params.add(name);
                  params.add(display_name);
                  params.add(json(permissions).dump());
                  params.add(0);
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert(
                          "INSERT INTO roles(name, display_name, permissions, builtin, created_at) "
                          "VALUES(?, ?, ?, ?, ?)",
                          params, &new_id))) {
                    op->ok = false;
                    op->status = IsUniqueError(db_.last_error()) ? 409 : 500;
                    op->code = op->status == 409 ? "role_exists" : "database_error";
                    op->message = op->status == 409 ? "that role name already exists"
                                                    : "the role could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("permission", "info", actor.username, ip, ua, "role", std::to_string(id),
                        "create", "success", "",
                        json{{"name", name}, {"permissions", permissions}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id},
                                  {"name", name},
                                  {"displayName", display_name},
                                  {"permissions", permissions},
                                  {"builtin", false}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "user.manage"));

  web_.put(
      "/api/v1/roles/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t role_id = 0;
            if (!ParseIntParam(id_param, &role_id) || role_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid role id");
              next();
              return;
            }
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            std::vector<std::string> permissions;
            if (body.contains("permissions") && body["permissions"].is_array()) {
              for (const auto& item : body["permissions"]) {
                if (!item.is_string()) {
                  SendError(resp, 400, "invalid_permissions", "permissions must be strings");
                  next();
                  return;
                }
                permissions.push_back(item.get<std::string>());
              }
            }
            for (const auto& permission : permissions) {
              if (!IsKnownPermission(permission) || permission == "*") {
                SendError(resp, 400, "invalid_permission", "unknown permission: " + permission);
                next();
                return;
              }
            }
            const std::string display_name = JsonString(body, "displayName", "");
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, role_id, display_name, permissions, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table target;
                  uvcpp::uvcpp_db_params params;
                  params.add(role_id);
                  std::string error;
                  if (!Query("SELECT name, display_name, builtin FROM roles WHERE id = ?", params,
                             &target, &error) ||
                      target.empty()) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "role not found";
                    return;
                  }
                  const std::string name = TableText(target, 0, "name");
                  if (TableInt(target, 0, "builtin") != 0) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "builtin_role";
                    op->message = "built-in roles cannot be modified";
                    return;
                  }
                  const std::string final_display =
                      display_name.empty() ? TableText(target, 0, "display_name") : display_name;
                  uvcpp::uvcpp_db_params update;
                  update.add(final_display);
                  update.add(json(permissions).dump());
                  update.add(role_id);
                  if (!Exec("UPDATE roles SET display_name = ?, permissions = ? WHERE id = ?",
                            update, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the role could not be updated";
                    return;
                  }
                  Audit("permission", "info", actor.username, ip, ua, "role",
                        std::to_string(role_id), "update", "success", "",
                        json{{"permissions", permissions}}.dump());
                  op->data = json{{"id", role_id},
                                  {"name", name},
                                  {"displayName", final_display},
                                  {"permissions", permissions},
                                  {"builtin", false}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "user.manage"));

  web_.del("/api/v1/roles/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t role_id = 0;
                 if (!ParseIntParam(id_param, &role_id) || role_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid role id");
                   next();
                   return;
                 }
                 const UserContext actor = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, role_id, actor, ip, ua, op] {
                       uvcpp::uvcpp_db_table target;
                       uvcpp::uvcpp_db_params params;
                       params.add(role_id);
                       std::string error;
                       if (!Query("SELECT name, builtin FROM roles WHERE id = ?", params, &target,
                                  &error) ||
                           target.empty()) {
                         op->ok = false;
                         op->status = 404;
                         op->code = "not_found";
                         op->message = "role not found";
                         return;
                       }
                       if (TableInt(target, 0, "builtin") != 0) {
                         op->ok = false;
                         op->status = 400;
                         op->code = "builtin_role";
                         op->message = "built-in roles cannot be deleted";
                         return;
                       }
                       uvcpp::uvcpp_db_table users;
                       if (!Query("SELECT COUNT(*) AS n FROM users WHERE role_id = ?", params,
                                  &users, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the role could not be checked";
                         return;
                       }
                       if (!users.empty() && TableInt(users, 0, "n") > 0) {
                         op->ok = false;
                         op->status = 409;
                         op->code = "role_in_use";
                         op->message = "this role is still assigned to users";
                         return;
                       }
                       const std::string name = TableText(target, 0, "name");
                       if (!Exec("DELETE FROM roles WHERE id = ?", params, nullptr, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the role could not be deleted";
                         return;
                       }
                       Audit("permission", "warning", actor.username, ip, ua, "role",
                             std::to_string(role_id), "delete", "success", "",
                             json{{"name", name}}.dump());
                       op->data = json{{"ok", true}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "user.manage"));

  web_.get("/api/v1/permissions", Protected(
                                      [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                                         uvcpp::uvcpp_web_next next) {
                                        json items = json::array();
                                        for (const auto& permission : AllPermissions())
                                          items.push_back(permission);
                                        SendJson(resp, 200, items);
                                        next();
                                      },
                                      "user.view"));

  // Audit -----------------------------------------------------------------
  web_.get(
      "/api/v1/audit",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            int64_t page = 1;
            int64_t page_size = 50;
            if (const std::string* value = req.query("page"))
              ParseIntParam(value, &page);
            if (const std::string* value = req.query("pageSize"))
              ParseIntParam(value, &page_size);
            page = std::max<int64_t>(1, page);
            page_size = std::clamp<int64_t>(page_size, 1, static_cast<int64_t>(kMaxAuditPageSize));
            const std::string category = req.query("category") ? *req.query("category") : "";
            const std::string level = req.query("level") ? *req.query("level") : "";
            const std::string keyword = req.query("q") ? *req.query("q") : "";
            int64_t from = 0;
            int64_t to = 0;
            if (const std::string* value = req.query("from"))
              ParseIntParam(value, &from);
            if (const std::string* value = req.query("to"))
              ParseIntParam(value, &to);
            auto result = std::make_shared<json>();
            RunWork(
                [this, page, page_size, category, level, keyword, from, to, result] {
                  std::string where = " WHERE 1=1";
                  uvcpp::uvcpp_db_params params;
                  if (!category.empty()) {
                    where += " AND category = ?";
                    params.add(category);
                  }
                  if (!level.empty()) {
                    where += " AND level = ?";
                    params.add(level);
                  }
                  if (!keyword.empty()) {
                    where += " AND (actor LIKE ? OR action LIKE ? OR detail LIKE ?)";
                    const std::string like = "%" + keyword + "%";
                    params.add(like);
                    params.add(like);
                    params.add(like);
                  }
                  if (from > 0) {
                    where += " AND ts >= ?";
                    params.add(from);
                  }
                  if (to > 0) {
                    where += " AND ts <= ?";
                    params.add(to);
                  }
                  uvcpp::uvcpp_db_table count;
                  std::string error;
                  if (!Query("SELECT COUNT(*) AS n FROM audit_log" + where, params, &count,
                             &error)) {
                    return;
                  }
                  const int64_t total = count.empty() ? 0 : TableInt(count, 0, "n");
                  uvcpp::uvcpp_db_params list_params = params;
                  list_params.add(page_size);
                  list_params.add((page - 1) * page_size);
                  uvcpp::uvcpp_db_table table;
                  if (!Query("SELECT id, ts, category, level, actor, ip, user_agent, object_type, "
                             "object_id, action, result, error_code, detail FROM audit_log" +
                                 where + " ORDER BY ts DESC LIMIT ? OFFSET ?",
                             list_params, &table, &error)) {
                    return;
                  }
                  json items = json::array();
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    items.push_back(json{
                        {"id", TableInt(table, i, "id")},
                        {"ts", TableInt(table, i, "ts")},
                        {"category", TableText(table, i, "category")},
                        {"level", TableText(table, i, "level")},
                        {"actor", TableText(table, i, "actor")},
                        {"ip", TableText(table, i, "ip")},
                        {"userAgent", TableText(table, i, "user_agent")},
                        {"objectType", TableText(table, i, "object_type")},
                        {"objectId", TableText(table, i, "object_id")},
                        {"action", TableText(table, i, "action")},
                        {"result", TableText(table, i, "result")},
                        {"errorCode", TableText(table, i, "error_code")},
                        {"detail", TableText(table, i, "detail")},
                    });
                  }
                  *result = json{
                      {"items", items}, {"total", total}, {"page", page}, {"pageSize", page_size}};
                },
                [result, &resp, next] {
                  SendJson(resp, 200, *result);
                  next();
                });
          },
          "log.view"));

  // API tokens ------------------------------------------------------------
  web_.get(
      "/api/v1/tokens",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const UserContext user = UserFromRequest(req);
            auto items = std::make_shared<json>(json::array());
            RunWork(
                [this, user, items] {
                  uvcpp::uvcpp_db_table table;
                  uvcpp::uvcpp_db_params params;
                  params.add(user.id);
                  std::string error;
                  if (!Query(
                          "SELECT id, name, created_at, expires_at, last_used_at FROM api_tokens "
                          "WHERE user_id = ? ORDER BY id",
                          params, &table, &error)) {
                    return;
                  }
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    items->push_back(json{
                        {"id", TableInt(table, i, "id")},
                        {"name", TableText(table, i, "name")},
                        {"createdAt", TableInt(table, i, "created_at")},
                        {"expiresAt", TableInt(table, i, "expires_at")},
                        {"lastUsedAt", TableInt(table, i, "last_used_at")},
                    });
                  }
                },
                [items, &resp, next] {
                  SendJson(resp, 200, *items);
                  next();
                });
          },
          "api.access"));

  web_.post(
      "/api/v1/tokens",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const std::string name = JsonString(body, "name", "");
            const int64_t days = JsonInt(body, "expiresInDays", 0);
            if (name.empty() || name.size() > 64) {
              SendError(resp, 400, "invalid_name", "token name is required (max 64 characters)");
              next();
              return;
            }
            if (days < 0 || days > 3650) {
              SendError(resp, 400, "invalid_expiry", "expiresInDays must be between 0 and 3650");
              next();
              return;
            }
            const UserContext user = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, name, days, user, ip, ua, op] {
                  const std::string raw_token = "uvnvr_" + RandomHex(32);
                  if (raw_token.size() <= 6) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "random_failed";
                    op->message = "could not generate a token";
                    return;
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(user.id);
                  params.add(name);
                  params.add(Sha256Hex(raw_token));
                  params.add(NowSeconds());
                  if (days > 0) {
                    params.add(NowSeconds() + days * 86400);
                  } else {
                    params.add_null();
                  }
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert("INSERT INTO api_tokens(user_id, name, "
                                                     "token_hash, created_at, expires_at) "
                                                     "VALUES(?, ?, ?, ?, ?)",
                                                     params, &new_id))) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the token could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("api", "info", user.username, ip, ua, "token", std::to_string(id), "create",
                        "success", "", json{{"name", name}, {"expiresInDays", days}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id}, {"name", name}, {"token", raw_token}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "api.access"));

  web_.del("/api/v1/tokens/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t token_id = 0;
                 if (!ParseIntParam(id_param, &token_id) || token_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid token id");
                   next();
                   return;
                 }
                 const UserContext user = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, token_id, user, ip, ua, op] {
                       uvcpp::uvcpp_db_params params;
                       params.add(token_id);
                       params.add(user.id);
                       int64_t affected = 0;
                       std::string error;
                       if (!Exec("DELETE FROM api_tokens WHERE id = ? AND user_id = ?", params,
                                 &affected, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the token could not be deleted";
                         return;
                       }
                       if (affected == 0) {
                         op->ok = false;
                         op->status = 404;
                         op->code = "not_found";
                         op->message = "token not found";
                         return;
                       }
                       Audit("api", "warning", user.username, ip, ua, "token",
                             std::to_string(token_id), "delete", "success", "", "{}");
                       op->data = json{{"ok", true}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "api.access"));
}

} // namespace uvnvr
