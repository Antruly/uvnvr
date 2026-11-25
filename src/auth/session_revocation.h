#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace uvnvr::auth {

// Security-relevant fields of a user update (PUT /api/v1/users/:id). The
// handler fills this from the row before the update and the requested values,
// so the revocation decision does not depend on the SQL branch that ran.
struct UserSecurityChange {
  bool password_changed = false;
  bool role_changed = false;
  bool enabled_before = true;
  bool enabled_after = true;
};

// Which credential material must be destroyed. Both kinds are tied to the
// account, not to a single request, so a change to the account's secrets or
// authority invalidates them together.
struct RevocationPlan {
  bool sessions = false;
  bool api_tokens = false;

  bool any() const {
    return sessions || api_tokens;
  }
};

struct RevocationCounts {
  int64_t sessions = 0;
  int64_t api_tokens = 0;
};

// A password change, a role change, or a transition from enabled to disabled
// revokes everything. Re-enabling an account, or an update that only touches
// other fields, leaves existing credentials alone.
RevocationPlan PlanRevocation(const UserSecurityChange& change);

// Statements are user-scoped; `user_id` is bound as the single parameter.
inline constexpr char kRevokeSessionsSql[] = "DELETE FROM sessions WHERE user_id = ?";
inline constexpr char kRevokeApiTokensSql[] = "DELETE FROM api_tokens WHERE user_id = ?";

// Executor shaped like App::Exec: run one parameterised statement, return false
// and fill `error` on failure. Keeping the SQL here (instead of in the handler)
// lets the unit test run the same statements against an in-memory database.
using ExecUserSql = std::function<bool(const std::string& sql, int64_t user_id,
                                       int64_t* affected, std::string* error)>;

// Applies `plan` for `user_id`. On success `counts` holds the number of deleted
// rows. If a statement fails the function stops and returns false; earlier
// deletions (if any) stay applied, which is the safe direction: a client may be
// logged out twice, but never keeps a credential that should be gone.
bool RevokeUserCredentials(const ExecUserSql& exec, int64_t user_id,
                           const RevocationPlan& plan, RevocationCounts* counts,
                           std::string* error);

} // namespace uvnvr::auth
