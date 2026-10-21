#include "app.h"

#include "exporter.h"
#include "onvif.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <db/uvcpp_db.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
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
#include <sys/time.h>
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

std::string Base64Encode(const std::string& input) {
  if (input.empty())
    return {};
  std::string out(4 * ((input.size() + 2) / 3), '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                      reinterpret_cast<const unsigned char*>(input.data()),
                                      static_cast<int>(input.size()));
  if (written <= 0)
    return {};
  out.resize(static_cast<size_t>(written));
  return out;
}

bool SmtpRead(BIO* bio, int expected, std::string* error) {
  char line[1024];
  int code = 0;
  do {
    const int n = BIO_gets(bio, line, sizeof(line));
    if (n <= 0) {
      if (error)
        *error = "SMTP connection closed";
      return false;
    }
    code = std::atoi(line);
    if (std::strlen(line) < 4 || line[3] != '-')
      break;
  } while (true);
  if (code / 100 != expected / 100) {
    if (error)
      *error = "SMTP error: " + std::string(line);
    return false;
  }
  return true;
}

bool SmtpCommand(BIO* bio, const std::string& command, int expected, std::string* error) {
  const std::string line = command + "\r\n";
  if (BIO_write(bio, line.data(), static_cast<int>(line.size())) <= 0) {
    if (error)
      *error = "SMTP write failed";
    return false;
  }
  return SmtpRead(bio, expected, error);
}

bool SendSmtpMail(const std::string& host, int port, const std::string& username,
                  const std::string& password, const std::string& from, const std::string& to,
                  const std::string& subject, const std::string& body, bool use_ssl,
                  std::string* error) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == nullptr) {
    if (error)
      *error = "cannot create the TLS context";
    return false;
  }
  SSL_CTX_set_default_verify_paths(ctx);
  BIO* bio = BIO_new_ssl_connect(ctx);
  if (bio == nullptr) {
    SSL_CTX_free(ctx);
    if (error)
      *error = "cannot create the SMTP connection";
    return false;
  }
  SSL* ssl = nullptr;
  BIO_get_ssl(bio, &ssl);
  if (ssl != nullptr)
    SSL_set_tlsext_host_name(ssl, host.c_str());
  const std::string host_port = host + ":" + std::to_string(port);
  BIO_set_conn_hostname(bio, host_port.c_str());
  if (BIO_do_connect(bio) <= 0 || BIO_do_handshake(bio) <= 0) {
    if (error)
      *error = "cannot connect to the SMTP server";
    BIO_free_all(bio);
    SSL_CTX_free(ctx);
    return false;
  }
  if (ssl != nullptr && SSL_get_verify_result(ssl) != X509_V_OK) {
    if (error)
      *error = "SMTP TLS certificate verification failed";
    BIO_free_all(bio);
    SSL_CTX_free(ctx);
    return false;
  }
  bool ok = SmtpRead(bio, 220, error);
  if (ok)
    ok = SmtpCommand(bio, "EHLO uvnvr", 250, error);
  if (ok)
    ok = SmtpCommand(bio, "AUTH LOGIN", 334, error);
  if (ok)
    ok = SmtpCommand(bio, Base64Encode(username), 334, error);
  if (ok)
    ok = SmtpCommand(bio, Base64Encode(password), 235, error);
  if (ok)
    ok = SmtpCommand(bio, "MAIL FROM:<" + from + ">", 250, error);
  if (ok)
    ok = SmtpCommand(bio, "RCPT TO:<" + to + ">", 250, error);
  if (ok)
    ok = SmtpCommand(bio, "DATA", 354, error);
  if (ok) {
    std::string message = "From: " + from + "\r\nTo: " + to + "\r\nSubject: " + subject +
                          "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                          "Content-Transfer-Encoding: 8bit\r\n\r\n" +
                          body + "\r\n.\r\n";
    if (BIO_write(bio, message.data(), static_cast<int>(message.size())) <= 0) {
      if (error)
        *error = "SMTP message write failed";
      ok = false;
    } else {
      ok = SmtpRead(bio, 250, error);
    }
  }
  if (ok)
    SmtpCommand(bio, "QUIT", 221, error);
  BIO_free_all(bio);
  SSL_CTX_free(ctx);
  (void)use_ssl;
  return ok;
}

std::string PercentEncode(const std::string& input) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0x0f]);
      out.push_back(kHex[c & 0x0f]);
    }
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

void FillChannelRow(const uvcpp::uvcpp_db_table& table, size_t row, ChannelRow* channel) {
  channel->id = TableInt(table, row, "id");
  channel->group_id = TableInt(table, row, "group_id");
  channel->name = TableText(table, row, "name");
  channel->protocol = TableText(table, row, "protocol");
  channel->host = TableText(table, row, "host");
  channel->port = static_cast<int>(TableInt(table, row, "port"));
  channel->username = TableText(table, row, "username");
  channel->password_enc = TableText(table, row, "password_enc");
  channel->main_url = TableText(table, row, "main_url");
  channel->sub_url = TableText(table, row, "sub_url");
  channel->sub_enabled = TableInt(table, row, "sub_enabled") != 0;
  channel->enabled = TableInt(table, row, "enabled") != 0;
  channel->brand = TableText(table, row, "brand");
  channel->media_json = TableText(table, row, "media_json");
  channel->watermark_json = TableText(table, row, "watermark_json");
  channel->last_status = TableText(table, row, "last_status");
  channel->last_error = TableText(table, row, "last_error");
  channel->last_online_at = TableInt(table, row, "last_online_at");
  channel->last_offline_at = TableInt(table, row, "last_offline_at");
  channel->created_at = TableInt(table, row, "created_at");
  channel->updated_at = TableInt(table, row, "updated_at");
}

bool ChannelEventsEnabled(const ChannelRow& channel) {
  const json media = json::parse(channel.media_json, nullptr, false);
  return media.is_object() && media.value("events", false);
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

struct ExportResult {
  bool ok = false;
  int status = 500;
  std::string code = "export_failed";
  std::string message;
  std::string path;
  std::string work_dir;
  std::string filename;
  uint64_t size = 0;
};

struct ByteRange {
  bool present = false;
  bool syntactically_valid = true;
  bool satisfiable = true;
  uint64_t first = 0;
  uint64_t last = 0;
};

// Parses a single-range `Range: bytes=...` header. Multiple ranges are treated
// as unsupported rather than silently returning only the first one.
ByteRange ParseByteRange(const std::string& header, uint64_t size) {
  ByteRange range;
  range.present = true;
  const std::string prefix = "bytes=";
  if (header.size() < prefix.size() || header.compare(0, prefix.size(), prefix) != 0) {
    range.syntactically_valid = false;
    return range;
  }
  std::string spec = header.substr(prefix.size());
  const auto trim = [](std::string& value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
      value.pop_back();
  };
  trim(spec);
  if (spec.find(',') != std::string::npos) {
    range.syntactically_valid = false;
    return range;
  }
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    range.syntactically_valid = false;
    return range;
  }
  const std::string first_text = spec.substr(0, dash);
  const std::string last_text = spec.substr(dash + 1);
  if (size == 0) {
    range.satisfiable = false;
    return range;
  }

  if (first_text.empty()) {
    // Suffix range: the final N bytes.
    uint64_t suffix = 0;
    if (last_text.empty()) {
      range.syntactically_valid = false;
      return range;
    }
    const auto result =
        std::from_chars(last_text.data(), last_text.data() + last_text.size(), suffix);
    if (result.ec != std::errc() || result.ptr != last_text.data() + last_text.size()) {
      range.syntactically_valid = false;
      return range;
    }
    if (suffix == 0) {
      range.satisfiable = false;
      return range;
    }
    range.first = suffix >= size ? 0 : size - suffix;
    range.last = size - 1;
    return range;
  }

  uint64_t first = 0;
  auto result = std::from_chars(first_text.data(), first_text.data() + first_text.size(), first);
  if (result.ec != std::errc() || result.ptr != first_text.data() + first_text.size()) {
    range.syntactically_valid = false;
    return range;
  }
  if (first >= size) {
    range.satisfiable = false;
    return range;
  }
  uint64_t last = size - 1;
  if (!last_text.empty()) {
    result = std::from_chars(last_text.data(), last_text.data() + last_text.size(), last);
    if (result.ec != std::errc() || result.ptr != last_text.data() + last_text.size()) {
      range.syntactically_valid = false;
      return range;
    }
    if (last < first) {
      range.syntactically_valid = false;
      return range;
    }
    if (last >= size)
      last = size - 1;
  }
  range.first = first;
  range.last = last;
  return range;
}

