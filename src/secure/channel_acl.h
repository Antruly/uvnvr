#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace uvnvr::acl {

// Object-level channel grants for a single account.
//
// `groups` contains the group ids the caller may use. Callers expand a
// configured grant through ExpandGroupGrants() first, so a grant on a parent
// group also covers its descendants. Channel 0 / group 0 never match.
struct Grants {
  bool unrestricted = false;
  std::set<int64_t> channels;
  std::set<int64_t> groups;

  bool Allows(int64_t channel_id, int64_t group_id) const {
    if (unrestricted)
      return true;
    if (channel_id > 0 && channels.find(channel_id) != channels.end())
      return true;
    return group_id > 0 && groups.find(group_id) != groups.end();
  }

  bool Empty() const {
    return !unrestricted && channels.empty() && groups.empty();
  }
};

// Accounts with the wildcard permission (the built-in "admin" role) are
// exempt from object ACLs; role names are checked as a fallback so a renamed
// administrator role keeps working.
bool IsAdministrator(const std::string& role_name, const std::vector<std::string>& permissions);

// Expands a set of granted group ids with every descendant in the hierarchy.
// `parent_edges` is a list of (group id, parent id) pairs. Cycles are tolerated:
// each group is visited at most once.
std::set<int64_t> ExpandGroupGrants(const std::set<int64_t>& granted,
                                    const std::vector<std::pair<int64_t, int64_t>>& parent_edges);

} // namespace uvnvr::acl
