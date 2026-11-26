#include "auth/session_revocation.h"

namespace uvnvr::auth {

RevocationPlan PlanRevocation(const UserSecurityChange& change) {
  const bool disabled = change.enabled_before && !change.enabled_after;
  const bool invalidated = change.password_changed || change.role_changed || disabled;

  RevocationPlan plan;
  plan.sessions = invalidated;
  // API tokens are long-lived credentials for the same account. Revoking them
  // too is what makes "change the password" mean "the old key stops working";
  // it also prevents a role upgrade from silently widening the permissions of
  // a token that was minted while the account had a weaker role.
  plan.api_tokens = invalidated;
  return plan;
}

bool RevokeUserCredentials(const ExecUserSql& exec, int64_t user_id,
                           const RevocationPlan& plan, RevocationCounts* counts,
                           std::string* error) {
  if (counts != nullptr)
    *counts = RevocationCounts{};
  if (!plan.any())
    return true;
  if (!exec)
    return false;

  if (plan.sessions) {
    int64_t affected = 0;
    if (!exec(kRevokeSessionsSql, user_id, &affected, error))
      return false;
    if (counts != nullptr)
      counts->sessions = affected;
  }
  if (plan.api_tokens) {
    int64_t affected = 0;
    if (!exec(kRevokeApiTokensSql, user_id, &affected, error))
      return false;
    if (counts != nullptr)
      counts->api_tokens = affected;
  }
  return true;
}

} // namespace uvnvr::auth