const std::vector<std::string>& AllPermissions() {
  static const std::vector<std::string> kPermissions = {
      "dashboard.view", "preview.view",  "playback.view", "playback.download", "channel.view",
      "channel.create", "channel.delete", "channel.manage", "plan.view",       "plan.manage",
      "storage.view",   "storage.manage",
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

// `channel.manage` and the other manage permissions imply their read/create
// counterparts. Tokens can therefore grant a narrower subset without changing
// the endpoint checks.
bool PermissionImplies(const std::string& granted, const std::string& required) {
  if (granted == "*" || granted == required)
    return true;
  if (granted == "channel.manage")
    return required == "channel.view" || required == "channel.create" ||
           required == "channel.delete";
  if (granted == "playback.download")
    return required == "playback.view";
  if (granted == "plan.manage")
    return required == "plan.view";
  if (granted == "storage.manage")
    return required == "storage.view";
  if (granted == "alert.manage")
    return required == "alert.view";
  if (granted == "user.manage")
    return required == "user.view";
  if (granted == "system.manage")
    return required == "system.view";
  return false;
}

const std::map<std::string, std::vector<std::string>>& ApiScopes() {
  static const std::map<std::string, std::vector<std::string>> kScopes = {
      {"all", {"*"}},
      {"preview", {"preview.view", "api.stream"}},
      {"snapshots", {"api.snapshot"}},
      {"channels.read", {"channel.view"}},
      {"channels.create", {"channel.create"}},
      {"channels.delete", {"channel.delete"}},
      {"channels.manage", {"channel.manage"}},
      {"playback", {"playback.view"}},
      {"download", {"playback.view", "playback.download", "api.download"}},
      {"alerts", {"alert.view", "alert.manage", "api.alerts"}},
      {"plans", {"plan.view", "plan.manage"}},
      {"storage", {"storage.view", "storage.manage"}},
      {"time", {"system.view", "system.manage"}},
      {"system", {"system.view"}},
      {"rtsp", {"rtsp.access"}},
      {"audit", {"log.view"}},
      {"users", {"user.view", "user.manage"}},
  };
  return kScopes;
}

bool IsKnownApiScope(const std::string& scope) {
  return ApiScopes().find(scope) != ApiScopes().end();
}

std::vector<std::string> EffectiveTokenPermissions(const std::vector<std::string>& scopes,
                                                   const std::vector<std::string>& role) {
  if (scopes.empty())
    return role;
  if (std::find(scopes.begin(), scopes.end(), "all") != scopes.end())
    return role;
  std::vector<std::string> expanded = {"api.access"};
  for (const std::string& scope : scopes) {
    const auto it = ApiScopes().find(scope);
    if (it == ApiScopes().end())
      continue;
    for (const std::string& permission : it->second)
      expanded.push_back(permission);
  }
  std::vector<std::string> effective;
  for (const std::string& permission : expanded) {
    const bool allowed =
        std::any_of(role.begin(), role.end(), [&](const std::string& granted) {
          return PermissionImplies(granted, permission);
        });
    if (allowed && std::find(effective.begin(), effective.end(), permission) == effective.end())
      effective.push_back(permission);
  }
  return effective;
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
      "last_used_at INTEGER,"
      "scopes_json TEXT NOT NULL DEFAULT '[]')",
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
      "CREATE TABLE IF NOT EXISTS channels ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL,"
      "protocol TEXT NOT NULL DEFAULT 'rtsp',"
      "host TEXT NOT NULL DEFAULT '',"
      "port INTEGER NOT NULL DEFAULT 554,"
      "username TEXT NOT NULL DEFAULT '',"
      "password_enc TEXT NOT NULL DEFAULT '',"
      "main_url TEXT NOT NULL DEFAULT '',"
      "sub_url TEXT NOT NULL DEFAULT '',"
      "sub_enabled INTEGER NOT NULL DEFAULT 0,"
      "enabled INTEGER NOT NULL DEFAULT 1,"
      "brand TEXT NOT NULL DEFAULT '',"
      "media_json TEXT NOT NULL DEFAULT '{}',"
      "watermark_json TEXT NOT NULL DEFAULT '{}',"
      "last_status TEXT NOT NULL DEFAULT 'unknown',"
      "last_error TEXT NOT NULL DEFAULT '',"
      "last_online_at INTEGER NOT NULL DEFAULT 0,"
      "last_offline_at INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS channel_groups ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "name TEXT NOT NULL,"
      "parent_id INTEGER NOT NULL DEFAULT 0,"
      "sort INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS channel_group_members ("
      "channel_id INTEGER PRIMARY KEY,"
      "group_id INTEGER NOT NULL)",
      "CREATE INDEX IF NOT EXISTS idx_channel_groups_parent "
      "ON channel_groups(parent_id, sort)",
      "CREATE INDEX IF NOT EXISTS idx_channel_group_members_group "
      "ON channel_group_members(group_id)",
      "CREATE TABLE IF NOT EXISTS layouts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "user_id INTEGER NOT NULL,"
      "name TEXT NOT NULL,"
      "grid_json TEXT NOT NULL,"
      "is_default INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL)",
      "CREATE INDEX IF NOT EXISTS idx_layouts_user ON layouts(user_id, updated_at)",
      "CREATE TABLE IF NOT EXISTS record_plans ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "channel_id INTEGER NOT NULL,"
      "weekday INTEGER NOT NULL,"
      "start_min INTEGER NOT NULL,"
      "end_min INTEGER NOT NULL,"
      "mode TEXT NOT NULL DEFAULT 'timed',"
      "stream TEXT NOT NULL DEFAULT 'main',"
      "enabled INTEGER NOT NULL DEFAULT 1,"
      "created_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS storage_volumes ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "path TEXT NOT NULL UNIQUE,"
      "label TEXT NOT NULL DEFAULT '',"
      "mode TEXT NOT NULL DEFAULT 'loop',"
      "max_usage_pct INTEGER NOT NULL DEFAULT 90,"
      "reserve_gb INTEGER NOT NULL DEFAULT 5,"
      "retention_days INTEGER NOT NULL DEFAULT 30,"
      "enabled INTEGER NOT NULL DEFAULT 1,"
      "created_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS recordings ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "channel_id INTEGER NOT NULL,"
      "stream TEXT NOT NULL,"
      "volume_id INTEGER NOT NULL DEFAULT 1,"
      "path TEXT NOT NULL,"
      "start_ms INTEGER NOT NULL,"
      "end_ms INTEGER NOT NULL,"
      "duration_ms INTEGER NOT NULL,"
      "size_bytes INTEGER NOT NULL,"
      "codec TEXT NOT NULL DEFAULT '',"
      "record_type TEXT NOT NULL DEFAULT 'timed',"
      "created_at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS channel_events ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "channel_id INTEGER NOT NULL,"
      "state TEXT NOT NULL,"
      "reason TEXT NOT NULL DEFAULT '',"
      "at INTEGER NOT NULL)",
      "CREATE TABLE IF NOT EXISTS alerts ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "channel_id INTEGER NOT NULL DEFAULT 0,"
      "type TEXT NOT NULL,"
      "level TEXT NOT NULL DEFAULT 'warning',"
      "message TEXT NOT NULL,"
      "snapshot_path TEXT NOT NULL DEFAULT '',"
      "acknowledged INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL)",
      "CREATE INDEX IF NOT EXISTS idx_alerts_created ON alerts(created_at)",
      "CREATE INDEX IF NOT EXISTS idx_alerts_ack ON alerts(acknowledged, created_at)",
      "CREATE INDEX IF NOT EXISTS idx_channels_enabled ON channels(enabled)",
      "CREATE INDEX IF NOT EXISTS idx_plans_channel ON record_plans(channel_id, weekday)",
      "CREATE INDEX IF NOT EXISTS idx_recordings_channel ON recordings(channel_id, start_ms)",
      "CREATE INDEX IF NOT EXISTS idx_recordings_volume ON recordings(volume_id, start_ms)",
      "CREATE INDEX IF NOT EXISTS idx_channel_events ON channel_events(channel_id, at)",
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

  // Existing databases predate per-token API scopes.
  {
    uvcpp::uvcpp_db_table columns;
    if (Query("PRAGMA table_info(api_tokens)", {}, &columns, error)) {
      bool has_scopes = false;
      for (size_t row = 0; row < columns.rows().size(); ++row) {
        if (TableText(columns, row, "name") == "scopes_json")
          has_scopes = true;
      }
      if (!has_scopes &&
          !Exec("ALTER TABLE api_tokens ADD COLUMN scopes_json TEXT NOT NULL DEFAULT '[]'", {},
                nullptr, error)) {
        return false;
      }
    }
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
      {"security.web_max_failures", "10"},
      {"security.web_lock_minutes", "10"},
      {"security.api_max_failures", "100"},
      {"security.api_lock_minutes", "10"},
      {"security.api_locked_until", "0"},
      {"audit.retention_days", "180"},
      {"system.timezone", "UTC"},
      {"system.language", "auto"},
      {"system.gpu_enabled", "1"},
      {"system.ntp_enabled", "0"},
      {"system.ntp_server", "pool.ntp.org"},
      {"system.ntp_last_offset_ms", "0"},
      {"system.ntp_last_sync", "0"},
      {"smtp.enabled", "0"},
      {"smtp.host", "smtp.qq.com"},
      {"smtp.port", "465"},
      {"smtp.username", ""},
      {"smtp.password_enc", ""},
      {"smtp.from", ""},
      {"smtp.to", ""},
      {"smtp.ssl", "1"},
      {"live.cache_mb", "8"},
      {"recording.segment_minutes", "5"},
      {"recording.segment_size_mb", "256"},
      {"recording.segment_mode", "time"},
      {"recording.retention_days", "30"},
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
    if (error != nullptr) {
      std::error_code absolute_ec;
      const std::filesystem::path absolute =
          std::filesystem::absolute(config_.data_dir, absolute_ec);
      const std::string location = absolute_ec ? config_.data_dir : absolute.string();
      *error = "cannot create data directory '" + location + "': " + ec.message() +
               "; pass --data-dir with a writable path";
    }
    return false;
  }

  // Exports are temporary; remove leftovers from a previous crash so an
  // interrupted job cannot fill the data volume.
  {
    std::error_code export_ec;
    const std::filesystem::path exports = std::filesystem::path(config_.data_dir) / "exports";
    for (const auto& entry : std::filesystem::directory_iterator(exports, export_ec))
      std::filesystem::remove_all(entry.path(), export_ec);
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
  if (!EnsureSecretKey(error)) {
    if (error != nullptr)
      *error = "cannot initialise the secret key: " + *error;
    return false;
  }
  EnsureDefaults();

  setup_required_ = CountUsers() == 0;
  CleanupAudit();
  return true;
}

void App::CleanupAudit() {
  std::lock_guard<std::mutex> lock(audit_cleanup_mutex_);
  CleanupAuditLocked();
}

void App::CleanupAuditLocked() {
  const int64_t retention_days = std::max<int64_t>(1, GetSettingInt("audit.retention_days", 180));
  const int64_t cutoff = NowSeconds() - retention_days * 86400;
  uvcpp::uvcpp_db_params params;
  params.add(cutoff);
  std::string error;
  if (!Exec("DELETE FROM audit_log WHERE ts < ?", params, nullptr, &error)) {
    std::fprintf(stderr, "audit cleanup failed: %s\n", error.c_str());
  }
  last_audit_cleanup_ = NowSeconds();
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
    return;
  }
  if (NowSeconds() - last_audit_cleanup_.load() > 86400) {
    CleanupAudit();
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
    if (PermissionImplies(item, permission))
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
        "t.id AS token_id, t.scopes_json AS scopes_json FROM api_tokens t "
        "JOIN users u ON u.id = t.user_id "
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
    const std::vector<std::string> role_permissions =
        ParsePermissions(TableText(table, 0, "permissions"));
    const std::vector<std::string> token_scopes =
        ParsePermissions(TableText(table, 0, "scopes_json"));
    user.permissions = EffectiveTokenPermissions(token_scopes, role_permissions);
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
  const std::string hostname = platform::HostName();
  const std::string os_name = platform::OsDescription();
  const double uptime = platform::UptimeSeconds();
  const uint64_t total_memory = platform::TotalMemoryBytes();
  const uint64_t free_memory = platform::FreeMemoryBytes();
  const uint64_t used_memory = total_memory > free_memory ? total_memory - free_memory : 0;
  const double memory_percent =
      total_memory == 0 ? 0.0 : (100.0 * static_cast<double>(used_memory) / total_memory);
  const int cpu_count = platform::CpuCount();
  double cpu_percent = 0.0;
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    cpu_percent = platform::CpuUsagePercent(&last_cpu_total_, &last_cpu_idle_);
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
  uvcpp::uvcpp_db_table alert_count;
  uvcpp::uvcpp_db_params alert_params;
  alert_params.add(1);
  Query("SELECT COUNT(*) AS n FROM alerts WHERE acknowledged = ?", alert_params, &alert_count,
        &error);

  const double memory_gb = static_cast<double>(total_memory) / (1024.0 * 1024.0 * 1024.0);
  const int tier_cores =
      cpu_count > 0 ? cpu_count : static_cast<int>(std::thread::hardware_concurrency());
  std::string tier = "high";
  if (tier_cores <= 4 || memory_gb <= 8.0) {
    tier = "low";
  } else if (tier_cores <= 8 || memory_gb <= 16.0) {
    tier = "medium";
  }
  bool gpu_available = false;
  std::string gpu_vendor = "none";
#ifdef _WIN32
  {
    std::error_code gpu_ec;
    if (std::filesystem::exists("C:\\Program Files\\NVIDIA Corporation", gpu_ec)) {
      gpu_available = true;
      gpu_vendor = "nvidia";
    }
  }
#else
  {
    std::error_code gpu_ec;
    if (std::filesystem::exists("/dev/nvidia0", gpu_ec) ||
        std::getenv("CUDA_VISIBLE_DEVICES") != nullptr) {
      gpu_available = true;
      gpu_vendor = "nvidia";
    } else if (std::filesystem::exists("/dev/dri", gpu_ec)) {
      gpu_available = true;
      gpu_vendor = "intel/amd";
    }
  }
#endif
  const bool gpu_enabled = GetSettingInt("system.gpu_enabled", 1) != 0;

  result["version"] = UVNVR_VERSION;
  result["commit"] = UVNVR_GIT_SHA;
  result["serial"] = serial_;
  result["hostname"] = hostname;
  result["os"] = os_name;
  result["uptimeSeconds"] = static_cast<int64_t>(uptime);
  result["setupRequired"] = setup_required_.load();
  result["maintenance"] = config_.maintenance;
  result["performance"] =
      json{{"tier", tier},
           {"gpu",
            json{{"available", gpu_available}, {"vendor", gpu_vendor}, {"enabled", gpu_enabled}}}};
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
                          {"alerts", alert_count.empty() ? 0 : TableInt(alert_count, 0, "n")},
                          {"channels", 0},
                          {"recordings", 0}};
  if (playback_) {
    const BlockCache::Stats cache = playback_->CacheStats();
    result["cache"] = json{{"hits", cache.hits},
                           {"misses", cache.misses},
                           {"bytesRead", cache.bytes_read},
                           {"bytesCached", cache.bytes_cached},
                           {"entries", cache.entries},
                           {"capacityBytes", cache.capacity_bytes},
                           {"blockSize", cache.block_size}};
  }
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
                        "img-src 'self' data:; media-src 'self' blob:; "
                        "worker-src 'self' blob:; child-src 'self' blob:; font-src 'self'; "
                        "connect-src 'self' ws: wss:; object-src 'none'; base-uri 'self'; "
                        "frame-ancestors 'none'");
        next();
      });
  web_.use(uvcpp::web_middleware_request_id("X-Request-Id", false));
  web_.use(uvcpp::web_middleware_access_log());
}

void App::RegisterStatic() {
  std::string web_root = "./frontend/dist";
  const std::string exe_path = platform::ExecutablePath();
  if (!exe_path.empty()) {
    const std::filesystem::path dir = std::filesystem::path(exe_path).parent_path();
    const std::vector<std::filesystem::path> candidates = {
        dir / ".." / "share" / "uvnvr" / "frontend" / "dist",
        dir / ".." / "share" / "uvnvr" / "web",
        dir / "frontend" / "dist",
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::exists(candidate / "index.html", ec) && !ec) {
        web_root = candidate.lexically_normal().string();
        break;
      }
    }
  }
  std::ifstream entry(web_root + "/index.html");
  if (entry.good()) {
    uvcpp::uvcpp_web_static_options options;
    options.spa_fallback = true;
    web_.serve_static("/", web_root, options);
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
  if (!config_.host.empty())
    web_.set_host(config_.host);
  if (config_.log_level == "trace")
    web_.set_log_level(uvcpp::log_level::TRACE);
  else if (config_.log_level == "debug")
    web_.set_log_level(uvcpp::log_level::DEBUG);
  else if (config_.log_level == "warn")
    web_.set_log_level(uvcpp::log_level::WARN);
  else if (config_.log_level == "error")
    web_.set_log_level(uvcpp::log_level::ERR);
  else
    web_.set_log_level(uvcpp::log_level::INFO);
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

  const std::string display_host =
      config_.host.empty() || config_.host == "0.0.0.0" || config_.host == "::" ? "127.0.0.1"
                                                                                : config_.host;
  std::printf("UvNVR %s (%s) listening on %s://%s:%d\n", UVNVR_VERSION, UVNVR_GIT_SHA,
              tls_enabled ? "https" : "http", display_host.c_str(), web_.bound_port());
  std::printf("config: %s\n",
              config_.config_loaded ? config_.config_path.c_str() : "(built-in defaults)");
  media_ = std::make_unique<MediaService>(&web_);
  if (!config_.ffmpeg_path.empty())
    media_->SetFfmpegPath(config_.ffmpeg_path);
  if (const char* ffmpeg = std::getenv("UVNVR_FFMPEG"); ffmpeg != nullptr && ffmpeg[0] != '\0') {
    media_->SetFfmpegPath(ffmpeg);
  }
  onvif_events_ = std::make_unique<OnvifEventService>(
      [this](int64_t channel_id, const std::string& type, const std::string& message) {
        RaiseAlert(channel_id, type, "warning", message, "");
      });
  media_->Start();
  SyncAllMedia();
  recording_ = std::make_unique<RecordingService>(&db_, config_.data_dir);
  media_->SetDataSink(
      [this](int64_t channel_id, const std::string& stream, const uint8_t* data, size_t len) {
        if (recording_)
          recording_->OnTs(channel_id, stream, data, len);
      });
  recording_->Start();
  playback_ = std::make_unique<PlaybackService>(
      &web_, &db_, static_cast<size_t>(config_.cache_mb) * 1024 * 1024,
      static_cast<size_t>(config_.readahead_mb) * 1024 * 1024);
  rtsp_ = std::make_unique<RtspServer>(
      [this](const std::string& user, const std::string& password) {
        return VerifyRtspCredentials(user, password);
      },
      [this](int64_t channel, const std::string& stream, MediaService::RawSink sink,
             std::string* error) {
        return media_ ? media_->AddRawSubscriber(channel, stream, std::move(sink), error) : 0;
      },
      [this](int64_t channel, const std::string& stream, uint64_t id) {
        if (media_)
          media_->RemoveRawSubscriber(channel, stream, id);
      },
      config_.rtsp_port);
  std::string rtsp_error;
  if (!rtsp_->Start(&rtsp_error)) {
    std::fprintf(stderr, "RTSP server unavailable: %s\n", rtsp_error.c_str());
  } else {
    std::printf("RTSP: rtsp://0.0.0.0:%d/live/{channel}/{main|sub}\n", rtsp_->Port());
  }
  StartMonitors();

  std::printf("setup required: %s\n", setup_required_.load() ? "yes" : "no");
  std::error_code data_ec;
  const std::filesystem::path data_path = std::filesystem::absolute(config_.data_dir, data_ec);
  std::printf("data: %s\n", (data_ec ? config_.data_dir : data_path.string()).c_str());
  std::printf("ffmpeg: %s (%s)\n", media_->FfmpegPath().c_str(),
              media_->Available() ? "available" : "not found");
  std::fflush(stdout);
  web_.join();
  if (recording_)
    recording_->Stop();
  media_.reset();
  recording_.reset();
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
                         if (system.contains("ntpEnabled")) {
                           const bool enabled = JsonBool(system, "ntpEnabled", false);
                           std::string db_error;
                           if (!SetSetting("system.ntp_enabled", enabled ? "1" : "0", &db_error)) {
                             *error = db_error;
                             return;
                           }
                         }
                         if (system.contains("ntpServer")) {
                           const std::string server =
                               JsonString(system, "ntpServer", "pool.ntp.org");
                           if (server.empty() || server.size() > 255) {
                             *error = "NTP server is invalid";
                             return;
                           }
                           std::string db_error;
                           if (!SetSetting("system.ntp_server", server, &db_error)) {
                             *error = db_error;
                             return;
                           }
                         }
                         if (system.contains("gpuEnabled")) {
                           const bool enabled = JsonBool(system, "gpuEnabled", true);
                           std::string db_error;
                           if (!SetSetting("system.gpu_enabled", enabled ? "1" : "0", &db_error)) {
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
                       if (body.contains("smtp") && body["smtp"].is_object()) {
                         const auto& smtp = body["smtp"];
                         auto set_string = [&](const char* key, const std::string& value) -> bool {
                           std::string db_error;
                           if (!SetSetting(std::string("smtp.") + key, value, &db_error)) {
                             *error = db_error;
                             return false;
                           }
                           return true;
                         };
                         if (smtp.contains("enabled") &&
                             !set_string("enabled", JsonBool(smtp, "enabled", false) ? "1" : "0"))
                           return;
                         if (smtp.contains("host")) {
                           const std::string host = JsonString(smtp, "host", "smtp.qq.com");
                           if (host.empty() || host.size() > 255) {
                             *error = "SMTP host is invalid";
                             return;
                           }
                           if (!set_string("host", host))
                             return;
                         }
                         if (smtp.contains("port")) {
                           const int64_t port = JsonInt(smtp, "port", 465);
                           if (port < 1 || port > 65535) {
                             *error = "SMTP port is invalid";
                             return;
                           }
                           if (!set_string("port", std::to_string(port)))
                             return;
                         }
                         if (smtp.contains("username") &&
                             !set_string("username", JsonString(smtp, "username")))
                           return;
                         if (smtp.contains("from") && !set_string("from", JsonString(smtp, "from")))
                           return;
                         if (smtp.contains("to") && !set_string("to", JsonString(smtp, "to")))
                           return;
                         if (smtp.contains("password") && smtp["password"].is_string() &&
                             !smtp["password"].get<std::string>().empty()) {
                           std::string encrypt_error;
                           const std::string encrypted =
                               EncryptSecret(smtp["password"].get<std::string>(), &encrypt_error);
                           if (encrypted.empty()) {
                             *error = encrypt_error;
                             return;
                           }
                           if (!set_string("password_enc", encrypted))
                             return;
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
                      {"language", GetSetting("system.language", "auto")},
                      {"gpuEnabled", GetSettingInt("system.gpu_enabled", 1) != 0},
                      {"ntpEnabled", GetSettingInt("system.ntp_enabled", 0) != 0},
                      {"ntpServer", GetSetting("system.ntp_server", "pool.ntp.org")},
                      {"ntpOffsetMs", GetSettingInt("system.ntp_last_offset_ms", 0)}}},
      {"smtp", json{{"enabled", GetSettingInt("smtp.enabled", 0) != 0},
                    {"host", GetSetting("smtp.host", "smtp.qq.com")},
                    {"port", GetSettingInt("smtp.port", 465)},
                    {"username", GetSetting("smtp.username", "")},
                    {"from", GetSetting("smtp.from", "")},
                    {"to", GetSetting("smtp.to", "")},
                    {"hasPassword", !GetSetting("smtp.password_enc", "").empty()}}}};
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
  RegisterChannelRoutes();
  RegisterGroupRoutes();
  RegisterLayoutRoutes();
  RegisterOnvifRoutes();
  RegisterStreamRoutes();
  RegisterRecordingRoutes();
  RegisterStorageRoutes();
  RegisterPlanRoutes();
  RegisterAlertRoutes();
  RegisterSystemRoutes();

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
                          "SELECT id, name, created_at, expires_at, last_used_at, scopes_json "
                          "FROM api_tokens WHERE user_id = ? ORDER BY id",
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
                        {"scopes", json::parse(TableText(table, i, "scopes_json"), nullptr, false)},
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
            std::vector<std::string> scopes;
            if (body.contains("scopes")) {
              if (!body["scopes"].is_array()) {
                SendError(resp, 400, "invalid_scopes", "scopes must be an array");
                next();
                return;
              }
              for (const auto& item : body["scopes"]) {
                if (!item.is_string() || !IsKnownApiScope(item.get<std::string>())) {
                  SendError(resp, 400, "invalid_scopes", "unknown API scope");
                  next();
                  return;
                }
                const std::string scope = item.get<std::string>();
                if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end())
                  scopes.push_back(scope);
              }
            }
            const UserContext user = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, name, days, scopes, user, ip, ua, op] {
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
                  params.add(json(scopes).dump());
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert("INSERT INTO api_tokens(user_id, name, "
                                                     "token_hash, created_at, expires_at, "
                                                     "scopes_json) VALUES(?, ?, ?, ?, ?, ?)",
                                                     params, &new_id))) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the token could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("api", "info", user.username, ip, ua, "token", std::to_string(id),
                        "create", "success", "",
                        json{{"name", name}, {"expiresInDays", days}, {"scopes", scopes}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id},
                                  {"name", name},
                                  {"token", raw_token},
                                  {"scopes", scopes}};
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

