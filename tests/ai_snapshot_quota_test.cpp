#include "ai/snapshot_quota.h"

#include <cstdint>
#include <cstdio>
#include <string>

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

constexpr int64_t kMb = 1024 * 1024;
// 2026-11-25 12:00:00 UTC-ish. The day stamp is local time, so tests only rely
// on two fixed instants being on the same local day.
constexpr int64_t kNow = 1795603200000;
constexpr int64_t kLaterSameDay = kNow + 60 * 60 * 1000;

uvnvr::SnapshotQuotaLimits Limits(int64_t per_day, int64_t total_mb, int64_t free_mb) {
  uvnvr::SnapshotQuotaLimits limits;
  limits.max_per_day = per_day;
  limits.max_total_mb = total_mb;
  limits.min_free_mb = free_mb;
  return limits;
}

// Mirrors AiService::HandleFace: the event row is inserted first and snapshot
// writing is only skipped, so a quota never loses the event itself.
void TestDailyLimitKeepsEvents() {
  uvnvr::SnapshotQuota quota([](const std::string&) { return 0; },
                             [](const std::string&) { return 4096 * kMb; });
  quota.Configure("snapshots", Limits(5, 0, 0), kNow);

  int64_t events_inserted = 0;
  int64_t images_written = 0;
  for (int i = 0; i < 20; ++i) {
    ++events_inserted;
    if (quota.Acquire(kNow + i * 1000)) {
      ++images_written;
      quota.Commit(100 * 1024, kNow + i * 1000);
    }
  }
  ExpectInt("daily: images written", images_written, 5);
  ExpectInt("daily: events kept", events_inserted, 20);
  ExpectInt("daily: today counter", quota.Today(), 5);
  ExpectInt("daily: skipped counter", static_cast<int64_t>(quota.Skipped()), 15);
  ExpectBool("daily: blocked flag", quota.Blocked(), true);
  ExpectBool("daily: reason is per-day",
             quota.LastBlockReason() == uvnvr::SnapshotBlockReason::kDailyLimit, true);
}

void TestTotalSizeLimit() {
  int64_t measured = 0;
  uvnvr::SnapshotQuota quota([&](const std::string&) { return measured; },
                             [](const std::string&) { return 4096 * kMb; });
  quota.Configure("snapshots", Limits(0, 1, 0), kNow);

  int images = 0;
  for (int i = 0; i < 10; ++i) {
    if (quota.Acquire(kNow + i * 1000)) {
      ++images;
      const int64_t chunk = 300 * 1024;
      quota.Commit(chunk, kNow + i * 1000);
      measured += chunk;
    }
  }
  // 300 KiB per image against a 1 MiB directory cap: the fourth write takes
  // the directory past the cap and the fifth is refused.
  ExpectInt("total: images written", images, 4);
  ExpectInt("total: tracked bytes", quota.DirectoryBytes(), 4 * 300 * 1024);
  ExpectInt("total: skipped", static_cast<int64_t>(quota.Skipped()), 6);
  ExpectBool("total: reason", quota.LastBlockReason() == uvnvr::SnapshotBlockReason::kTotalLimit,
             true);
}

void TestFreeSpaceLimit() {
  int64_t free_bytes = 50 * kMb;
  uvnvr::SnapshotQuota quota([](const std::string&) { return 0; },
                             [&](const std::string&) { return free_bytes; });
  quota.Configure("snapshots", Limits(0, 0, 100), kNow);

  ExpectBool("free: blocked below threshold", quota.Acquire(kNow), false);
  ExpectBool("free: reason", quota.LastBlockReason() == uvnvr::SnapshotBlockReason::kLowDisk, true);

  free_bytes = 200 * kMb;
  ExpectBool("free: allowed after space is freed", quota.Acquire(kNow), true);
  quota.Abort();
  ExpectInt("free: aborted reservation is not counted", quota.Today(), 0);
}

void TestUnlimited() {
  uvnvr::SnapshotQuota quota([](const std::string&) { return 0; },
                             [](const std::string&) { return 0; });
  quota.Configure("snapshots", Limits(0, 0, 0), kNow);
  int allowed = 0;
  for (int i = 0; i < 100; ++i) {
    if (quota.Acquire(kNow + i))
      ++allowed;
  }
  ExpectInt("unlimited: all allowed", allowed, 100);
  ExpectInt("unlimited: skipped", static_cast<int64_t>(quota.Skipped()), 0);
}

void TestSeedAndDayRollover() {
  int64_t persisted_day = 0;
  int64_t persisted_count = 0;
  uvnvr::SnapshotQuota quota([](const std::string&) { return 0; },
                             [](const std::string&) { return 4096 * kMb; });
  quota.Configure("snapshots", Limits(5, 0, 0), kNow);
  quota.SetPersist([&](int64_t day, int64_t count) {
    persisted_day = day;
    persisted_count = count;
  });

  // Resume a restart that already wrote four images today.
  const int64_t today = 20261125;
  quota.Seed(today, 4, kNow);
  ExpectInt("seed: today counter", quota.Today(), 4);
  ExpectBool("seed: one slot left", quota.Acquire(kNow), true);
  quota.Commit(1024, kNow);
  ExpectBool("seed: limit reached", quota.Acquire(kNow), false);

  quota.Flush();
  ExpectInt("seed: persisted day", persisted_day, today);
  ExpectInt("seed: persisted count", persisted_count, 5);

  // A stale day stamp is ignored rather than restoring yesterday's count.
  uvnvr::SnapshotQuota stale([](const std::string&) { return 0; },
                             [](const std::string&) { return 4096 * kMb; });
  stale.Configure("snapshots", Limits(5, 0, 0), kNow);
  stale.Seed(20261124, 5, kNow);
  ExpectInt("seed: stale day ignored", stale.Today(), 0);
}

void TestFailedWriteDoesNotConsumeQuota() {
  uvnvr::SnapshotQuota quota([](const std::string&) { return 0; },
                             [](const std::string&) { return 4096 * kMb; });
  quota.Configure("snapshots", Limits(5, 0, 0), kNow);
  for (int i = 0; i < 10; ++i) {
    if (!quota.Acquire(kNow))
      break;
    quota.Abort(); // e.g. JPEG encode or ofstream failure
  }
  ExpectInt("abort: counter returns to zero", quota.Today(), 0);
  ExpectInt("abort: nothing skipped", static_cast<int64_t>(quota.Skipped()), 0);
}

} // namespace

int main() {
  TestDailyLimitKeepsEvents();
  TestTotalSizeLimit();
  TestFreeSpaceLimit();
  TestUnlimited();
  TestSeedAndDayRollover();
  TestFailedWriteDoesNotConsumeQuota();
  if (failures == 0) {
    std::printf("ai_snapshot_quota_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "ai_snapshot_quota_test: %d check(s) failed\n", failures);
  return 1;
}
