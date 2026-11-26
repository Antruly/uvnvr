// Unit tests for the credential-revocation policy used by
// PUT /api/v1/users/:id: password change, role change and disable must revoke
// browser sessions and API tokens, while a no-op or re-enable must not.
//
// The second half runs the real DELETE statements from the module against an
// in-memory SQLite database, so the test also covers the exact SQL and the
// affected-row accounting that lands in the audit detail.

#include "auth/session_revocation.h"

#include <cstdint>
#include <cstdio>
#include <db/uvcpp_db.h>
#include <string>
#include <vector>

namespace {

int failures = 0;

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectInt(const std::string& name, int64_t actual, int64_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %lld, expected %lld\n", name.c_str(),
               static_cast<long long>(actual), static_cast<long long>(expected));
  ++failures;
}

uvnvr::auth::UserSecurityChange Change(bool password, bool role, bool before, bool after) {
  uvnvr::auth::UserSecurityChange change;
  change.password_changed = password;
  change.role_changed = role;
  change.enabled_before = before;
  change.enabled_after = after;
  return change;
}

void TestPolicy() {
  using uvnvr::auth::PlanRevocation;

  const auto none = PlanRevocation(Change(false, false, true, true));
  ExpectBool("no change: sessions", none.sessions, false);
  ExpectBool("no change: api tokens", none.api_tokens, false);
  ExpectBool("no change: any", none.any(), false);

  const auto password = PlanRevocation(Change(true, false, true, true));
  ExpectBool("password change: sessions", password.sessions, true);
  ExpectBool("password change: api tokens", password.api_tokens, true);

  const auto role = PlanRevocation(Change(false, true, true, true));
  ExpectBool("role change: sessions", role.sessions, true);
  ExpectBool("role change: api tokens", role.api_tokens, true);

  const auto disabled = PlanRevocation(Change(false, false, true, false));
  ExpectBool("disable: sessions", disabled.sessions, true);
  ExpectBool("disable: api tokens", disabled.api_tokens, true);

  // Re-enabling an account is not a reason to log it out, and an update that
  // leaves the account disabled changes nothing that a live credential holds.
  const auto reenabled = PlanRevocation(Change(false, false, false, true));
  ExpectBool("re-enable: sessions", reenabled.sessions, false);
  ExpectBool("re-enable: api tokens", reenabled.api_tokens, false);
  const auto still_disabled = PlanRevocation(Change(false, false, false, false));
  ExpectBool("still disabled: any", still_disabled.any(), false);

  const auto combined = PlanRevocation(Change(true, true, false, true));
  ExpectBool("password+role, disabled before: sessions", combined.sessions, true);
  ExpectBool("password+role, disabled before: api tokens", combined.api_tokens, true);
}

bool ExecuteSql(uvcpp::uvcpp_db_client& db, const std::string& sql, int64_t user_id,
                int64_t* affected, std::string* error) {
  uvcpp::uvcpp_db_params params;
  params.add(user_id);
  const auto status = db.execute(sql, params, affected);
  if (uvcpp::uvcpp_db_ok(status))
    return true;
  if (error != nullptr)
    *error = db.last_error();
  return false;
}

int64_t CountRows(uvcpp::uvcpp_db_client& db, const std::string& sql, int64_t user_id) {
  uvcpp::uvcpp_db_params params;
  params.add(user_id);
  uvcpp::uvcpp_db_table table;
  if (!uvcpp::uvcpp_db_ok(db.query(sql, params, &table)) || table.empty())
    return -1;
  return table.rows()[0]["n"].to_int64();
}