bool App::EnsureSecretKey(std::string* error) {
  const std::filesystem::path path = std::filesystem::path(config_.data_dir) / "secret.key";
  if (std::filesystem::exists(path)) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      if (error)
        *error = "cannot read " + path.string();
      return false;
    }
    secret_key_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (secret_key_.size() != 32) {
      if (error)
        *error = "secret.key must contain exactly 32 bytes";
      return false;
    }
    return true;
  }

  secret_key_.assign(32, 0);
  if (RAND_bytes(secret_key_.data(), static_cast<int>(secret_key_.size())) != 1) {
    if (error)
      *error = "cannot generate the secret key";
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error)
      *error = "cannot write " + path.string();
    return false;
  }
  output.write(reinterpret_cast<const char*>(secret_key_.data()),
               static_cast<std::streamsize>(secret_key_.size()));
  output.close();
  std::error_code ec;
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
  return true;
}

void App::EnsureDefaults() {
  std::error_code ec;
  const std::filesystem::path recordings =
      std::filesystem::absolute(std::filesystem::path(config_.data_dir) / "recordings", ec);
  std::filesystem::create_directories(recordings, ec);

  uvcpp::uvcpp_db_table table;
  std::string error;
  if (!Query("SELECT COUNT(*) AS n FROM storage_volumes", {}, &table, &error))
    return;
  if (!table.empty() && TableInt(table, 0, "n") > 0)
    return;

  uvcpp::uvcpp_db_params params;
  params.add(recordings.string());
  params.add("Default");
  params.add("loop");
  params.add(90);
  params.add(5);
  params.add(30);
  params.add(1);
  params.add(NowSeconds());
  Exec("INSERT INTO storage_volumes(path, label, mode, max_usage_pct, reserve_gb, "
       "retention_days, enabled, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
       params, nullptr, &error);
}

std::string App::EncryptSecret(const std::string& plain, std::string* error) {
  if (plain.empty())
    return {};
  if (secret_key_.size() != 32) {
    if (error)
      *error = "secret key is not initialised";
    return {};
  }
  unsigned char iv[12];
  if (RAND_bytes(iv, sizeof(iv)) != 1) {
    if (error)
      *error = "cannot generate an IV";
    return {};
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    if (error)
      *error = "cannot allocate the cipher context";
    return {};
  }
  std::vector<unsigned char> cipher(plain.size());
  unsigned char tag[16];
  int out_len = 0;
  int total = 0;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, secret_key_.data(), iv) == 1 &&
            EVP_EncryptUpdate(ctx, cipher.data(), &out_len,
                              reinterpret_cast<const unsigned char*>(plain.data()),
                              static_cast<int>(plain.size())) == 1;
  if (ok) {
    total = out_len;
    ok = EVP_EncryptFinal_ex(ctx, cipher.data() + total, &out_len) == 1;
    if (ok)
      total += out_len;
  }
  if (ok)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    if (error)
      *error = "AES-GCM encryption failed";
    return {};
  }
  cipher.resize(static_cast<size_t>(total));
  return "v1:" + HexEncode(iv, sizeof(iv)) + ":" + HexEncode(cipher.data(), cipher.size()) + ":" +
         HexEncode(tag, sizeof(tag));
}

std::string App::DecryptSecret(const std::string& encoded, std::string* error) {
  if (encoded.empty())
    return {};
  const std::vector<std::string> parts = Split(encoded, ':');
  if (parts.size() != 4 || parts[0] != "v1") {
    if (error)
      *error = "unsupported secret format";
    return {};
  }
  std::vector<unsigned char> iv;
  std::vector<unsigned char> cipher;
  std::vector<unsigned char> tag;
  if (!HexDecode(parts[1], &iv) || !HexDecode(parts[2], &cipher) || !HexDecode(parts[3], &tag) ||
      iv.size() != 12 || tag.size() != 16 || secret_key_.size() != 32) {
    if (error)
      *error = "invalid secret encoding";
    return {};
  }
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    if (error)
      *error = "cannot allocate the cipher context";
    return {};
  }
  std::vector<unsigned char> plain(cipher.size() + 16);
  int out_len = 0;
  int total = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
      EVP_DecryptInit_ex(ctx, nullptr, nullptr, secret_key_.data(), iv.data()) == 1 &&
      EVP_DecryptUpdate(ctx, plain.data(), &out_len, cipher.data(),
                        static_cast<int>(cipher.size())) == 1;
  if (ok) {
    total = out_len;
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) ==
             1 &&
         EVP_DecryptFinal_ex(ctx, plain.data() + total, &out_len) == 1;
    if (ok)
      total += out_len;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    if (error)
      *error = "AES-GCM authentication failed";
    return {};
  }
  return std::string(reinterpret_cast<char*>(plain.data()), static_cast<size_t>(total));
}

bool App::LoadChannel(int64_t id, ChannelRow* out, std::string* error) {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(id);
  if (!Query("SELECT c.*, COALESCE(m.group_id, 0) AS group_id FROM channels c "
             "LEFT JOIN channel_group_members m ON m.channel_id = c.id WHERE c.id = ?",
             params, &table, error) ||
      table.empty()) {
    return false;
  }
  FillChannelRow(table, 0, out);
  return true;
}

std::vector<ChannelRow> App::LoadChannels(std::string* error) {
  std::vector<ChannelRow> result;
  uvcpp::uvcpp_db_table table;
  if (!Query("SELECT c.*, COALESCE(m.group_id, 0) AS group_id FROM channels c "
             "LEFT JOIN channel_group_members m ON m.channel_id = c.id "
             "ORDER BY c.name COLLATE NOCASE, c.id",
             {}, &table, error)) {
    return result;
  }
  result.reserve(table.rows().size());
  for (size_t i = 0; i < table.rows().size(); ++i) {
    ChannelRow channel;
    FillChannelRow(table, i, &channel);
    result.push_back(std::move(channel));
  }
  return result;
}

json App::ChannelJson(const ChannelRow& channel) const {
  json media = json::parse(channel.media_json, nullptr, false);
  if (media.is_discarded() || !media.is_object())
    media = json::object();
  json watermark = json::parse(channel.watermark_json, nullptr, false);
  if (watermark.is_discarded() || !watermark.is_object())
    watermark = json::object();
  return json{
      {"id", channel.id},
      {"groupId", channel.group_id},
      {"name", channel.name},
      {"protocol", channel.protocol},
      {"host", channel.host},
      {"port", channel.port},
      {"username", channel.username},
      {"hasPassword", !channel.password_enc.empty()},
      {"mainUrl", channel.main_url},
      {"subUrl", channel.sub_url},
      {"subEnabled", channel.sub_enabled},
      {"enabled", channel.enabled},
      {"brand", channel.brand},
      {"media", media},
      {"eventsEnabled", ChannelEventsEnabled(channel)},
      {"watermark", watermark},
      {"lastStatus", channel.last_status},
      {"lastError", channel.last_error},
      {"lastOnlineAt", channel.last_online_at},
      {"lastOfflineAt", channel.last_offline_at},
      {"createdAt", channel.created_at},
      {"updatedAt", channel.updated_at},
  };
}

std::string App::BuildChannelUrl(const ChannelRow& channel, bool sub, std::string* error) {
  std::string url = sub ? channel.sub_url : channel.main_url;
  if (url.empty() && channel.protocol == "hikvision") {
    url = sub ? "rtsp://{user}:{pass}@{host}:{port}/Streaming/Channels/102"
              : "rtsp://{user}:{pass}@{host}:{port}/Streaming/Channels/101";
  } else if (url.empty() && channel.protocol == "dahua") {
    url = sub ? "rtsp://{user}:{pass}@{host}:{port}/cam/realmonitor?channel=1&subtype=1"
              : "rtsp://{user}:{pass}@{host}:{port}/cam/realmonitor?channel=1&subtype=0";
  }
  if (url.empty()) {
    if (error)
      *error = "stream URL is empty";
    return {};
  }
  std::string password;
  if (!channel.password_enc.empty()) {
    password = DecryptSecret(channel.password_enc, error);
    if (password.empty() && error != nullptr && !error->empty())
      return {};
  }
  auto replace_all = [](std::string& target, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = target.find(from, pos)) != std::string::npos) {
      target.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(url, "{user}", PercentEncode(channel.username));
  replace_all(url, "{pass}", PercentEncode(password));
  replace_all(url, "{host}", channel.host);
  replace_all(url, "{port}", std::to_string(channel.port));

  if (!channel.username.empty() && url.rfind("rtsp://", 0) == 0) {
    const size_t authority = 7;
    const size_t slash = url.find('/', authority);
    const size_t at = url.find('@', authority);
    if (at == std::string::npos || (slash != std::string::npos && at > slash)) {
      url.insert(authority, PercentEncode(channel.username) + ":" + PercentEncode(password) + "@");
    }
  }
  return url;
}

void App::SyncMediaChannel(int64_t id) {
  ChannelRow channel;
  std::string error;
  if (!LoadChannel(id, &channel, &error)) {
    if (media_)
      media_->Remove(id);
    if (onvif_events_)
      onvif_events_->Remove(id);
    return;
  }

  if (onvif_events_) {
    if (!channel.enabled || channel.protocol != "onvif" || !ChannelEventsEnabled(channel)) {
      onvif_events_->Remove(id);
    } else {
      OnvifEventConfig config;
      config.channel_id = id;
      config.host = channel.host;
      config.port = channel.port > 0 ? channel.port : 80;
      config.username = channel.username;
      if (!channel.password_enc.empty()) {
        std::string decrypt_error;
        config.password = DecryptSecret(channel.password_enc, &decrypt_error);
        if (config.password.empty() && !decrypt_error.empty()) {
          std::fprintf(stderr, "channel %lld ONVIF events: %s\n", static_cast<long long>(id),
                       decrypt_error.c_str());
        }
      }
      onvif_events_->Upsert(config);
    }
  }

  if (!media_)
    return;
  if (!channel.enabled) {
    media_->Remove(id);
    return;
  }
  ChannelSpec spec;
  spec.id = channel.id;
  spec.name = channel.name;
  spec.enabled = channel.enabled;
  spec.sub_enabled = channel.sub_enabled;
  spec.main_url = BuildChannelUrl(channel, false, &error);
  if (spec.main_url.empty()) {
    std::fprintf(stderr, "channel %lld has no usable main stream: %s\n", static_cast<long long>(id),
                 error.c_str());
    media_->Remove(id);
    return;
  }
  if (channel.sub_enabled) {
    spec.sub_url = BuildChannelUrl(channel, true, &error);
    if (spec.sub_url.empty()) {
      spec.sub_enabled = false;
      std::fprintf(stderr, "channel %lld has no usable sub stream: %s\n",
                   static_cast<long long>(id), error.c_str());
    }
  }
  media_->Upsert(spec);
}

void App::SyncAllMedia() {
  if (!media_)
    return;
  std::string error;
  const auto channels = LoadChannels(&error);
  for (const auto& channel : channels) {
    if (!channel.enabled)
      continue;
    SyncMediaChannel(channel.id);
  }
}

bool App::CreateStreamTicket(int64_t user_id, int64_t channel_id, const std::string& stream,
                             std::string* token, int64_t* expires_at, std::string* error) {
  if (stream != "main" && stream != "sub") {
    if (error)
      *error = "stream must be main or sub";
    return false;
  }
  *token = RandomHex(24);
  if (token->empty()) {
    if (error)
      *error = "cannot generate a stream ticket";
    return false;
  }
  *expires_at = NowSeconds() + 30;
  std::lock_guard<std::mutex> lock(tickets_mutex_);
  const int64_t now = NowSeconds();
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    if (it->second.expires_at <= now) {
      it = tickets_.erase(it);
    } else {
      ++it;
    }
  }
  StreamTicket ticket;
  ticket.user_id = user_id;
  ticket.channel_id = channel_id;
  ticket.stream = stream;
  ticket.expires_at = *expires_at;
  tickets_[*token] = ticket;
  return true;
}

bool App::VerifyRtspCredentials(const std::string& username, const std::string& password) {
  uvcpp::uvcpp_db_table table;
  uvcpp::uvcpp_db_params params;
  params.add(username);
  std::string error;
  if (!Query("SELECT u.password_hash, u.enabled, r.permissions FROM users u "
             "JOIN roles r ON r.id = u.role_id WHERE u.username = ? COLLATE NOCASE",
             params, &table, &error) ||
      table.empty() || TableInt(table, 0, "enabled") == 0) {
    return false;
  }
  if (!VerifyPassword(password, TableText(table, 0, "password_hash")))
    return false;
  UserContext user;
  user.permissions = ParsePermissions(TableText(table, 0, "permissions"));
  return HasPermission(user, "rtsp.access");
}

bool App::CreatePlaybackTicket(int64_t user_id, int64_t channel_id, const std::string& stream,
                               int64_t start_ms, int64_t end_ms, double speed, std::string* token,
                               int64_t* expires_at, std::string* error) {
  if (stream != "main" && stream != "sub") {
    if (error)
      *error = "stream must be main or sub";
    return false;
  }
  *token = RandomHex(24);
  if (token->empty()) {
    if (error)
      *error = "cannot generate a playback ticket";
    return false;
  }
  *expires_at = NowSeconds() + 60;
  std::lock_guard<std::mutex> lock(tickets_mutex_);
  const int64_t now = NowSeconds();
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    if (it->second.expires_at <= now) {
      it = tickets_.erase(it);
    } else {
      ++it;
    }
  }
  StreamTicket ticket;
  ticket.user_id = user_id;
  ticket.channel_id = channel_id;
  ticket.stream = stream;
  ticket.expires_at = *expires_at;
  ticket.playback = true;
  ticket.start_ms = start_ms;
  ticket.end_ms = end_ms;
  ticket.speed = speed;
  tickets_[*token] = ticket;
  return true;
}

bool App::ValidateStreamTicket(const std::string& token, StreamTicket* ticket) {
  std::lock_guard<std::mutex> lock(tickets_mutex_);
  const auto it = tickets_.find(token);
  if (it == tickets_.end())
    return false;
  if (it->second.expires_at <= NowSeconds()) {
    tickets_.erase(it);
    return false;
  }
  *ticket = it->second;
  tickets_.erase(it);
  return true;
}

