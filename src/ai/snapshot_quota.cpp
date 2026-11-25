#include "snapshot_quota.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

namespace uvnvr {
namespace {

constexpr int64_t kMb = 1024 * 1024;
// Walking a large snapshot tree on every image would be wasteful; the cached
// size is refreshed at this interval and adjusted by Commit() in between.
constexpr int64_t kSizeTtlMs = 60 * 1000;
// system_meta survives a restart. Writing it for every JPEG is unnecessary;
// losing at most half a minute of accounting on a crash is acceptable.
constexpr int64_t kPersistIntervalMs = 30 * 1000;

int64_t LocalDay(int64_t now_ms) {
  const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
  std::tm value{};
#ifdef _WIN32
  localtime_s(&value, &seconds);
#else
  localtime_r(&seconds, &value);
#endif
  return static_cast<int64_t>(value.tm_year + 1900) * 10000 +
         static_cast<int64_t>(value.tm_mon + 1) * 100 + value.tm_mday;
}

int64_t DefaultDirectorySize(const std::string& directory) {
  std::error_code error;
  if (!std::filesystem::exists(directory, error))
    return 0;
  int64_t total = 0;
  std::filesystem::recursive_directory_iterator it(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && it != end) {
    std::error_code entry_error;
    if (it->is_regular_file(entry_error)) {
      const auto size = it->file_size(entry_error);
      if (!entry_error)
        total += static_cast<int64_t>(size);
    }
    it.increment(error);
  }
  return total;
}

int64_t DefaultFreeBytes(const std::string& directory) {
#ifdef _WIN32
  ULARGE_INTEGER available;
  if (!GetDiskFreeSpaceExA(directory.c_str(), &available, nullptr, nullptr))
    return -1;
  return static_cast<int64_t>(available.QuadPart);
#else
  struct statvfs info {};
  if (statvfs(directory.c_str(), &info) != 0)
    return -1;
  return static_cast<int64_t>(info.f_bavail) * static_cast<int64_t>(info.f_frsize);
#endif
}

} // namespace

const char* SnapshotBlockReasonName(SnapshotBlockReason reason) {
  switch (reason) {
  case SnapshotBlockReason::kDailyLimit:
    return "ai.snapshotMaxPerDay";
  case SnapshotBlockReason::kTotalLimit:
    return "ai.snapshotMaxTotalMb";
  case SnapshotBlockReason::kLowDisk:
    return "ai.snapshotMinFreeMb";
  case SnapshotBlockReason::kNone:
    break;
  }
  return "none";
}

SnapshotQuota::SnapshotQuota(SizeProbe size_probe, FreeProbe free_probe)
    : size_probe_(std::move(size_probe)), free_probe_(std::move(free_probe)) {
  if (!size_probe_)
    size_probe_ = DefaultDirectorySize;
  if (!free_probe_)
    free_probe_ = DefaultFreeBytes;
}

SnapshotQuota::~SnapshotQuota() = default;

void SnapshotQuota::Configure(std::string directory, SnapshotQuotaLimits limits, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  directory_ = std::move(directory);
  limits_ = limits;
  dir_bytes_valid_ = false;
  dir_bytes_probed_ms_ = 0;
  // A Reload() keeps the in-memory counter: it is more accurate than the
  // throttled system_meta copy. Only a new calendar day resets it.
  const int64_t day = LocalDay(now_ms);
  if (day != day_) {
    day_ = day;
    today_ = 0;
  }
}

void SnapshotQuota::SetPersist(PersistFn persist) {
  std::lock_guard<std::mutex> lock(mutex_);
  persist_ = std::move(persist);
}

void SnapshotQuota::Seed(int64_t day, int64_t count, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (seeded_)
    return;
  seeded_ = true;
  const int64_t current = LocalDay(now_ms);
  if (day_ == 0)
    day_ = current;
  if (day == day_)
    today_ = std::max<int64_t>(0, count);
}

bool SnapshotQuota::Acquire(int64_t now_ms, SnapshotBlockReason* reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  RollDayLocked(now_ms);

  SnapshotBlockReason blocked = SnapshotBlockReason::kNone;
  if (limits_.max_per_day > 0 && today_ >= limits_.max_per_day) {
    blocked = SnapshotBlockReason::kDailyLimit;
  } else if (limits_.max_total_mb > 0) {
    RefreshSizeLocked(now_ms);
    if (dir_bytes_ >= limits_.max_total_mb * kMb)
      blocked = SnapshotBlockReason::kTotalLimit;
  }
  if (blocked == SnapshotBlockReason::kNone && limits_.min_free_mb > 0) {
    const int64_t free_bytes = ProbeFreeLocked();
    if (free_bytes >= 0 && free_bytes < limits_.min_free_mb * kMb)
      blocked = SnapshotBlockReason::kLowDisk;
  }

  if (blocked != SnapshotBlockReason::kNone) {
    last_blocked_ = true;
    last_reason_ = blocked;
    ++skipped_;
    if (reason != nullptr)
      *reason = blocked;
    return false;
  }

  ++today_;
  ++reserved_;
  last_blocked_ = false;
  last_reason_ = SnapshotBlockReason::kNone;
  if (reason != nullptr)
    *reason = SnapshotBlockReason::kNone;
  return true;
}

void SnapshotQuota::Commit(int64_t bytes, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reserved_ > 0)
    --reserved_;
  if (bytes > 0) {
    dir_bytes_ += bytes;
    dir_bytes_valid_ = true;
  }
  PersistLocked(now_ms, false);
}