void TestRevocationSql() {
  uvcpp::uvcpp_db_client db;
  auto status = db.open("sqlite://:memory:");
  if (!uvcpp::uvcpp_db_ok(status)) {
    std::fprintf(stderr, "FAIL sqlite open: %s\n", db.last_error().c_str());
    ++failures;
    return;
  }

  for (const char* sql : {
           "CREATE TABLE sessions(token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL)",
           "CREATE TABLE api_tokens(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL)",
           "INSERT INTO sessions(token_hash, user_id) VALUES('s1', 10)",
           "INSERT INTO sessions(token_hash, user_id) VALUES('s2', 10)",
           "INSERT INTO sessions(token_hash, user_id) VALUES('s3', 11)",
           "INSERT INTO api_tokens(id, user_id) VALUES(1, 10)",
           "INSERT INTO api_tokens(id, user_id) VALUES(2, 10)",
           "INSERT INTO api_tokens(id, user_id) VALUES(3, 11)",
       }) {
    if (!uvcpp::uvcpp_db_ok(db.execute(sql))) {
      std::fprintf(stderr, "FAIL setup '%s': %s\n", sql, db.last_error().c_str());
      ++failures;
      return;
    }
  }

  const auto exec = [&db](const std::string& sql, int64_t user_id, int64_t* affected,
                          std::string* error) {
    return ExecuteSql(db, sql, user_id, affected, error);
  };

  const uvnvr::auth::UserSecurityChange change = Change(true, false, true, true);
  uvnvr::auth::RevocationCounts counts;
  std::string error;
  ExpectBool("revoke password change: ok",
             uvnvr::auth::RevokeUserCredentials(exec, 10, uvnvr::auth::PlanRevocation(change),
                                                &counts, &error),
             true);
  ExpectInt("revoked sessions", counts.sessions, 2);
  ExpectInt("revoked api tokens", counts.api_tokens, 2);
  ExpectInt("sessions left for target", CountRows(db, "SELECT COUNT(*) AS n FROM sessions "
                                                      "WHERE user_id = ?",
                                                  10),
            0);
  ExpectInt("tokens left for target",
            CountRows(db, "SELECT COUNT(*) AS n FROM api_tokens WHERE user_id = ?", 10), 0);
  ExpectInt("other user's sessions kept",
            CountRows(db, "SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?", 11), 1);
  ExpectInt("other user's tokens kept",
            CountRows(db, "SELECT COUNT(*) AS n FROM api_tokens WHERE user_id = ?", 11), 1);

  // A second revocation with a no-op plan touches nothing.
  uvnvr::auth::RevocationCounts again;
  ExpectBool("revoke no-op: ok",
             uvnvr::auth::RevokeUserCredentials(
                 exec, 11, uvnvr::auth::PlanRevocation(Change(false, false, true, true)), &again,
                 &error),
             true);
  ExpectInt("no-op sessions", again.sessions, 0);
  ExpectInt("no-op api tokens", again.api_tokens, 0);
  ExpectInt("no-op keeps other user's sessions",
            CountRows(db, "SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?", 11), 1);
}

void TestFailurePropagates() {
  std::string error;
  const uvnvr::auth::ExecUserSql broken =
      [](const std::string&, int64_t, int64_t*, std::string* out_error) {
        if (out_error != nullptr)
          *out_error = "database unavailable";
        return false;
      };
  uvnvr::auth::RevocationCounts counts;
  uvnvr::auth::RevocationCounts untouched;
  untouched.sessions = 99;
  ExpectBool("failure reported",
             uvnvr::auth::RevokeUserCredentials(
                 broken, 10, uvnvr::auth::PlanRevocation(Change(true, false, true, true)),
                 &untouched, &error),
             false);
  ExpectBool("failure message", error == "database unavailable", true);
  ExpectInt("counts reset on failure", untouched.sessions, 0);
  (void)counts;
}

} // namespace

int main() {
  TestPolicy();
  TestRevocationSql();
  TestFailurePropagates();
  if (failures == 0) {
    std::printf("session_revocation_test passed\n");
    return 0;
  }
  std::fprintf(stderr, "session_revocation_test failed: %d failure(s)\n", failures);
  return 1;
}