void App::RegisterChannelRoutes() {
  web_.get("/api/v1/channels",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto items = std::make_shared<json>(json::array());
                 RunWork(
                     [this, items] {
                       std::string error;
                       const auto channels = LoadChannels(&error);
                       for (const auto& channel : channels) {
                         json item = ChannelJson(channel);
                         StreamStatus main_status;
                         StreamStatus sub_status;
                         if (media_) {
                           main_status = media_->Status(channel.id, "main");
                           sub_status = media_->Status(channel.id, "sub");
                         }
                         item["mainState"] = main_status.state;
                         item["mainError"] = main_status.error;
                         item["mainHasKeyframe"] = main_status.has_keyframe;
                         item["subState"] = sub_status.state;
                         item["subError"] = sub_status.error;
                         item["subHasKeyframe"] = sub_status.has_keyframe;
                         items->push_back(std::move(item));
                       }
                     },
                     [items, &resp, next] {
                       SendJson(resp, 200, *items);
                       next();
                     });
               },
               "channel.view"));

  web_.get("/api/v1/preview/channels",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto items = std::make_shared<json>(json::array());
                 RunWork(
                     [this, items] {
                       std::string error;
                       const auto channels = LoadChannels(&error);
                       for (const auto& channel : channels) {
                         json item = ChannelJson(channel);
                         // Preview clients only need display metadata; URLs,
                         // usernames and stored error text stay behind
                         // channel.view.
                         for (const char* key : {"username", "hasPassword", "mainUrl", "subUrl",
                                                 "brand", "media", "lastStatus", "lastError"}) {
                           item.erase(key);
                         }
                         StreamStatus main_status;
                         StreamStatus sub_status;
                         if (media_) {
                           main_status = media_->Status(channel.id, "main");
                           sub_status = media_->Status(channel.id, "sub");
                         }
                         item["mainState"] = main_status.state;
                         item["mainError"] = main_status.error;
                         item["mainHasKeyframe"] = main_status.has_keyframe;
                         item["subState"] = sub_status.state;
                         item["subError"] = sub_status.error;
                         item["subHasKeyframe"] = sub_status.has_keyframe;
                         items->push_back(std::move(item));
                       }
                     },
                     [items, &resp, next] {
                       SendJson(resp, 200, *items);
                       next();
                     });
               },
               "preview.view"));

  web_.post(
      "/api/v1/channels",
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
            const std::string protocol = JsonString(body, "protocol", "rtsp");
            const std::string host = JsonString(body, "host");
            const int64_t port = JsonInt(body, "port", 554);
            const std::string username = JsonString(body, "username");
            const std::string password = JsonString(body, "password");
            const std::string main_url = JsonString(body, "mainUrl");
            const std::string sub_url = JsonString(body, "subUrl");
            const bool sub_enabled = JsonBool(body, "subEnabled", !sub_url.empty());
            const bool enabled = JsonBool(body, "enabled", true);
            const int64_t group_id = JsonInt(body, "groupId", 0);
            const std::string brand = JsonString(body, "brand");
            json media = body.contains("media") && body["media"].is_object() ? body["media"]
                                                                             : json::object();
            if (body.contains("eventsEnabled"))
              media["events"] = JsonBool(body, "eventsEnabled", false);
            const json watermark = body.contains("watermark") && body["watermark"].is_object()
                                       ? body["watermark"]
                                       : json::object();
            if (name.empty() || name.size() > 64) {
              SendError(resp, 400, "invalid_name", "channel name is required (max 64 characters)");
              next();
              return;
            }
            if (port < 1 || port > 65535) {
              SendError(resp, 400, "invalid_port", "port must be between 1 and 65535");
              next();
              return;
            }
            if (main_url.empty() && protocol != "hikvision" && protocol != "dahua") {
              SendError(resp, 400, "missing_url", "mainUrl is required for this protocol");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, name, protocol, host, port, username, password, main_url, sub_url,
                 sub_enabled, enabled, group_id, brand, media, watermark, actor, ip, ua, op] {
                  if (group_id > 0) {
                    uvcpp::uvcpp_db_table group_table;
                    uvcpp::uvcpp_db_params group_params;
                    group_params.add(group_id);
                    std::string group_error;
                    if (!Query("SELECT id FROM channel_groups WHERE id = ?", group_params,
                               &group_table, &group_error) ||
                        group_table.empty()) {
                      op->ok = false;
                      op->status = 400;
                      op->code = "invalid_group";
                      op->message = "the channel group does not exist";
                      return;
                    }
                  }
                  std::string encrypt_error;
                  const std::string password_enc = EncryptSecret(password, &encrypt_error);
                  if (!password.empty() && password_enc.empty()) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "encrypt_failed";
                    op->message = encrypt_error;
                    return;
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(name);
                  params.add(protocol);
                  params.add(host);
                  params.add(port);
                  params.add(username);
                  params.add(password_enc);
                  params.add(main_url);
                  params.add(sub_url);
                  params.add(sub_enabled ? 1 : 0);
                  params.add(enabled ? 1 : 0);
                  params.add(brand);
                  params.add(media.dump());
                  params.add(watermark.dump());
                  params.add(NowSeconds());
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert("INSERT INTO channels(name, protocol, host, "
                                                     "port, username, password_enc, "
                                                     "main_url, sub_url, sub_enabled, enabled, "
                                                     "brand, media_json, watermark_json, "
                                                     "created_at, updated_at) VALUES(?, ?, ?, ?, "
                                                     "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                                                     params, &new_id))) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the channel could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  if (group_id > 0) {
                    uvcpp::uvcpp_db_params member;
                    member.add(id);
                    member.add(group_id);
                    if (!Exec("INSERT OR REPLACE INTO channel_group_members(channel_id, group_id) "
                              "VALUES(?, ?)",
                              member, nullptr, &error)) {
                      op->ok = false;
                      op->status = 500;
                      op->code = "database_error";
                      op->message = "the channel group could not be assigned";
                      return;
                    }
                  }
                  Audit("channel", "info", actor.username, ip, ua, "channel", std::to_string(id),
                        "create", "success", "", json{{"name", name}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id}, {"name", name}, {"groupId", group_id}};
                },
                [this, op, &resp, next] {
                  if (op->ok && op->data.contains("id")) {
                    SyncMediaChannel(op->data["id"].get<int64_t>());
                  }
                  SendOp(resp, *op);
                  next();
                });
          },
          "channel.create"));

  web_.put(
      "/api/v1/channels/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t channel_id = 0;
            if (!ParseIntParam(id_param, &channel_id) || channel_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid channel id");
              next();
              return;
            }
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const bool has_password = body.contains("password");
            const std::string password = JsonString(body, "password");
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, channel_id, body, has_password, password, actor, ip, ua, op] {
                  ChannelRow existing;
                  std::string error;
                  if (!LoadChannel(channel_id, &existing, &error)) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "channel not found";
                    return;
                  }
                  const std::string name =
                      body.contains("name") ? JsonString(body, "name") : existing.name;
                  const std::string protocol =
                      body.contains("protocol") ? JsonString(body, "protocol") : existing.protocol;
                  const std::string host =
                      body.contains("host") ? JsonString(body, "host") : existing.host;
                  const int64_t port =
                      body.contains("port") ? JsonInt(body, "port", existing.port) : existing.port;
                  const std::string username =
                      body.contains("username") ? JsonString(body, "username") : existing.username;
                  const std::string main_url =
                      body.contains("mainUrl") ? JsonString(body, "mainUrl") : existing.main_url;
                  const std::string sub_url =
                      body.contains("subUrl") ? JsonString(body, "subUrl") : existing.sub_url;
                  const bool sub_enabled = body.contains("subEnabled")
                                               ? JsonBool(body, "subEnabled", existing.sub_enabled)
                                               : existing.sub_enabled;
                  const bool enabled = body.contains("enabled")
                                           ? JsonBool(body, "enabled", existing.enabled)
                                           : existing.enabled;
                  const std::string brand =
                      body.contains("brand") ? JsonString(body, "brand") : existing.brand;
                  std::string media_json = body.contains("media") && body["media"].is_object()
                                               ? body["media"].dump()
                                               : existing.media_json;
                  if (body.contains("eventsEnabled")) {
                    json media = json::parse(media_json, nullptr, false);
                    if (!media.is_object())
                      media = json::object();
                    media["events"] = JsonBool(body, "eventsEnabled", false);
                    media_json = media.dump();
                  }
                  const std::string watermark_json =
                      body.contains("watermark") && body["watermark"].is_object()
                          ? body["watermark"].dump()
                          : existing.watermark_json;
                  const int64_t group_id = body.contains("groupId")
                                               ? JsonInt(body, "groupId", existing.group_id)
                                               : existing.group_id;
                  if (name.empty() || name.size() > 64 || port < 1 || port > 65535) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_channel";
                    op->message = "invalid name or port";
                    return;
                  }
                  if (group_id > 0) {
                    uvcpp::uvcpp_db_table group_table;
                    uvcpp::uvcpp_db_params group_params;
                    group_params.add(group_id);
                    std::string group_error;
                    if (!Query("SELECT id FROM channel_groups WHERE id = ?", group_params,
                               &group_table, &group_error) ||
                        group_table.empty()) {
                      op->ok = false;
                      op->status = 400;
                      op->code = "invalid_group";
                      op->message = "the channel group does not exist";
                      return;
                    }
                  }
                  std::string password_enc = existing.password_enc;
                  if (has_password && !password.empty()) {
                    std::string encrypt_error;
                    password_enc = EncryptSecret(password, &encrypt_error);
                    if (password_enc.empty()) {
                      op->ok = false;
                      op->status = 500;
                      op->code = "encrypt_failed";
                      op->message = encrypt_error;
                      return;
                    }
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(name);
                  params.add(protocol);
                  params.add(host);
                  params.add(port);
                  params.add(username);
                  params.add(password_enc);
                  params.add(main_url);
                  params.add(sub_url);
                  params.add(sub_enabled ? 1 : 0);
                  params.add(enabled ? 1 : 0);
                  params.add(brand);
                  params.add(media_json);
                  params.add(watermark_json);
                  params.add(NowSeconds());
                  params.add(channel_id);
                  if (!Exec("UPDATE channels SET name=?, protocol=?, host=?, port=?, username=?, "
                            "password_enc=?, main_url=?, sub_url=?, sub_enabled=?, enabled=?, "
                            "brand=?, "
                            "media_json=?, watermark_json=?, updated_at=? WHERE id=?",
                            params, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the channel could not be updated";
                    return;
                  }
                  uvcpp::uvcpp_db_params member_delete;
                  member_delete.add(channel_id);
                  Exec("DELETE FROM channel_group_members WHERE channel_id = ?", member_delete,
                       nullptr, &error);
                  if (group_id > 0) {
                    uvcpp::uvcpp_db_params member;
                    member.add(channel_id);
                    member.add(group_id);
                    if (!Exec("INSERT OR REPLACE INTO channel_group_members(channel_id, group_id) "
                              "VALUES(?, ?)",
                              member, nullptr, &error)) {
                      op->ok = false;
                      op->status = 500;
                      op->code = "database_error";
                      op->message = "the channel group could not be assigned";
                      return;
                    }
                  }
                  Audit("channel", "info", actor.username, ip, ua, "channel",
                        std::to_string(channel_id), "update", "success", "",
                        json{{"name", name}}.dump());
                  op->data = json{{"id", channel_id}, {"name", name}, {"groupId", group_id}};
                },
                [this, op, channel_id, &resp, next] {
                  if (op->ok)
                    SyncMediaChannel(channel_id);
                  SendOp(resp, *op);
                  next();
                });
          },
          "channel.manage"));

  web_.del("/api/v1/channels/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t channel_id = 0;
                 if (!ParseIntParam(id_param, &channel_id) || channel_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid channel id");
                   next();
                   return;
                 }
                 const UserContext actor = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, channel_id, actor, ip, ua, op] {
                       ChannelRow channel;
                       std::string error;
                       if (!LoadChannel(channel_id, &channel, &error)) {
                         op->ok = false;
                         op->status = 404;
                         op->code = "not_found";
                         op->message = "channel not found";
                         return;
                       }
                       uvcpp::uvcpp_db_params params;
                       params.add(channel_id);
                       Exec("DELETE FROM record_plans WHERE channel_id = ?", params, nullptr,
                            &error);
                       Exec("DELETE FROM channel_group_members WHERE channel_id = ?", params,
                            nullptr, &error);
                       if (!Exec("DELETE FROM channels WHERE id = ?", params, nullptr, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the channel could not be deleted";
                         return;
                       }
                       Audit("channel", "warning", actor.username, ip, ua, "channel",
                             std::to_string(channel_id), "delete", "success", "",
                             json{{"name", channel.name}}.dump());
                       op->data = json{{"ok", true}};
                     },
                     [this, op, channel_id, &resp, next] {
                       if (op->ok) {
                         if (media_)
                           media_->Remove(channel_id);
                         if (onvif_events_)
                           onvif_events_->Remove(channel_id);
                       }
                       SendOp(resp, *op);
                       next();
                     });
               },
               "channel.delete"));

  web_.get("/api/v1/channels/:id/snapshot",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t channel_id = 0;
                 if (!ParseIntParam(id_param, &channel_id) || channel_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid channel id");
                   next();
                   return;
                 }
                 auto payload = std::make_shared<std::string>();
                 auto status = std::make_shared<int>(200);
                 auto message = std::make_shared<std::string>();
                 RunWork(
                     [this, channel_id, payload, status, message] {
                       ChannelRow channel;
                       std::string error;
                       if (!LoadChannel(channel_id, &channel, &error) || !channel.enabled) {
                         *status = 404;
                         *message = "channel not found or disabled";
                         return;
                       }
                       const std::string url =
                           BuildChannelUrl(channel, channel.sub_enabled, &error);
                       if (url.empty()) {
                         *status = 400;
                         *message = error;
                         return;
                       }
                       if (!media_ || !media_->CaptureJpeg(url, payload.get(), &error)) {
                         *status = 502;
                         *message = error.empty() ? "camera snapshot failed" : error;
                       }
                     },
                     [payload, status, message, &resp, next] {
                       if (*status != 200) {
                         SendError(resp, *status, "snapshot_failed", *message);
                       } else {
                         resp.set_header("Content-Type", "image/jpeg");
                         resp.status(200).body(*payload, "image/jpeg");
                         resp.end();
                       }
                       next();
                     });
               },
               "api.snapshot"));

  web_.post("/api/v1/channels/:id/reconnect",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const std::string* id_param = req.param("id");
                  int64_t channel_id = 0;
                  if (!ParseIntParam(id_param, &channel_id) || channel_id <= 0) {
                    SendError(resp, 400, "invalid_id", "invalid channel id");
                    next();
                    return;
                  }
                  if (media_)
                    media_->Reconnect(channel_id);
                  SendJson(resp, 200, json{{"ok", true}});
                  next();
                },
                "channel.manage"));

  web_.post("/api/v1/channels/sync",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const json body = json::parse(req.body_str(), nullptr, false);
                  if (body.is_discarded() || !body.is_object() || !body.contains("targetIds") ||
                      !body["targetIds"].is_array()) {
                    SendError(resp, 400, "invalid_json", "targetIds is required");
                    next();
                    return;
                  }
                  const int64_t source_id = JsonInt(body, "sourceId", 0);
                  std::vector<int64_t> targets;
                  for (const auto& item : body["targetIds"]) {
                    if (item.is_number_integer())
                      targets.push_back(item.get<int64_t>());
                  }
                  if (source_id <= 0 || targets.empty()) {
                    SendError(resp, 400, "invalid_targets", "sourceId and targetIds are required");
                    next();
                    return;
                  }
                  const UserContext actor = UserFromRequest(req);
                  const std::string ip = req.peer_ip();
                  const std::string ua = req.header("User-Agent", "");
                  auto op = std::make_shared<OpResult>();
                  RunWork(
                      [this, source_id, targets, actor, ip, ua, op] {
                        ChannelRow source;
                        std::string error;
                        if (!LoadChannel(source_id, &source, &error)) {
                          op->ok = false;
                          op->status = 404;
                          op->code = "not_found";
                          op->message = "source channel not found";
                          return;
                        }
                        for (const int64_t target : targets) {
                          ChannelRow existing;
                          if (!LoadChannel(target, &existing, &error))
                            continue;
                          uvcpp::uvcpp_db_params params;
                          params.add(source.protocol);
                          params.add(source.host);
                          params.add(source.port);
                          params.add(source.username);
                          params.add(source.password_enc);
                          params.add(source.main_url);
                          params.add(source.sub_url);
                          params.add(source.sub_enabled ? 1 : 0);
                          params.add(source.brand);
                          params.add(source.media_json);
                          params.add(source.watermark_json);
                          params.add(NowSeconds());
                          params.add(target);
                          Exec("UPDATE channels SET protocol=?, host=?, port=?, username=?, "
                               "password_enc=?, "
                               "main_url=?, sub_url=?, sub_enabled=?, brand=?, media_json=?, "
                               "watermark_json=?, updated_at=? WHERE id=?",
                               params, nullptr, &error);
                        }
                        Audit("channel", "info", actor.username, ip, ua, "channel",
                              std::to_string(source_id), "sync", "success", "",
                              json{{"targets", targets}}.dump());
                        op->data = json{{"ok", true}, {"targets", targets}};
                      },
                      [this, op, targets, &resp, next] {
                        if (op->ok) {
                          for (const int64_t target : targets)
                            SyncMediaChannel(target);
                        }
                        SendOp(resp, *op);
                        next();
                      });
                },
                "channel.manage"));
}