void SnapshotQuota::Abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reserved_ > 0) {
    --reserved_;
    if (today_ > 0)
      --today_;
  }
}

void SnapshotQuota::Refresh(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  RefreshSizeLocked(now_ms);
}

void SnapshotQuota::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!persist_ || day_ == 0)
    return;
  persist_(day_, today_);
  persisted_ = true;
  last_persist_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
}

bool SnapshotQuota::Blocked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_blocked_;
}

SnapshotBlockReason SnapshotQuota::LastBlockReason() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_reason_;
}

int64_t SnapshotQuota::Today() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return today_;
}

int64_t SnapshotQuota::DirectoryBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dir_bytes_;
}

int64_t SnapshotQuota::FreeBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return free_bytes_;
}

uint64_t SnapshotQuota::Skipped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skipped_;
}

int64_t SnapshotQuota::LocalDay(int64_t now_ms) const {
  return uvnvr::LocalDay(now_ms);
}

void SnapshotQuota::RollDayLocked(int64_t now_ms) {
  const int64_t day = LocalDay(now_ms);
  if (day == day_)
    return;
  day_ = day;
  today_ = 0;
  PersistLocked(now_ms, true);
}

void SnapshotQuota::RefreshSizeLocked(int64_t now_ms) {
  if (directory_.empty()) {
    dir_bytes_ = 0;
    dir_bytes_valid_ = true;
    dir_bytes_probed_ms_ = now_ms;
    return;
  }
  if (dir_bytes_valid_ && now_ms - dir_bytes_probed_ms_ < kSizeTtlMs)
    return;
  const int64_t measured = size_probe_ ? size_probe_(directory_) : 0;
  if (measured >= 0) {
    dir_bytes_ = measured;
    dir_bytes_valid_ = true;
    dir_bytes_probed_ms_ = now_ms;
  }
}

int64_t SnapshotQuota::ProbeFreeLocked() {
  // Free space must be checked against the current state; unlike the directory
  // size it is cheap to query and changes as recordings are written.
  const std::string target = directory_.empty() ? std::string(".") : directory_;
  const int64_t measured = free_probe_ ? free_probe_(target) : -1;
  free_bytes_ = measured;
  return measured;
}

void SnapshotQuota::PersistLocked(int64_t now_ms, bool force) {
  if (!persist_ || day_ == 0)
    return;
  if (!force && persisted_ && now_ms - last_persist_ms_ < kPersistIntervalMs)
    return;
  persist_(day_, today_);
  persisted_ = true;
  last_persist_ms_ = now_ms;
}

} // namespace uvnvr
