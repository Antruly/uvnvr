// Unit tests for the object-level channel ACL decision helpers. The database
// plumbing lives in app.cpp; these tests pin the deny-by-default semantics and
// the group-hierarchy expansion that every enforcement point relies on.

#include "secure/channel_acl.h"

#include <cstdio>
#include <vector>

namespace acl = uvnvr::acl;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

void TestAdministrator() {
  Expect(acl::IsAdministrator("admin", {}), "the built-in admin role is unrestricted");
  Expect(acl::IsAdministrator("auditor", {"*"}), "a wildcard permission is unrestricted");
  Expect(!acl::IsAdministrator("operator", {"channel.view", "preview.view"}),
         "ordinary roles are restricted");
  Expect(!acl::IsAdministrator("operator", {}), "an empty role is restricted");
}

void TestGrantDecisions() {
  acl::Grants empty;
  Expect(!empty.Allows(1, 0), "empty grants deny a direct channel");
  Expect(empty.Empty(), "empty grants report empty");

  acl::Grants direct;
  direct.channels.insert(7);
  Expect(direct.Allows(7, 0), "a direct channel grant allows that channel");
  Expect(!direct.Allows(8, 0), "a direct channel grant does not allow other channels");
  Expect(!direct.Allows(8, 3), "a direct grant does not extend to another channel in a group");
  Expect(!direct.Empty(), "a direct grant is not empty");

  acl::Grants grouped;
  grouped.groups.insert(4);
  Expect(grouped.Allows(9, 4), "a group grant allows a channel in that group");
  Expect(!grouped.Allows(9, 5), "a group grant does not allow another group");
  Expect(!grouped.Allows(9, 0), "a group grant does not allow an ungrouped channel");

  acl::Grants all;
  all.unrestricted = true;
  Expect(all.Allows(123, 0), "an unrestricted grant allows any channel");
  Expect(all.Allows(0, 0), "an unrestricted grant does not depend on ids");
}

void TestGroupExpansion() {
  // 1 -> 2 -> 4 and 1 -> 3; 5 is a separate tree.
  const std::vector<std::pair<int64_t, int64_t>> edges = {
      {2, 1},
      {3, 1},
      {4, 2},
      {5, 0},
  };

  const auto expanded = acl::ExpandGroupGrants({1}, edges);
  Expect(expanded == std::set<int64_t>({1, 2, 3, 4}), "a parent grant expands to all descendants");

  const auto leaf = acl::ExpandGroupGrants({2}, edges);
  Expect(leaf == std::set<int64_t>({2, 4}), "a leaf grant expands only downwards");

  const auto unrelated = acl::ExpandGroupGrants({5}, edges);
  Expect(unrelated == std::set<int64_t>({5}), "an unrelated grant stays alone");

  const auto combined = acl::ExpandGroupGrants({3, 5}, edges);
  Expect(combined == std::set<int64_t>({3, 5}), "multiple roots stay independent");
}

void TestCycleGuard() {
  // 1 -> 2 -> 3 -> 1: the walk must terminate and return the reachable set.
  const std::vector<std::pair<int64_t, int64_t>> edges = {
      {2, 1},
      {3, 2},
      {1, 3},
  };
  const auto expanded = acl::ExpandGroupGrants({1}, edges);
  Expect(expanded == std::set<int64_t>({1, 2, 3}), "cycles are visited once");
}

} // namespace

int main() {
  TestAdministrator();
  TestGrantDecisions();
  TestGroupExpansion();
  TestCycleGuard();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d channel ACL assertion(s) failed\n", g_failures);
    return 1;
  }
  std::printf("channel ACL unit tests passed\n");
  return 0;
}