void App::RegisterGroupRoutes() {
  web_.get("/api/v1/channel-groups",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, op] {
                       uvcpp::uvcpp_db_table table;
                       std::string error;
                       if (!Query("SELECT g.id, g.name, g.parent_id, g.sort, "
                                  "COUNT(m.channel_id) AS channels "
                                  "FROM channel_groups g LEFT JOIN channel_group_members m "
                                  "ON m.group_id = g.id GROUP BY g.id "
                                  "ORDER BY g.sort, g.name COLLATE NOCASE",
                                  {}, &table, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = error;
                         return;
                       }
                       json items = json::array();
                       for (size_t row = 0; row < table.rows().size(); ++row) {
                         items.push_back(json{
                             {"id", TableInt(table, row, "id")},
                             {"name", TableText(table, row, "name")},
                             {"parentId", TableInt(table, row, "parent_id")},
                             {"sort", TableInt(table, row, "sort")},
                             {"channels", TableInt(table, row, "channels")},
                         });
                       }
                       op->data = std::move(items);
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               ""));

  web_.post(
      "/api/v1/channel-groups",
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
            const int64_t parent_id = JsonInt(body, "parentId", 0);
            const int64_t sort = JsonInt(body, "sort", 0);
            if (name.empty() || name.size() > 64) {
              SendError(resp, 400, "invalid_name", "group name is required (max 64 characters)");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, name, parent_id, sort, actor, ip, ua, op] {
                  std::string error;
                  if (parent_id > 0) {
                    uvcpp::uvcpp_db_table parent;
                    uvcpp::uvcpp_db_params params;
                    params.add(parent_id);
                    if (!Query("SELECT id FROM channel_groups WHERE id = ?", params, &parent,
                               &error) ||
                        parent.empty()) {
                      op->ok = false;
                      op->status = 400;
                      op->code = "invalid_parent";
                      op->message = "the parent group does not exist";
                      return;
                    }
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(name);
                  params.add(parent_id);
                  params.add(sort);
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  if (!uvcpp::uvcpp_db_ok(db_.insert(
                          "INSERT INTO channel_groups(name, parent_id, sort, created_at) "
                          "VALUES(?, ?, ?, ?)",
                          params, &new_id))) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the group could not be created";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("channel", "info", actor.username, ip, ua, "channel_group",
                        std::to_string(id), "create", "success", "", json{{"name", name}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id},
                                  {"name", name},
                                  {"parentId", parent_id},
                                  {"sort", sort},
                                  {"channels", 0}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "channel.manage"));

  web_.put(
      "/api/v1/channel-groups/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t group_id = 0;
            if (!ParseIntParam(id_param, &group_id) || group_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid group id");
              next();
              return;
            }
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, group_id, body, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table current;
                  uvcpp::uvcpp_db_params current_params;
                  current_params.add(group_id);
                  std::string error;
                  if (!Query("SELECT id, name, parent_id, sort FROM channel_groups WHERE id = ?",
                             current_params, &current, &error) ||
                      current.empty()) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "group not found";
                    return;
                  }
                  const std::string name = body.contains("name") ? JsonString(body, "name")
                                                                 : TableText(current, 0, "name");
                  const int64_t parent_id = body.contains("parentId")
                                                ? JsonInt(body, "parentId", 0)
                                                : TableInt(current, 0, "parent_id");
                  const int64_t sort = body.contains("sort") ? JsonInt(body, "sort", 0)
                                                             : TableInt(current, 0, "sort");
                  if (name.empty() || name.size() > 64) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_name";
                    op->message = "group name is required (max 64 characters)";
                    return;
                  }
                  if (parent_id == group_id) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "invalid_parent";
                    op->message = "a group cannot be its own parent";
                    return;
                  }
                  int64_t cursor = parent_id;
                  for (int depth = 0; depth < 64 && cursor > 0; ++depth) {
                    if (cursor == group_id) {
                      op->ok = false;
                      op->status = 400;
                      op->code = "invalid_parent";
                      op->message = "the parent would create a cycle";
                      return;
                    }
                    uvcpp::uvcpp_db_table parent;
                    uvcpp::uvcpp_db_params parent_params;
                    parent_params.add(cursor);
                    if (!Query("SELECT parent_id FROM channel_groups WHERE id = ?", parent_params,
                               &parent, &error) ||
                        parent.empty()) {
                      op->ok = false;
                      op->status = 400;
                      op->code = "invalid_parent";
                      op->message = "the parent group does not exist";
                      return;
                    }
                    cursor = TableInt(parent, 0, "parent_id");
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(name);
                  params.add(parent_id);
                  params.add(sort);
                  params.add(group_id);
                  if (!Exec("UPDATE channel_groups SET name = ?, parent_id = ?, sort = ? "
                            "WHERE id = ?",
                            params, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the group could not be updated";
                    return;
                  }
                  Audit("channel", "info", actor.username, ip, ua, "channel_group",
                        std::to_string(group_id), "update", "success", "",
                        json{{"name", name}}.dump());
                  op->data = json{
                      {"id", group_id}, {"name", name}, {"parentId", parent_id}, {"sort", sort}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "channel.manage"));

  web_.del(
      "/api/v1/channel-groups/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t group_id = 0;
            if (!ParseIntParam(id_param, &group_id) || group_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid group id");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, group_id, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table current;
                  uvcpp::uvcpp_db_params current_params;
                  current_params.add(group_id);
                  std::string error;
                  if (!Query("SELECT id, name, parent_id FROM channel_groups WHERE id = ?",
                             current_params, &current, &error) ||
                      current.empty()) {
                    op->ok = false;
                    op->status = 404;
                    op->code = "not_found";
                    op->message = "group not found";
                    return;
                  }
                  const int64_t parent_id = TableInt(current, 0, "parent_id");
                  uvcpp::uvcpp_db_params params;
                  params.add(group_id);
                  Exec("DELETE FROM channel_group_members WHERE group_id = ?", params, nullptr,
                       &error);
                  uvcpp::uvcpp_db_params reparent;
                  reparent.add(parent_id);
                  reparent.add(group_id);
                  Exec("UPDATE channel_groups SET parent_id = ? WHERE parent_id = ?", reparent,
                       nullptr, &error);
                  uvcpp::uvcpp_db_params remove;
                  remove.add(group_id);
                  if (!Exec("DELETE FROM channel_groups WHERE id = ?", remove, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the group could not be deleted";
                    return;
                  }
                  Audit("channel", "warning", actor.username, ip, ua, "channel_group",
                        std::to_string(group_id), "delete", "success", "",
                        json{{"name", TableText(current, 0, "name")}}.dump());
                  op->data = json{{"ok", true}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "channel.manage"));
}

void App::RegisterLayoutRoutes() {
  web_.get("/api/v1/layouts",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const UserContext user = UserFromRequest(req);
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, user, op] {
                       uvcpp::uvcpp_db_table table;
                       uvcpp::uvcpp_db_params params;
                       params.add(user.id);
                       std::string error;
                       if (!Query("SELECT id, name, grid_json, is_default, created_at, updated_at "
                                  "FROM layouts WHERE user_id = ? "
                                  "ORDER BY is_default DESC, updated_at DESC",
                                  params, &table, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = error;
                         return;
                       }
                       json items = json::array();
                       for (size_t row = 0; row < table.rows().size(); ++row) {
                         json grid =
                             json::parse(TableText(table, row, "grid_json"), nullptr, false);
                         if (grid.is_discarded() || !grid.is_object())
                           grid = json::object();
                         items.push_back(json{
                             {"id", TableInt(table, row, "id")},
                             {"name", TableText(table, row, "name")},
                             {"grid", grid},
                             {"isDefault", TableInt(table, row, "is_default") != 0},
                             {"createdAt", TableInt(table, row, "created_at")},
                             {"updatedAt", TableInt(table, row, "updated_at")},
                         });
                       }
                       op->data = std::move(items);
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "preview.view"));

  web_.post(
      "/api/v1/layouts",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object() || !body.contains("grid") ||
                !body["grid"].is_object()) {
              SendError(resp, 400, "invalid_json", "name and grid are required");
              next();
              return;
            }
            const std::string name = JsonString(body, "name");
            const json grid = body["grid"];
            const bool is_default = JsonBool(body, "isDefault", false);
            if (name.empty() || name.size() > 64 || grid.dump().size() > 64 * 1024) {
              SendError(resp, 400, "invalid_layout",
                        "layout name (max 64) or grid (max 64 KiB) is invalid");
              next();
              return;
            }
            const UserContext user = UserFromRequest(req);
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, user, name, grid, is_default, op] {
                  uvcpp::uvcpp_db_table count;
                  uvcpp::uvcpp_db_params count_params;
                  count_params.add(user.id);
                  std::string error;
                  if (!Query("SELECT COUNT(*) AS n FROM layouts WHERE user_id = ?", count_params,
                             &count, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = error;
                    return;
                  }
                  const int64_t existing = count.empty() ? 0 : TableInt(count, 0, "n");
                  if (existing >= 20) {
                    op->ok = false;
                    op->status = 409;
                    op->code = "too_many_layouts";
                    op->message = "up to 20 layouts are allowed per user";
                    return;
                  }
                  const bool make_default = is_default || existing == 0;
                  if (make_default) {
                    uvcpp::uvcpp_db_params clear;
                    clear.add(user.id);
                    Exec("UPDATE layouts SET is_default = 0 WHERE user_id = ?", clear, nullptr,
                         &error);
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(user.id);
                  params.add(name);
                  params.add(grid.dump());
                  params.add(make_default ? 1 : 0);
                  params.add(NowSeconds());
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  if (!uvcpp::uvcpp_db_ok(db_.insert(
                          "INSERT INTO layouts(user_id, name, grid_json, is_default, created_at, "
                          "updated_at) VALUES(?, ?, ?, ?, ?, ?)",
                          params, &new_id))) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the layout could not be created";
                    return;
                  }
                  op->status = 201;
                  op->data = json{{"id", new_id.to_int64()},
                                  {"name", name},
                                  {"grid", grid},
                                  {"isDefault", make_default}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "preview.view"));

  web_.put("/api/v1/layouts/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t layout_id = 0;
                 if (!ParseIntParam(id_param, &layout_id) || layout_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid layout id");
                   next();
                   return;
                 }
                 const json body = json::parse(req.body_str(), nullptr, false);
                 if (body.is_discarded() || !body.is_object()) {
                   SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                   next();
                   return;
                 }
                 const UserContext user = UserFromRequest(req);
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, user, layout_id, body, op] {
                       uvcpp::uvcpp_db_table current;
                       uvcpp::uvcpp_db_params current_params;
                       current_params.add(layout_id);
                       current_params.add(user.id);
                       std::string error;
                       if (!Query("SELECT id, name, grid_json, is_default FROM layouts "
                                  "WHERE id = ? AND user_id = ?",
                                  current_params, &current, &error) ||
                           current.empty()) {
                         op->ok = false;
                         op->status = 404;
                         op->code = "not_found";
                         op->message = "layout not found";
                         return;
                       }
                       const std::string name = body.contains("name")
                                                    ? JsonString(body, "name")
                                                    : TableText(current, 0, "name");
                       const std::string grid_json =
                           body.contains("grid") && body["grid"].is_object()
                               ? body["grid"].dump()
                               : TableText(current, 0, "grid_json");
                       const bool is_default = body.contains("isDefault")
                                                   ? JsonBool(body, "isDefault", false)
                                                   : TableInt(current, 0, "is_default") != 0;
                       if (name.empty() || name.size() > 64 || grid_json.size() > 64 * 1024) {
                         op->ok = false;
                         op->status = 400;
                         op->code = "invalid_layout";
                         op->message = "layout name (max 64) or grid (max 64 KiB) is invalid";
                         return;
                       }
                       if (is_default) {
                         uvcpp::uvcpp_db_params clear;
                         clear.add(user.id);
                         Exec("UPDATE layouts SET is_default = 0 WHERE user_id = ?", clear, nullptr,
                              &error);
                       }
                       uvcpp::uvcpp_db_params params;
                       params.add(name);
                       params.add(grid_json);
                       params.add(is_default ? 1 : 0);
                       params.add(NowSeconds());
                       params.add(layout_id);
                       if (!Exec("UPDATE layouts SET name = ?, grid_json = ?, is_default = ?, "
                                 "updated_at = ? WHERE id = ?",
                                 params, nullptr, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the layout could not be updated";
                         return;
                       }
                       op->data = json{{"id", layout_id},
                                       {"name", name},
                                       {"grid", json::parse(grid_json, nullptr, false)},
                                       {"isDefault", is_default}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "preview.view"));

  web_.del("/api/v1/layouts/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t layout_id = 0;
                 if (!ParseIntParam(id_param, &layout_id) || layout_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid layout id");
                   next();
                   return;
                 }
                 const UserContext user = UserFromRequest(req);
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, user, layout_id, op] {
                       uvcpp::uvcpp_db_params params;
                       params.add(layout_id);
                       params.add(user.id);
                       std::string error;
                       if (!Exec("DELETE FROM layouts WHERE id = ? AND user_id = ?", params,
                                 nullptr, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the layout could not be deleted";
                         return;
                       }
                       op->data = json{{"ok", true}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "preview.view"));
}

void App::RegisterOnvifRoutes() {
  web_.post("/api/v1/channels/discover",
            Protected(
                [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  auto op = std::make_shared<OpResult>();
                  RunWork(
                      [op] {
                        std::vector<OnvifDevice> devices;
                        std::string error;
                        if (!DiscoverOnvif(&devices, 3000, &error)) {
                          op->ok = false;
                          op->status = 502;
                          op->code = "discovery_failed";
                          op->message = error;
                          return;
                        }
                        json items = json::array();
                        for (const auto& device : devices) {
                          items.push_back(json{{"xaddr", device.xaddr}, {"ip", device.ip}});
                        }
                        op->data = json{{"devices", items}};
                      },
                      [op, &resp, next] {
                        SendOp(resp, *op);
                        next();
                      });
                },
                "channel.manage"));

  web_.post("/api/v1/channels/onvif/probe",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const json body = json::parse(req.body_str(), nullptr, false);
                  if (body.is_discarded() || !body.is_object()) {
                    SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                    next();
                    return;
                  }
                  const std::string xaddr = JsonString(body, "xaddr");
                  const std::string username = JsonString(body, "username");
                  const std::string password = JsonString(body, "password");
                  if (xaddr.empty()) {
                    SendError(resp, 400, "missing_xaddr", "xaddr is required");
                    next();
                    return;
                  }
                  auto op = std::make_shared<OpResult>();
                  RunWork(
                      [xaddr, username, password, op] {
                        OnvifInfo info;
                        std::string error;
                        if (!ProbeOnvif(xaddr, username, password, &info, &error)) {
                          op->ok = false;
                          op->status = 502;
                          op->code = "probe_failed";
                          op->message = error.empty()
                                            ? "the camera did not return usable ONVIF profiles"
                                            : error;
                          return;
                        }
                        json profiles = json::array();
                        for (const auto& profile : info.profiles) {
                          profiles.push_back(json{{"token", profile.token},
                                                  {"name", profile.name},
                                                  {"streamUri", profile.stream_uri}});
                        }
                        op->data = json{{"device", json{{"manufacturer", info.manufacturer},
                                                        {"model", info.model},
                                                        {"firmware", info.firmware},
                                                        {"serial", info.serial}}},
                                        {"profiles", profiles}};
                      },
                      [op, &resp, next] {
                        SendOp(resp, *op);
                        next();
                      });
                },
                "channel.manage"));
}

void App::RegisterStreamRoutes() {
  web_.post("/api/v1/streams/ticket",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const json body = json::parse(req.body_str(), nullptr, false);
                  if (body.is_discarded() || !body.is_object()) {
                    SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                    next();
                    return;
                  }
                  const int64_t channel_id = JsonInt(body, "channelId", 0);
                  const std::string stream = JsonString(body, "stream", "main");
                  if (channel_id <= 0 || (stream != "main" && stream != "sub")) {
                    SendError(resp, 400, "invalid_stream",
                              "channelId and stream (main/sub) are required");
                    next();
                    return;
                  }
                  const UserContext user = UserFromRequest(req);
                  auto result = std::make_shared<OpResult>();
                  RunWork(
                      [this, user, channel_id, stream, result] {
                        ChannelRow channel;
                        std::string error;
                        if (!LoadChannel(channel_id, &channel, &error) || !channel.enabled) {
                          result->ok = false;
                          result->status = 404;
                          result->code = "not_found";
                          result->message = "channel not found or disabled";
                          return;
                        }
                        if (stream == "sub" && !channel.sub_enabled) {
                          result->ok = false;
                          result->status = 400;
                          result->code = "sub_disabled";
                          result->message = "the sub stream is disabled for this channel";
                          return;
                        }
                        std::string token;
                        int64_t expires_at = 0;
                        if (!CreateStreamTicket(user.id, channel_id, stream, &token, &expires_at,
                                                &error)) {
                          result->ok = false;
                          result->status = 500;
                          result->code = "ticket_failed";
                          result->message = error;
                          return;
                        }
                        result->data = json{{"ticket", token},
                                            {"wsPath", "/ws/live?ticket=" + token},
                                            {"expiresAt", expires_at},
                                            {"channelId", channel_id},
                                            {"stream", stream}};
                      },
                      [result, &resp, next] {
                        SendOp(resp, *result);
                        next();
                      });
                },
                "preview.view"));

  web_.post("/api/v1/playback/ticket",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const json body = json::parse(req.body_str(), nullptr, false);
                  if (body.is_discarded() || !body.is_object()) {
                    SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                    next();
                    return;
                  }
                  const int64_t channel_id = JsonInt(body, "channelId", 0);
                  const std::string stream = JsonString(body, "stream", "main");
                  const int64_t start_ms = JsonInt(body, "startMs", 0);
                  const int64_t end_ms = JsonInt(body, "endMs", 0);
                  double speed = 1.0;
                  if (body.contains("speed")) {
                    if (!body["speed"].is_number()) {
                      SendError(resp, 400, "invalid_speed", "speed must be a number");
                      next();
                      return;
                    }
                    speed = body["speed"].get<double>();
                  }
                  if (channel_id <= 0 || (stream != "main" && stream != "sub")) {
                    SendError(resp, 400, "invalid_stream",
                              "channelId and stream (main/sub) are required");
                    next();
                    return;
                  }
                  if (speed < 0.25 || speed > 32.0) {
                    SendError(resp, 400, "invalid_speed", "speed must be between 0.25 and 32");
                    next();
                    return;
                  }
                  const UserContext user = UserFromRequest(req);
                  auto result = std::make_shared<OpResult>();
                  RunWork(
                      [this, user, channel_id, stream, start_ms, end_ms, speed, result] {
                        ChannelRow channel;
                        std::string error;
                        if (!LoadChannel(channel_id, &channel, &error) || !channel.enabled) {
                          result->ok = false;
                          result->status = 404;
                          result->code = "not_found";
                          result->message = "channel not found or disabled";
                          return;
                        }
                        std::string token;
                        int64_t expires_at = 0;
                        if (!CreatePlaybackTicket(user.id, channel_id, stream, start_ms, end_ms,
                                                  speed, &token, &expires_at, &error)) {
                          result->ok = false;
                          result->status = 500;
                          result->code = "ticket_failed";
                          result->message = error;
                          return;
                        }
                        result->data = json{{"ticket", token},
                                            {"wsPath", "/ws/playback?ticket=" + token},
                                            {"expiresAt", expires_at},
                                            {"channelId", channel_id},
                                            {"stream", stream}};
                      },
                      [result, &resp, next] {
                        SendOp(resp, *result);
                        next();
                      });
                },
                "playback.view"));

  web_.websocket("/ws/live", [this](uvcpp::uvcpp_web_ws_request& ws) {
    const std::string* ticket_value = ws.query("ticket");
    StreamTicket ticket;
    if (ticket_value == nullptr || !ValidateStreamTicket(*ticket_value, &ticket)) {
      if (auto* connection = ws.connection()) {
        const std::string message = R"({"error":"invalid_ticket"})";
        connection->send_text(message.data(), message.size());
        connection->close();
      }
      return;
    }
    std::string error;
    if (!media_ || !media_->Subscribe(ticket.channel_id, ticket.stream, ws.connection(), &error)) {
      if (auto* connection = ws.connection()) {
        const json message = json{{"error", "stream_unavailable"}, {"message", error}};
        const std::string text = message.dump();
        connection->send_text(text.data(), text.size());
        connection->close();
      }
    }
  });

  web_.websocket("/ws/playback", [this](uvcpp::uvcpp_web_ws_request& ws) {
    const std::string* ticket_value = ws.query("ticket");
    StreamTicket ticket;
    if (ticket_value == nullptr || !ValidateStreamTicket(*ticket_value, &ticket) ||
        !ticket.playback) {
      if (auto* connection = ws.connection()) {
        const std::string message = R"({"error":"invalid_ticket"})";
        connection->send_text(message.data(), message.size());
        connection->close();
      }
      return;
    }
    if (!playback_) {
      if (auto* connection = ws.connection()) {
        const std::string message = R"({"error":"playback_unavailable"})";
        connection->send_text(message.data(), message.size());
        connection->close();
      }
      return;
    }
    playback_->Open(ws.connection(), ticket.channel_id, ticket.stream, ticket.start_ms,
                    ticket.end_ms, ticket.speed);
  });
}

