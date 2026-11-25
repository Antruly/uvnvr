#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace uvnvr {

// Hard caps applied before any AI snapshot file is written. Zero disables the
// corresponding check.
struct SnapshotQuotaLimits {
  int64_t max_per_day = 0;  // images per local calendar day
  int64_t max_total_mb = 0; // total size of the AI snapshot directory
  int64_t min_free_mb = 0;  // stop writing when the disk has less free space
};

enum class SnapshotBlockReason {
  kNone = 0,
  kDailyLimit,
  kTotalLimit,
  kLowDisk,
};

const char* SnapshotBlockReasonName(SnapshotBlockReason reason);

// Thread-safe gatekeeper for AI snapshot writes.
//
// The daily counter lives in memory and is seeded from system_meta at startup
// (see AiService::StartLocked). The directory-size and free-space probes are
// injectable so the decision logic can be exercised without a real filesystem.
class SnapshotQuota {
public:
  using SizeProbe = std::function<int64_t(const std::string& directory)>;
  using FreeProbe = std::function<int64_t(const std::string& directory)>;
  using PersistFn = std::function<void(int64_t day, int64_t count)>;

  // Defaults to the real filesystem probes.
  explicit SnapshotQuota(SizeProbe size_probe = {}, FreeProbe free_probe = {});
  ~SnapshotQuota();
  SnapshotQuota(const SnapshotQuota&) = delete;
  SnapshotQuota& operator=(const SnapshotQuota&) = delete;

  // Sets the snapshot root and the active limits. Clears the cached directory
  // size so the next check re-probes.
  void Configure(std::string directory, SnapshotQuotaLimits limits, int64_t now_ms);
  void SetPersist(PersistFn persist);
  // Seeds today's counter after a restart. `day` uses the yyyyMMdd local stamp;
  // a stale day is ignored.
  void Seed(int64_t day, int64_t count, int64_t now_ms);

  // Reserves one image slot. Returns false when a limit is reached and sets
  // `reason` when non-null. Every successful Acquire must be followed by
  // exactly one Commit or Abort.
  bool Acquire(int64_t now_ms, SnapshotBlockReason* reason = nullptr);
  void Commit(int64_t bytes, int64_t now_ms);
  void Abort();

  // Recomputes the cached directory size when its 60 s TTL has expired. Used by
  // the stats endpoint; cheap enough to call from a worker thread.
  void Refresh(int64_t now_ms);
  // Persists the current day/count immediately (called on shutdown).
  void Flush();

  bool Blocked() const;
  SnapshotBlockReason LastBlockReason() const;
  int64_t Today() const;
  int64_t DirectoryBytes() const;
  int64_t FreeBytes() const;
  uint64_t Skipped() const;

private:
  int64_t LocalDay(int64_t now_ms) const;
  void RollDayLocked(int64_t now_ms);
  void RefreshSizeLocked(int64_t now_ms);
  int64_t ProbeFreeLocked();
  void PersistLocked(int64_t now_ms, bool force);

  SizeProbe size_probe_;
  FreeProbe free_probe_;
  PersistFn persist_;

  mutable std::mutex mutex_;
  std::string directory_;
  SnapshotQuotaLimits limits_;
  int64_t day_ = 0;
  int64_t today_ = 0;
  int64_t reserved_ = 0;
  int64_t dir_bytes_ = 0;
  bool dir_bytes_valid_ = false;
  int64_t dir_bytes_probed_ms_ = 0;
  int64_t free_bytes_ = -1;
  int64_t free_bytes_probed_ms_ = 0;
  bool last_blocked_ = false;
  SnapshotBlockReason last_reason_ = SnapshotBlockReason::kNone;
  uint64_t skipped_ = 0;
  int64_t last_persist_ms_ = 0;
  bool persisted_ = false;
  bool seeded_ = false;
};

} // namespace uvnvr
