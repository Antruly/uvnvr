#pragma once

// Serialisation of one AI recognition event for the private protocol.
//
// The private-protocol AI subscription is a real-time push, but its payload
// reuses the REST event item field names (`GET /api/v1/ai/events`) so a client
// can share parsing code between the two entry points. Two fields that only
// exist on the /ws/events real-time message are carried as well:
//
//   * `alert`        — the watchlist hit flag;
//   * `matchedName`  — the face-library name, mirroring WS `matched.name`.
//
// Everything else is byte-for-byte the same spelling as the REST item. The
// private protocol wraps the object with `subscriptionId` (§7.3).
//
// This header is deliberately free of OpenCV and of the AI service so the
// field contract can be unit-tested without the recognition pipeline.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace uvnvr {

struct AiEventFields {
  int64_t id = 0;
  bool face = false; // false = plate
  int64_t channel_id = 0;
  std::string channel_name;
  int64_t ts = 0; // epoch seconds
  double confidence = 0.0;
  double quality = 0.0; // face only
  std::string plate;
  std::string plate_norm;
  std::string color;
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  std::string engine;
  bool alert = false;
  bool matched = false; // face only
  int64_t matched_face_id = 0;
  double matched_score = 0.0;
  std::string matched_name;
  std::string image_url;
  std::string crop_url;
};

inline nlohmann::json AiEventJson(const AiEventFields& fields) {
  nlohmann::json item{{"id", fields.id},
                      {"type", fields.face ? "face" : "plate"},
                      {"channelId", fields.channel_id},
                      {"channelName", fields.channel_name},
                      {"ts", fields.ts},
                      {"confidence", fields.confidence},
                      {"box", nlohmann::json{{"x", fields.x},
                                             {"y", fields.y},
                                             {"w", fields.w},
                                             {"h", fields.h}}},
                      {"engine", fields.engine},
                      {"alert", fields.alert},
                      {"createdAt", fields.ts},
                      {"imageUrl", fields.image_url},
                      {"cropUrl", fields.crop_url}};
  if (fields.face) {
    item["quality"] = fields.quality;
    item["matchedFaceId"] =
        fields.matched ? nlohmann::json(fields.matched_face_id) : nlohmann::json(nullptr);
    item["matchedScore"] = fields.matched_score;
    item["matchedName"] = fields.matched ? nlohmann::json(fields.matched_name)
                                         : nlohmann::json(nullptr);
  } else {
    item["plate"] = fields.plate;
    item["plateNorm"] = fields.plate_norm;
    item["color"] = fields.color;
  }
  return item;
}

} // namespace uvnvr