void App::RegisterPlanRoutes() {
  web_.get(
      "/api/v1/plans",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            int64_t channel_id = 0;
            if (const std::string* value = req.query("channelId")) {
              ParseIntParam(value, &channel_id);
            }
            auto items = std::make_shared<json>(json::array());
            RunWork(
                [this, channel_id, items] {
                  uvcpp::uvcpp_db_table table;
                  std::string error;
                  if (channel_id > 0) {
                    uvcpp::uvcpp_db_params params;
                    params.add(channel_id);
                    Query(
                        "SELECT id, channel_id, weekday, start_min, end_min, mode, stream, enabled "
                        "FROM record_plans WHERE channel_id = ? ORDER BY weekday, start_min",
                        params, &table, &error);
                  } else {
                    Query(
                        "SELECT id, channel_id, weekday, start_min, end_min, mode, stream, enabled "
                        "FROM record_plans ORDER BY channel_id, weekday, start_min",
                        {}, &table, &error);
                  }
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    items->push_back(json{
                        {"id", TableInt(table, i, "id")},
                        {"channelId", TableInt(table, i, "channel_id")},
                        {"weekday", TableInt(table, i, "weekday")},
                        {"startMin", TableInt(table, i, "start_min")},
                        {"endMin", TableInt(table, i, "end_min")},
                        {"mode", TableText(table, i, "mode")},
                        {"stream", TableText(table, i, "stream")},
                        {"enabled", TableInt(table, i, "enabled") != 0},
                    });
                  }
                },
                [items, &resp, next] {
                  SendJson(resp, 200, *items);
                  next();
                });
          },
          "plan.view"));

  web_.put("/api/v1/plans",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const json body = json::parse(req.body_str(), nullptr, false);
                 const int64_t channel_id = body.is_object() ? JsonInt(body, "channelId", 0) : 0;
                 if (!body.is_object() || channel_id <= 0 || !body.contains("plans") ||
                     !body["plans"].is_array()) {
                   SendError(resp, 400, "invalid_json", "channelId and plans are required");
                   next();
                   return;
                 }
                 struct Plan {
                   int weekday = 0;
                   int start_min = 0;
                   int end_min = 0;
                   std::string mode;
                   std::string stream;
                   bool enabled = true;
                 };
                 std::vector<Plan> plans;
                 for (const auto& item : body["plans"]) {
                   if (!item.is_object())
                     continue;
                   Plan plan;
                   plan.weekday = static_cast<int>(JsonInt(item, "weekday", -1));
                   plan.start_min = static_cast<int>(JsonInt(item, "startMin", -1));
                   plan.end_min = static_cast<int>(JsonInt(item, "endMin", -1));
                   plan.mode = JsonString(item, "mode", "timed");
                   plan.stream = JsonString(item, "stream", "main");
                   plan.enabled = JsonBool(item, "enabled", true);
                   if (plan.weekday < 0 || plan.weekday > 6 || plan.start_min < 0 ||
                       plan.start_min > 1439 || plan.end_min < 0 || plan.end_min > 1440) {
                     continue;
                   }
                   if (plan.mode != "timed" && plan.mode != "event" && plan.mode != "both")
                     continue;
                   if (plan.stream != "main" && plan.stream != "sub")
                     plan.stream = "main";
                   plans.push_back(std::move(plan));
                 }
                 const UserContext actor = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, channel_id, plans, actor, ip, ua, op] {
                       uvcpp::uvcpp_db_params del;
                       del.add(channel_id);
                       std::string error;
                       if (!Exec("DELETE FROM record_plans WHERE channel_id = ?", del, nullptr,
                                 &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "existing plans could not be replaced";
                         return;
                       }
                       for (const auto& plan : plans) {
                         uvcpp::uvcpp_db_params params;
                         params.add(channel_id);
                         params.add(plan.weekday);
                         params.add(plan.start_min);
                         params.add(plan.end_min);
                         params.add(plan.mode);
                         params.add(plan.stream);
                         params.add(plan.enabled ? 1 : 0);
                         params.add(NowSeconds());
                         if (!Exec("INSERT INTO record_plans(channel_id, weekday, start_min, "
                                   "end_min, mode, "
                                   "stream, enabled, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                                   params, nullptr, &error)) {
                           op->ok = false;
                           op->status = 500;
                           op->code = "database_error";
                           op->message = "a plan could not be stored";
                           return;
                         }
                       }
                       Audit("record", "info", actor.username, ip, ua, "channel",
                             std::to_string(channel_id), "plan_update", "success", "",
                             json{{"count", plans.size()}}.dump());
                       op->data = json{{"ok", true}, {"count", plans.size()}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "plan.manage"));

  web_.post(
      "/api/v1/plans/sync",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            const int64_t source_id = body.is_object() ? JsonInt(body, "sourceId", 0) : 0;
            if (!body.is_object() || source_id <= 0 || !body.contains("targetIds") ||
                !body["targetIds"].is_array()) {
              SendError(resp, 400, "invalid_json", "sourceId and targetIds are required");
              next();
              return;
            }
            std::vector<int64_t> targets;
            for (const auto& item : body["targetIds"]) {
              if (item.is_number_integer())
                targets.push_back(item.get<int64_t>());
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, source_id, targets, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table source;
                  uvcpp::uvcpp_db_params params;
                  params.add(source_id);
                  std::string error;
                  if (!Query("SELECT weekday, start_min, end_min, mode, stream, enabled FROM "
                             "record_plans WHERE channel_id = ?",
                             params, &source, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "source plans could not be read";
                    return;
                  }
                  for (const int64_t target : targets) {
                    uvcpp::uvcpp_db_params del;
                    del.add(target);
                    Exec("DELETE FROM record_plans WHERE channel_id = ?", del, nullptr, &error);
                    for (size_t i = 0; i < source.rows().size(); ++i) {
                      uvcpp::uvcpp_db_params insert;
                      insert.add(target);
                      insert.add(TableInt(source, i, "weekday"));
                      insert.add(TableInt(source, i, "start_min"));
                      insert.add(TableInt(source, i, "end_min"));
                      insert.add(TableText(source, i, "mode"));
                      insert.add(TableText(source, i, "stream"));
                      insert.add(TableInt(source, i, "enabled"));
                      insert.add(NowSeconds());
                      Exec(
                          "INSERT INTO record_plans(channel_id, weekday, start_min, end_min, mode, "
                          "stream, enabled, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                          insert, nullptr, &error);
                    }
                  }
                  Audit("record", "info", actor.username, ip, ua, "channel",
                        std::to_string(source_id), "plan_sync", "success", "",
                        json{{"targets", targets}}.dump());
                  op->data = json{{"ok", true}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "plan.manage"));
}

void App::RegisterStorageRoutes() {
  web_.get(
      "/api/v1/storage/volumes",
      Protected(
          [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            auto items = std::make_shared<json>(json::array());
            RunWork(
                [this, items] {
                  uvcpp::uvcpp_db_table table;
                  std::string error;
                  if (!Query("SELECT id, path, label, mode, max_usage_pct, reserve_gb, "
                             "retention_days, "
                             "enabled FROM storage_volumes ORDER BY id",
                             {}, &table, &error)) {
                    return;
                  }
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    const std::string path = TableText(table, i, "path");
                    json item{
                        {"id", TableInt(table, i, "id")},
                        {"path", path},
                        {"label", TableText(table, i, "label")},
                        {"mode", TableText(table, i, "mode")},
                        {"maxUsagePct", TableInt(table, i, "max_usage_pct")},
                        {"reserveGb", TableInt(table, i, "reserve_gb")},
                        {"retentionDays", TableInt(table, i, "retention_days")},
                        {"enabled", TableInt(table, i, "enabled") != 0},
                        {"totalBytes", 0},
                        {"freeBytes", 0},
                        {"usagePercent", 0.0},
                    };
#ifdef _WIN32
                    ULARGE_INTEGER free_bytes{};
                    ULARGE_INTEGER total_bytes{};
                    if (GetDiskFreeSpaceExA(path.c_str(), &free_bytes, &total_bytes, nullptr)) {
                      item["totalBytes"] = total_bytes.QuadPart;
                      item["freeBytes"] = free_bytes.QuadPart;
                    }
#else
                    struct statvfs stat {};
                    if (statvfs(path.c_str(), &stat) == 0) {
                      const uint64_t total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
                      const uint64_t free = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
                      item["totalBytes"] = total;
                      item["freeBytes"] = free;
                    }
#endif
                    const uint64_t total = item["totalBytes"].get<uint64_t>();
                    const uint64_t free = item["freeBytes"].get<uint64_t>();
                    if (total > 0)
                      item["usagePercent"] = 100.0 * (1.0 - static_cast<double>(free) / total);
                    items->push_back(std::move(item));
                  }
                },
                [items, &resp, next] {
                  SendJson(resp, 200, *items);
                  next();
                });
          },
          "storage.view"));

  web_.post(
      "/api/v1/storage/volumes",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const std::string path = JsonString(body, "path");
            const std::string label = JsonString(body, "label", "Volume");
            const std::string mode = JsonString(body, "mode", "loop");
            const int64_t max_usage = JsonInt(body, "maxUsagePct", 90);
            const int64_t reserve = JsonInt(body, "reserveGb", 5);
            const int64_t retention = JsonInt(body, "retentionDays", 30);
            if (path.empty() || path.size() > 1024 || (mode != "loop" && mode != "alert") ||
                max_usage < 50 || max_usage > 99 || reserve < 0 || reserve > 1000 ||
                retention < 0 || retention > 3650) {
              SendError(resp, 400, "invalid_volume", "invalid volume settings");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, path, label, mode, max_usage, reserve, retention, actor, ip, ua, op] {
                  std::error_code ec;
                  std::filesystem::create_directories(path, ec);
                  if (ec) {
                    op->ok = false;
                    op->status = 400;
                    op->code = "path_unusable";
                    op->message = "cannot create the storage directory: " + ec.message();
                    return;
                  }
                  uvcpp::uvcpp_db_params params;
                  params.add(path);
                  params.add(label);
                  params.add(mode);
                  params.add(max_usage);
                  params.add(reserve);
                  params.add(retention);
                  params.add(1);
                  params.add(NowSeconds());
                  uvcpp::uvcpp_db_value new_id;
                  std::string error;
                  if (!uvcpp::uvcpp_db_ok(db_.insert(
                          "INSERT INTO storage_volumes(path, label, mode, max_usage_pct, "
                          "reserve_gb, "
                          "retention_days, enabled, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                          params, &new_id))) {
                    op->ok = false;
                    op->status = 409;
                    op->code = "volume_exists";
                    op->message = "that storage path is already configured";
                    return;
                  }
                  const int64_t id = new_id.to_int64();
                  Audit("storage", "info", actor.username, ip, ua, "volume", std::to_string(id),
                        "create", "success", "", json{{"path", path}}.dump());
                  op->status = 201;
                  op->data = json{{"id", id}, {"path", path}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "storage.manage"));

  web_.put("/api/v1/storage/volumes/:id",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t volume_id = 0;
                 if (!ParseIntParam(id_param, &volume_id) || volume_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid volume id");
                   next();
                   return;
                 }
                 const json body = json::parse(req.body_str(), nullptr, false);
                 if (body.is_discarded() || !body.is_object()) {
                   SendError(resp, 400, "invalid_json", "request body must be a JSON object");
                   next();
                   return;
                 }
                 const std::string label = JsonString(body, "label", "Volume");
                 const std::string mode = JsonString(body, "mode", "loop");
                 const int64_t max_usage = JsonInt(body, "maxUsagePct", 90);
                 const int64_t reserve = JsonInt(body, "reserveGb", 5);
                 const int64_t retention = JsonInt(body, "retentionDays", 30);
                 const bool enabled = JsonBool(body, "enabled", true);
                 if ((mode != "loop" && mode != "alert") || max_usage < 50 || max_usage > 99 ||
                     reserve < 0 || reserve > 1000 || retention < 1 || retention > 3650) {
                   SendError(resp, 400, "invalid_volume", "invalid volume settings");
                   next();
                   return;
                 }
                 const UserContext actor = UserFromRequest(req);
                 const std::string ip = req.peer_ip();
                 const std::string ua = req.header("User-Agent", "");
                 auto op = std::make_shared<OpResult>();
                 RunWork(
                     [this, volume_id, label, mode, max_usage, reserve, retention, enabled, actor,
                      ip, ua, body, op] {
                       uvcpp::uvcpp_db_params params;
                       params.add(label);
                       params.add(mode);
                       params.add(max_usage);
                       params.add(reserve);
                       params.add(retention);
                       params.add(enabled ? 1 : 0);
                       params.add(volume_id);
                       std::string error;
                       if (!Exec("UPDATE storage_volumes SET label=?, mode=?, max_usage_pct=?, "
                                 "reserve_gb=?, "
                                 "retention_days=?, enabled=? WHERE id=?",
                                 params, nullptr, &error)) {
                         op->ok = false;
                         op->status = 500;
                         op->code = "database_error";
                         op->message = "the volume could not be updated";
                         return;
                       }
                       Audit("storage", "info", actor.username, ip, ua, "volume",
                             std::to_string(volume_id), "update", "success", "", body.dump());
                       op->data = json{{"ok", true}};
                     },
                     [op, &resp, next] {
                       SendOp(resp, *op);
                       next();
                     });
               },
               "storage.manage"));

  web_.del(
      "/api/v1/storage/volumes/:id",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t volume_id = 0;
            if (!ParseIntParam(id_param, &volume_id) || volume_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid volume id");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            const std::string ip = req.peer_ip();
            const std::string ua = req.header("User-Agent", "");
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, volume_id, actor, ip, ua, op] {
                  uvcpp::uvcpp_db_table table;
                  uvcpp::uvcpp_db_params params;
                  params.add(volume_id);
                  std::string error;
                  if (!Query("SELECT COUNT(*) AS n FROM recordings WHERE volume_id = ?", params,
                             &table, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the volume could not be checked";
                    return;
                  }
                  if (!table.empty() && TableInt(table, 0, "n") > 0) {
                    op->ok = false;
                    op->status = 409;
                    op->code = "volume_in_use";
                    op->message = "the volume still contains recordings";
                    return;
                  }
                  if (!Exec("DELETE FROM storage_volumes WHERE id = ?", params, nullptr, &error)) {
                    op->ok = false;
                    op->status = 500;
                    op->code = "database_error";
                    op->message = "the volume could not be deleted";
                    return;
                  }
                  Audit("storage", "warning", actor.username, ip, ua, "volume",
                        std::to_string(volume_id), "delete", "success", "", "{}");
                  op->data = json{{"ok", true}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "storage.manage"));
}

void App::RegisterRecordingRoutes() {
  web_.get("/api/v1/recordings",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 int64_t channel_id = 0;
                 int64_t from_ms = 0;
                 int64_t to_ms = 0;
                 if (const std::string* value = req.query("channelId"))
                   ParseIntParam(value, &channel_id);
                 if (const std::string* value = req.query("from"))
                   ParseIntParam(value, &from_ms);
                 if (const std::string* value = req.query("to"))
                   ParseIntParam(value, &to_ms);
                 const std::string stream = req.query("stream") ? *req.query("stream") : "";
                 auto items = std::make_shared<json>(json::array());
                 RunWork(
                     [this, channel_id, from_ms, to_ms, stream, items] {
                       std::string where = " WHERE 1=1";
                       uvcpp::uvcpp_db_params params;
                       if (channel_id > 0) {
                         where += " AND channel_id = ?";
                         params.add(channel_id);
                       }
                       if (!stream.empty()) {
                         where += " AND stream = ?";
                         params.add(stream);
                       }
                       if (to_ms > 0) {
                         where += " AND start_ms < ?";
                         params.add(to_ms);
                       }
                       if (from_ms > 0) {
                         where += " AND end_ms > ?";
                         params.add(from_ms);
                       }
                       uvcpp::uvcpp_db_table table;
                       std::string error;
                       if (!Query("SELECT id, channel_id, stream, start_ms, end_ms, duration_ms, "
                                  "size_bytes, "
                                  "record_type FROM recordings" +
                                      where + " ORDER BY start_ms ASC LIMIT 2000",
                                  params, &table, &error)) {
                         return;
                       }
                       for (size_t i = 0; i < table.rows().size(); ++i) {
                         items->push_back(json{
                             {"id", TableInt(table, i, "id")},
                             {"channelId", TableInt(table, i, "channel_id")},
                             {"stream", TableText(table, i, "stream")},
                             {"startMs", TableInt(table, i, "start_ms")},
                             {"endMs", TableInt(table, i, "end_ms")},
                             {"durationMs", TableInt(table, i, "duration_ms")},
                             {"sizeBytes", TableInt(table, i, "size_bytes")},
                             {"recordType", TableText(table, i, "record_type")},
                         });
                       }
                     },
                     [items, &resp, next] {
                       SendJson(resp, 200, *items);
                       next();
                     });
               },
               "playback.view"));

  web_.get(
      "/api/v1/recordings/:id/download",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string* id_param = req.param("id");
            int64_t recording_id = 0;
            if (!ParseIntParam(id_param, &recording_id) || recording_id <= 0) {
              SendError(resp, 400, "invalid_id", "invalid recording id");
              next();
              return;
            }
            const std::string range_header = req.header("Range");
            auto path = std::make_shared<std::string>();
            auto file_size = std::make_shared<uint64_t>(0);
            auto status = std::make_shared<int>(200);
            auto message = std::make_shared<std::string>();
            RunWork(
                [this, recording_id, path, file_size, status, message] {
                  uvcpp::uvcpp_db_table table;
                  uvcpp::uvcpp_db_params params;
                  params.add(recording_id);
                  std::string error;
                  if (!Query("SELECT path FROM recordings WHERE id = ?", params, &table, &error) ||
                      table.empty()) {
                    *status = 404;
                    *message = "recording not found";
                    return;
                  }
                  *path = TableText(table, 0, "path");
                  std::error_code size_error;
                  const uintmax_t size = std::filesystem::file_size(*path, size_error);
                  if (size_error || size == 0) {
                    *status = 404;
                    *message = "recording file is missing or empty";
                    return;
                  }
                  *file_size = static_cast<uint64_t>(size);
                },
                [path, file_size, status, message, range_header, recording_id, &resp, next] {
                  if (*status != 200) {
                    SendError(resp, *status, "download_failed", *message);
                    next();
                    return;
                  }
                  const uint64_t size = *file_size;
                  ByteRange range;
                  if (!range_header.empty())
                    range = ParseByteRange(range_header, size);

                  resp.set_header("Accept-Ranges", "bytes");
                  resp.set_header("Content-Type", "video/mp2t");
                  resp.set_header("Content-Disposition", "attachment; filename=\"uvnvr-" +
                                                             std::to_string(recording_id) +
                                                             ".ts\"");
                  resp.set_header("Cache-Control", "no-store");

                  if (range.present && (!range.syntactically_valid || !range.satisfiable)) {
                    resp.status(416);
                    resp.set_header("Content-Range", "bytes */" + std::to_string(size));
                    resp.set_header("Content-Length", "0");
                    resp.end();
                    next();
                    return;
                  }
                  if (range.present) {
                    resp.status(206);
                    resp.set_header("Content-Range", "bytes " + std::to_string(range.first) + "-" +
                                                         std::to_string(range.last) + "/" +
                                                         std::to_string(size));
                    resp.send_file_range(*path, range.first, range.last);
                  } else {
                    resp.status(200);
                    resp.send_file(*path, static_cast<int64_t>(size));
                  }
                  next();
                });
          },
          "playback.download"));

  web_.post(
      "/api/v1/playback/export",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
              SendError(resp, 400, "invalid_json", "request body must be a JSON object");
              next();
              return;
            }
            const int64_t channel_id = JsonInt(body, "channelId", 0);
            const std::string stream = JsonString(body, "stream", "main");
            const int64_t start_ms = JsonInt(body, "startMs", 0);
            const int64_t end_ms = JsonInt(body, "endMs", 0);
            if (channel_id <= 0 || (stream != "main" && stream != "sub") || start_ms <= 0 ||
                end_ms <= start_ms) {
              SendError(resp, 400, "invalid_range",
                        "channelId, stream, startMs and endMs are required");
              next();
              return;
            }
            if (end_ms - start_ms > 6LL * 60 * 60 * 1000) {
              SendError(resp, 400, "range_too_large", "the export range is limited to 6 hours");
              next();
              return;
            }
            if (!media_ || !media_->Available()) {
              SendError(resp, 503, "ffmpeg_unavailable", "FFmpeg is required for MP4 export");
              next();
              return;
            }
            const std::string ffmpeg = media_->FfmpegPath();
            const UserContext user = UserFromRequest(req);
            auto result = std::make_shared<ExportResult>();
            RunWork(
                [this, user, channel_id, stream, start_ms, end_ms, ffmpeg, result] {
                  uvcpp::uvcpp_db_table table;
                  uvcpp::uvcpp_db_params params;
                  params.add(channel_id);
                  params.add(stream);
                  params.add(start_ms);
                  params.add(end_ms);
                  std::string error;
                  if (!Query("SELECT path, start_ms, end_ms, size_bytes FROM recordings "
                             "WHERE channel_id = ? AND stream = ? AND end_ms > ? AND start_ms < ? "
                             "ORDER BY start_ms",
                             params, &table, &error)) {
                    result->message = error;
                    return;
                  }
                  if (table.empty()) {
                    result->status = 404;
                    result->code = "no_recordings";
                    result->message = "no recordings in the requested range";
                    return;
                  }
                  std::vector<std::string> paths;
                  paths.reserve(table.rows().size());
                  uint64_t total_bytes = 0;
                  int64_t first_start = 0;
                  for (size_t row = 0; row < table.rows().size(); ++row) {
                    paths.push_back(table.at(row, "path").to_text());
                    if (first_start == 0)
                      first_start = table.at(row, "start_ms").to_int64();
                    total_bytes += static_cast<uint64_t>(
                        std::max<int64_t>(0, table.at(row, "size_bytes").to_int64()));
                  }
                  if (total_bytes > 4ULL * 1024 * 1024 * 1024) {
                    result->status = 413;
                    result->code = "range_too_large";
                    result->message = "the export range exceeds 4 GiB";
                    return;
                  }
                  const std::string id = std::to_string(NowSeconds()) + "-" +
                                         std::to_string(channel_id) + "-" + RandomHex(4);
                  result->work_dir =
                      (std::filesystem::path(config_.data_dir) / "exports" / id).string();
                  result->path = (std::filesystem::path(result->work_dir) / "export.mp4").string();
                  result->filename = "uvnvr-" + std::to_string(channel_id) + "-" +
                                     std::to_string(start_ms) + ".mp4";
                  ExportJob job;
                  job.segments = std::move(paths);
                  job.first_segment_start_ms = first_start;
                  job.start_ms = start_ms;
                  job.end_ms = end_ms;
                  job.ffmpeg_path = ffmpeg;
                  job.work_dir = result->work_dir;
                  job.output_path = result->path;
                  if (!ExportMp4(job, &error)) {
                    result->message = error.empty() ? "MP4 export failed" : error;
                    return;
                  }
                  std::error_code size_error;
                  const uintmax_t size = std::filesystem::file_size(result->path, size_error);
                  if (size_error || size == 0) {
                    result->message = "the exported file is missing";
                    return;
                  }
                  result->size = static_cast<uint64_t>(size);
                  result->ok = true;
                  Audit("record", "info", user.username, "", "", "recording",
                        std::to_string(channel_id), "export", "success", "",
                        json{{"channelId", channel_id},
                             {"stream", stream},
                             {"startMs", start_ms},
                             {"endMs", end_ms},
                             {"bytes", result->size}}
                            .dump());
                },
                [result, &resp, next] {
                  if (!result->ok) {
                    if (!result->work_dir.empty()) {
                      std::error_code ec;
                      std::filesystem::remove_all(result->work_dir, ec);
                    }
                    SendError(resp, result->status, result->code, result->message);
                    next();
                    return;
                  }
                  resp.set_header("Content-Type", "video/mp4");
                  resp.set_header("Content-Disposition",
                                  "attachment; filename=\"" + result->filename + "\"");
                  resp.set_header("Cache-Control", "no-store");
                  resp.status(200);
                  const std::string work_dir = result->work_dir;
                  resp.send_file(result->path, static_cast<int64_t>(result->size),
                                 [work_dir](int, uint64_t) {
                                   std::thread([work_dir] {
                                     std::error_code ec;
                                     std::filesystem::remove_all(work_dir, ec);
                                   }).detach();
                                 });
                  next();
                });
          },
          "playback.download"));
}

