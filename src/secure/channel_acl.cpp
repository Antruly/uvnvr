#include "secure/channel_acl.h"

#include <unordered_map>

namespace uvnvr::acl {

bool IsAdministrator(const std::string& role_name, const std::vector<std::string>& permissions) {
  if (role_name == "admin")
    return true;
  for (const std::string& permission : permissions) {
    if (permission == "*")
      return true;
  }
  return false;
}

std::set<int64_t> ExpandGroupGrants(const std::set<int64_t>& granted,
                                    const std::vector<std::pair<int64_t, int64_t>>& parent_edges) {
  std::unordered_map<int64_t, std::vector<int64_t>> children;
  for (const auto& [group_id, parent_id] : parent_edges) {
    if (group_id > 0 && parent_id != group_id)
      children[parent_id].push_back(group_id);
  }

  std::set<int64_t> result;
  std::vector<int64_t> pending;
  for (const int64_t group_id : granted) {
    if (group_id > 0 && result.insert(group_id).second)
      pending.push_back(group_id);
  }

  // Iterative walk instead of recursion: group hierarchies come from user
  // input and a deep or cyclic tree must not overflow the stack.
  while (!pending.empty()) {
    const int64_t parent_id = pending.back();
    pending.pop_back();
    const auto found = children.find(parent_id);
    if (found == children.end())
      continue;
    for (const int64_t child_id : found->second) {
      if (result.insert(child_id).second)
        pending.push_back(child_id);
    }
  }
  return result;
}

} // namespace uvnvr::acl
