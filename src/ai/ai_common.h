#pragma once

#include <cstdint>
#include <string>

namespace uvnvr {

// Global AI settings, mirrored by the ai.* rows in the settings table.
struct AiSettings {
  bool enabled = false;
  bool plate_enabled = true;
  bool face_enabled = true;
  int interval_ms = 1000;
  int queue_depth = 8;
  int plate_workers = 2;
  int face_workers = 2;
  double min_plate_confidence = 0.6;
  double min_face_quality = 0.3;
  double face_match_threshold = 0.75;
  int snapshot_retention_days = 30;
  bool snapshot_encrypt = false;
  // Hard caps for snapshot writes. Zero disables the individual check.
  int64_t snapshot_max_per_day = 20000;
  int64_t snapshot_max_total_mb = 2048;
  int64_t snapshot_min_free_mb = 2048;
  int dedup_seconds = 10;
  int face_dedup_seconds = 10;
  bool stub = false;
  // InspireFace resource-pack directory (relative paths resolve under data_dir)
  // and detector input level; -1 keeps the SDK default.
  std::string face_model_dir;
  int face_pixel_level = -1;
  // Legacy single plate watchlist. It is still read as a blacklist when
  // ai.plate.blacklist is empty, so existing installations keep alerting.
  std::string plate_watchlist;
  // Plate alert rules. The whitelist wins: a plate on both lists never raises
  // an alert. Entries are comma/semicolon/space separated and normalised.
  std::string plate_blacklist;
  std::string plate_whitelist;
  // ai_faces.id values that raise an alert when matched.
  std::string face_watchlist;
};

// Per-channel overrides from ai_channel_settings. interval_ms == 0 means
// "inherit ai.intervalMs".
struct AiChannelSettings {
  int64_t channel_id = 0;
  bool plate_enabled = false;
  bool face_enabled = false;
  int interval_ms = 0;
  std::string plate_roi;
  std::string face_roi;
  // Per-channel sensitivity overrides. A negative value inherits the global
  // ai.minPlateConfidence / ai.minFaceQuality.
  double min_plate_confidence = -1.0;
  double min_face_quality = -1.0;
};

struct AiChannelInfo {
  int64_t id = 0;
  std::string name;
  bool enabled = true;
  bool sub_enabled = false;
};

// Snapshot of the settings a worker needs while processing one frame. Copied
// into each queued frame so a config change cannot race with a running worker.
struct AiRuntimeConfig {
  bool stub = false;
  double min_plate_confidence = 0.6;
  double min_face_quality = 0.3;
  double face_match_threshold = 0.75;
  int dedup_seconds = 10;
  int face_dedup_seconds = 10;
  std::string face_model_dir;
  int face_pixel_level = -1;
  std::string plate_watchlist;
  std::string plate_blacklist;
  std::string plate_whitelist;
  std::string face_watchlist;
};

} // namespace uvnvr