App::~App() {
  StopMonitors();
  if (onvif_events_)
    onvif_events_->Stop();
}

void App::RaiseAlert(int64_t channel_id, const std::string& type, const std::string& level,
                     const std::string& message, const std::string& snapshot_path) {
  uvcpp::uvcpp_db_params params;
  params.add(channel_id);
  params.add(type);
  params.add(level);
  params.add(message);
  params.add(snapshot_path);
  params.add(0);
  params.add(NowSeconds());
  std::string error;
  if (!Exec("INSERT INTO alerts(channel_id, type, level, message, snapshot_path, acknowledged, "
            "created_at) VALUES(?, ?, ?, ?, ?, ?, ?)",
            params, nullptr, &error)) {
    std::fprintf(stderr, "alert insert failed: %s\n", error.c_str());
  }
  if (level != "info" && GetSettingInt("smtp.enabled", 0) != 0) {
    const std::string host = GetSetting("smtp.host", "smtp.qq.com");
    const int64_t port = GetSettingInt("smtp.port", 465);
    const std::string username = GetSetting("smtp.username", "");
    const std::string from = GetSetting("smtp.from", username);
    const std::string to = GetSetting("smtp.to", "");
    std::string decrypt_error;
    const std::string password = DecryptSecret(GetSetting("smtp.password_enc", ""), &decrypt_error);
    if (!to.empty() && !from.empty() && !password.empty()) {
      std::thread([host, port, username, password, from, to, type, message] {
        std::string smtp_error;
        if (!SendSmtpMail(host, static_cast<int>(port), username, password, from, to,
                          "UvNVR alert: " + type, message, true, &smtp_error)) {
          std::fprintf(stderr, "alert email failed: %s\n", smtp_error.c_str());
        }
      }).detach();
    }
  }
}

void App::StartMonitors() {
  if (monitor_thread_.joinable())
    return;
  monitor_stop_ = false;
  monitor_thread_ = std::thread([this] { MonitorRun(); });
}

void App::StopMonitors() {
  monitor_stop_ = true;
  if (monitor_thread_.joinable())
    monitor_thread_.join();
}

void App::MonitorRun() {
  const std::filesystem::path alert_dir = std::filesystem::path(config_.data_dir) / "alerts";
  std::error_code ec;
  std::filesystem::create_directories(alert_dir, ec);
  int64_t last_ntp_sync = 0;
  while (!monitor_stop_.load()) {
    int motion_channels = 0;
    std::string error;
    const auto channels = LoadChannels(&error);
    int64_t now = NowSeconds();
    for (const auto& channel : channels) {
      if (!channel.enabled)
        continue;
      const StreamStatus status = media_ ? media_->Status(channel.id, "main") : StreamStatus{};
      const std::string state = status.state;
      const auto previous = last_channel_state_.find(channel.id);
      if (state == "offline" || state == "error") {
        if (previous == last_channel_state_.end() || previous->second != state) {
          RaiseAlert(channel.id, "channel_offline", "warning",
                     "Channel " + channel.name + " is offline: " + status.error, "");
        }
      } else if (state == "online") {
        if (previous != last_channel_state_.end() &&
            (previous->second == "offline" || previous->second == "error")) {
          RaiseAlert(channel.id, "channel_online", "info",
                     "Channel " + channel.name + " is back online", "");
        }
      }
      if (previous == last_channel_state_.end() || previous->second != state) {
        uvcpp::uvcpp_db_params update;
        update.add(state);
        update.add(status.error);
        update.add(state == "online" ? now : 0);
        update.add(now);
        update.add((state == "offline" || state == "error") ? now : 0);
        update.add(now);
        update.add(channel.id);
        std::string state_error;
        Exec("UPDATE channels SET last_status=?, last_error=?, "
             "last_online_at=CASE WHEN ? > 0 THEN ? ELSE last_online_at END, "
             "last_offline_at=CASE WHEN ? > 0 THEN ? ELSE last_offline_at END WHERE id=?",
             update, nullptr, &state_error);
        uvcpp::uvcpp_db_params event;
        event.add(channel.id);
        event.add(state);
        event.add(status.error);
        event.add(now);
        Exec("INSERT INTO channel_events(channel_id, state, reason, at) VALUES(?, ?, ?, ?)", event,
             nullptr, &state_error);
      }
      last_channel_state_[channel.id] = state;

      if (state != "online" || motion_channels >= 4)
        continue;
      std::string url_error;
      const bool use_sub = channel.sub_enabled;
      const std::string url = BuildChannelUrl(channel, use_sub, &url_error);
      if (url.empty())
        continue;
      const int width = 64;
      const int height = 36;
      std::vector<uint8_t> frame;
      if (!media_->CaptureGray(url, width, height, &frame, &url_error))
        continue;
      motion_channels += 1;
      auto previous_frame = last_motion_frame_.find(channel.id);
      if (previous_frame != last_motion_frame_.end() &&
          previous_frame->second.size() == frame.size()) {
        int64_t diff = 0;
        for (size_t i = 0; i < frame.size(); ++i) {
          diff +=
              std::abs(static_cast<int>(frame[i]) - static_cast<int>(previous_frame->second[i]));
        }
        const double mean = static_cast<double>(diff) / frame.size();
        const int64_t last = motion_cooldown_[channel.id];
        if (mean > 6.0 && now - last > 60) {
          std::string jpeg;
          std::string jpeg_error;
          std::string snapshot_path;
          if (media_->CaptureJpeg(url, &jpeg, &jpeg_error) && !jpeg.empty()) {
            snapshot_path =
                (alert_dir / (std::to_string(channel.id) + "_" + std::to_string(now) + ".jpg"))
                    .string();
            std::ofstream output(snapshot_path, std::ios::binary);
            output.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
          }
          RaiseAlert(channel.id, "motion", "warning", "Motion detected on channel " + channel.name,
                     snapshot_path);
          motion_cooldown_[channel.id] = now;
        }
      }
      last_motion_frame_[channel.id] = std::move(frame);
      if (motion_channels >= 4)
        break;
    }

    if (now - last_storage_alert_ms_ > 3600) {
      uvcpp::uvcpp_db_table volumes;
      if (Query("SELECT id, path, mode, max_usage_pct FROM storage_volumes WHERE enabled = 1", {},
                &volumes, &error)) {
        for (size_t i = 0; i < volumes.rows().size(); ++i) {
          const std::string path = TableText(volumes, i, "path");
          const int64_t max_pct = TableInt(volumes, i, "max_usage_pct");
#ifdef _WIN32
          ULARGE_INTEGER free_bytes{};
          ULARGE_INTEGER total_bytes{};
          if (!GetDiskFreeSpaceExA(path.c_str(), &free_bytes, &total_bytes, nullptr))
            continue;
          const uint64_t total = total_bytes.QuadPart;
          const uint64_t free = free_bytes.QuadPart;
#else
          struct statvfs stat {};
          if (statvfs(path.c_str(), &stat) != 0)
            continue;
          const uint64_t total = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
          const uint64_t free = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
#endif
          if (total == 0)
            continue;
          const double used = 100.0 * (1.0 - static_cast<double>(free) / total);
          if (used >= static_cast<double>(max_pct)) {
            RaiseAlert(0, "storage_full", "warning",
                       "Storage volume " + path + " is " + std::to_string(static_cast<int>(used)) +
                           "% full",
                       "");
          }
        }
      }
      last_storage_alert_ms_ = now;
    }

    if (GetSettingInt("system.ntp_enabled", 0) != 0 && NowSeconds() - last_ntp_sync > 6 * 3600) {
      const std::string server = GetSetting("system.ntp_server", "pool.ntp.org");
      int64_t offset = 0;
      std::string ntp_error;
      if (QueryNtpOffset(server, &offset, &ntp_error)) {
        std::string db_error;
        SetSetting("system.ntp_last_offset_ms", std::to_string(offset), &db_error);
        SetSetting("system.ntp_last_sync", std::to_string(NowSeconds()), &db_error);
        if (std::llabs(offset) >= 500)
          ApplyTimeOffset(offset, &ntp_error);
      } else {
        std::fprintf(stderr, "NTP sync failed: %s\n", ntp_error.c_str());
      }
      last_ntp_sync = NowSeconds();
    }

    for (int i = 0; i < 30 && !monitor_stop_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void App::RegisterAlertRoutes() {
  web_.post("/api/v1/system/email/test",
            Protected(
                [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  auto op = std::make_shared<OpResult>();
                  RunWork(
                      [this, op] {
                        const std::string host = GetSetting("smtp.host", "smtp.qq.com");
                        const int64_t port = GetSettingInt("smtp.port", 465);
                        const std::string username = GetSetting("smtp.username", "");
                        const std::string from = GetSetting("smtp.from", username);
                        const std::string to = GetSetting("smtp.to", "");
                        std::string error;
                        const std::string password =
                            DecryptSecret(GetSetting("smtp.password_enc", ""), &error);
                        if (host.empty() || from.empty() || to.empty() || password.empty()) {
                          op->ok = false;
                          op->status = 400;
                          op->code = "smtp_not_configured";
                          op->message = "SMTP host, sender, recipient and password are required";
                          return;
                        }
                        if (!SendSmtpMail(host, static_cast<int>(port), username, password, from,
                                          to, "UvNVR test email",
                                          "This is a test message from UvNVR.", true, &error)) {
                          op->ok = false;
                          op->status = 502;
                          op->code = "smtp_failed";
                          op->message = error;
                          return;
                        }
                        op->data = json{{"ok", true}};
                      },
                      [op, &resp, next] {
                        SendOp(resp, *op);
                        next();
                      });
                },
                "system.manage"));

  web_.get(
      "/api/v1/alerts",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const std::string level = req.query("level") ? *req.query("level") : "";
            const std::string acknowledged =
                req.query("acknowledged") ? *req.query("acknowledged") : "";
            auto items = std::make_shared<json>(json::array());
            RunWork(
                [this, level, acknowledged, items] {
                  std::string where = " WHERE 1=1";
                  uvcpp::uvcpp_db_params params;
                  if (!level.empty()) {
                    where += " AND a.level = ?";
                    params.add(level);
                  }
                  if (acknowledged == "1" || acknowledged == "0") {
                    where += " AND a.acknowledged = ?";
                    params.add(acknowledged == "1" ? 1 : 0);
                  }
                  uvcpp::uvcpp_db_table table;
                  std::string error;
                  if (!Query(
                          "SELECT a.id, a.channel_id, c.name AS channel_name, a.type, a.level, "
                          "a.message, a.snapshot_path, a.acknowledged, a.created_at FROM alerts a "
                          "LEFT JOIN channels c ON c.id = a.channel_id" +
                              where + " ORDER BY a.created_at DESC LIMIT 300",
                          params, &table, &error)) {
                    return;
                  }
                  for (size_t i = 0; i < table.rows().size(); ++i) {
                    items->push_back(json{
                        {"id", TableInt(table, i, "id")},
                        {"channelId", TableInt(table, i, "channel_id")},
                        {"channelName", TableText(table, i, "channel_name")},
                        {"type", TableText(table, i, "type")},
                        {"level", TableText(table, i, "level")},
                        {"message", TableText(table, i, "message")},
                        {"snapshotPath", TableText(table, i, "snapshot_path")},
                        {"acknowledged", TableInt(table, i, "acknowledged") != 0},
                        {"createdAt", TableInt(table, i, "created_at")},
                    });
                  }
                },
                [items, &resp, next] {
                  SendJson(resp, 200, *items);
                  next();
                });
          },
          "alert.view"));

  web_.post("/api/v1/alerts/:id/ack",
            Protected(
                [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
                  const std::string* id_param = req.param("id");
                  int64_t alert_id = 0;
                  if (!ParseIntParam(id_param, &alert_id) || alert_id <= 0) {
                    SendError(resp, 400, "invalid_id", "invalid alert id");
                    next();
                    return;
                  }
                  const UserContext actor = UserFromRequest(req);
                  auto op = std::make_shared<OpResult>();
                  RunWork(
                      [this, alert_id, actor, op] {
                        uvcpp::uvcpp_db_params params;
                        params.add(NowSeconds());
                        params.add(alert_id);
                        std::string error;
                        if (!Exec("UPDATE alerts SET acknowledged = 1 WHERE id = ?", params,
                                  nullptr, &error)) {
                          op->ok = false;
                          op->status = 500;
                          op->code = "database_error";
                          op->message = "the alert could not be acknowledged";
                          return;
                        }
                        Audit("alert", "info", actor.username, "", "", "alert",
                              std::to_string(alert_id), "acknowledge", "success", "", "{}");
                        op->data = json{{"ok", true}};
                      },
                      [op, &resp, next] {
                        SendOp(resp, *op);
                        next();
                      });
                },
                "alert.manage"));

  web_.get("/api/v1/alerts/:id/snapshot",
           Protected(
               [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 const std::string* id_param = req.param("id");
                 int64_t alert_id = 0;
                 if (!ParseIntParam(id_param, &alert_id) || alert_id <= 0) {
                   SendError(resp, 400, "invalid_id", "invalid alert id");
                   next();
                   return;
                 }
                 auto payload = std::make_shared<std::string>();
                 auto status = std::make_shared<int>(200);
                 auto message = std::make_shared<std::string>();
                 RunWork(
                     [this, alert_id, payload, status, message] {
                       uvcpp::uvcpp_db_table table;
                       uvcpp::uvcpp_db_params params;
                       params.add(alert_id);
                       std::string error;
                       if (!Query("SELECT snapshot_path FROM alerts WHERE id = ?", params, &table,
                                  &error) ||
                           table.empty()) {
                         *status = 404;
                         *message = "alert not found";
                         return;
                       }
                       const std::string path = TableText(table, 0, "snapshot_path");
                       if (path.empty()) {
                         *status = 404;
                         *message = "alert has no snapshot";
                         return;
                       }
                       std::ifstream file(path, std::ios::binary);
                       if (!file) {
                         *status = 404;
                         *message = "snapshot file is missing";
                         return;
                       }
                       payload->assign(std::istreambuf_iterator<char>(file),
                                       std::istreambuf_iterator<char>());
                     },
                     [payload, status, message, &resp, next] {
                       if (*status != 200) {
                         SendError(resp, *status, "not_found", *message);
                       } else {
                         resp.status(200).body(*payload, "image/jpeg");
                         resp.end();
                       }
                       next();
                     });
               },
               "alert.view"));
}

void App::RegisterSystemRoutes() {
  web_.get("/api/v1/system/config",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 SendJson(resp, 200, ConfigToJson(config_));
                 next();
               },
               "system.view"));

  web_.get("/api/v1/system/time",
           Protected(
               [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next next) {
                 SendJson(resp, 200,
                          json{{"serverMs", NowSeconds() * 1000},
                               {"timezone", GetSetting("system.timezone", "UTC")}});
                 next();
               },
               "system.view"));

  web_.post(
      "/api/v1/system/time/sync",
      Protected(
          [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                 uvcpp::uvcpp_web_next next) {
            const json body = json::parse(req.body_str(), nullptr, false);
            const int64_t epoch_ms = body.is_object() ? JsonInt(body, "epochMs", 0) : 0;
            const int64_t now_ms = NowSeconds() * 1000;
            if (epoch_ms <= 0 || std::llabs(epoch_ms - now_ms) > 366LL * 24 * 3600 * 1000) {
              SendError(resp, 400, "invalid_time", "epochMs must be a plausible current time");
              next();
              return;
            }
            const UserContext actor = UserFromRequest(req);
            auto op = std::make_shared<OpResult>();
            RunWork(
                [this, epoch_ms, actor, op] {
                  std::string error;
#ifdef _WIN32
                  FILETIME file_time;
                  ULARGE_INTEGER value;
                  value.QuadPart =
                      static_cast<ULONGLONG>(epoch_ms) * 10000ULL + 116444736000000000ULL;
                  file_time.dwLowDateTime = value.LowPart;
                  file_time.dwHighDateTime = value.HighPart;
                  SYSTEMTIME system_time;
                  if (!FileTimeToSystemTime(&file_time, &system_time) ||
                      !SetSystemTime(&system_time)) {
                    error = "SetSystemTime failed; run the service with sufficient privileges";
                  }
#else
                  struct timeval tv {};
                  tv.tv_sec = static_cast<time_t>(epoch_ms / 1000);
                  tv.tv_usec = static_cast<suseconds_t>((epoch_ms % 1000) * 1000);
                  if (settimeofday(&tv, nullptr) != 0) {
                    error = std::string("settimeofday failed: ") + std::strerror(errno) +
                            " (CAP_SYS_TIME is required)";
                  }
#endif
                  if (!error.empty()) {
                    op->ok = false;
                    op->status = 403;
                    op->code = "time_sync_failed";
                    op->message = error;
                    return;
                  }
                  Audit("system", "warning", actor.username, "", "", "system", "", "time_sync",
                        "success", "", json{{"epochMs", epoch_ms}}.dump());
                  op->data = json{{"ok", true}, {"serverMs", epoch_ms}};
                },
                [op, &resp, next] {
                  SendOp(resp, *op);
                  next();
                });
          },
          "system.manage"));
}

} // namespace uvnvr
