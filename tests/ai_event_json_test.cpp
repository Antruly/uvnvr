// Contract tests for the private-protocol AI event payload.
//
// 1. AiEventJson() emits the same field spelling the REST event item uses,
//    plus the two real-time fields documented in ai_event_json.h.
// 2. The new AI opcodes frame and decode like every other v1 message.
//
// No recorder, OpenCV or database is involved.

#include "ai/ai_event_json.h"
#include "proto/proto_protocol.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

bool HasKey(const nlohmann::json& value, const char* key) {
  return value.contains(key);
}

void TestPlateEvent() {
  uvnvr::AiEventFields fields;
  fields.id = 4;
  fields.face = false;
  fields.channel_id = 1;
  fields.channel_name = "LabCam";
  fields.ts = 1795515031;
  fields.confidence = 0.93;
  fields.plate = "京A12345";
  fields.plate_norm = "京A12345";
  fields.color = "蓝";
  fields.x = 128;
  fields.y = 240;
  fields.w = 160;
  fields.h = 48;
  fields.engine = "stub";
  fields.alert = true;
  fields.image_url = "/api/v1/ai/events/4/snapshot?type=plate&kind=full";
  fields.crop_url = "/api/v1/ai/events/4/snapshot?type=plate&kind=crop";

  const nlohmann::json item = uvnvr::AiEventJson(fields);
  // REST item spellings.
  for (const char* key : {"id", "type", "channelId", "channelName", "ts", "plate",
                          "plateNorm", "confidence", "color", "box", "engine",
                          "createdAt", "imageUrl", "cropUrl"})
    Check(HasKey(item, key), key);
  // Real-time extras shared with /ws/events.
  Check(HasKey(item, "alert"), "alert");
  Check(item["type"] == "plate", "plate type");
  Check(item["id"] == 4, "plate id");
  Check(item["channelId"] == 1, "plate channelId");
  Check(item["channelName"] == "LabCam", "plate channelName");
  Check(item["plateNorm"] == "京A12345", "plate plateNorm");
  Check(item["box"]["x"] == 128 && item["box"]["y"] == 240 &&
            item["box"]["w"] == 160 && item["box"]["h"] == 48,
        "plate box");
  Check(item["alert"] == true, "plate alert");
  Check(!HasKey(item, "quality"), "plate has no face field");
  Check(!HasKey(item, "matchedFaceId"), "plate has no match field");
}

void TestFaceEvent() {
  uvnvr::AiEventFields fields;
  fields.id = 9;
  fields.face = true;
  fields.channel_id = 1;
  fields.channel_name = "LabCam";
  fields.ts = 1795515031;
  fields.confidence = 0.9;
  fields.quality = 0.81;
  fields.x = 213;
  fields.y = 120;
  fields.w = 106;
  fields.h = 120;
  fields.engine = "stub";
  fields.alert = true;
  fields.matched = true;
  fields.matched_face_id = 2;
  fields.matched_score = 0.87;
  fields.matched_name = "张三";
  fields.image_url = "/api/v1/ai/events/9/snapshot?type=face&kind=full";

  nlohmann::json item = uvnvr::AiEventJson(fields);
  for (const char* key : {"id", "type", "channelId", "channelName", "ts", "quality",
                          "confidence", "box", "engine", "matchedFaceId",
                          "matchedScore", "imageUrl", "cropUrl"})
    Check(HasKey(item, key), key);
  Check(HasKey(item, "matchedName"), "matchedName");
  Check(item["type"] == "face", "face type");
  Check(item["matchedFaceId"] == 2, "matchedFaceId");
  Check(item["matchedName"] == "张三", "matchedName value");
  Check(!HasKey(item, "plate"), "face has no plate field");

  fields.matched = false;
  item = uvnvr::AiEventJson(fields);
  Check(item["matchedFaceId"].is_null(), "unmatched matchedFaceId is null");
  Check(item["matchedName"].is_null(), "unmatched matchedName is null");
}

void TestFraming() {
  Check(std::string(uvnvr::proto::OpcodeName(uvnvr::proto::kOpAiSubscribe)) ==
            "AI_SUBSCRIBE",
        "opcode name AI_SUBSCRIBE");
  Check(std::string(uvnvr::proto::OpcodeName(uvnvr::proto::kOpAiSubscribeAck)) ==
            "AI_SUBSCRIBE_ACK",
        "opcode name AI_SUBSCRIBE_ACK");
  Check(std::string(uvnvr::proto::OpcodeName(uvnvr::proto::kOpAiEvent)) == "AI_EVENT",
        "opcode name AI_EVENT");
  Check(std::string(uvnvr::proto::OpcodeName(uvnvr::proto::kOpAiUnsubscribe)) ==
            "AI_UNSUBSCRIBE",
        "opcode name AI_UNSUBSCRIBE");

  const std::string subscription =
      R"({"subscriptionId":1,"channelId":0,"types":["plate","face"]})";
  uvnvr::proto::Header request;
  request.opcode = uvnvr::proto::kOpAiSubscribe;
  request.session_id = 0x1122334455667788ull;
  request.request_id = 7;
  const std::vector<uint8_t> frame = uvnvr::proto::EncodeFrame(
      request, subscription.data(), subscription.size());

  uvnvr::proto::Header decoded;
  std::string error;
  Check(uvnvr::proto::DecodeHeader(frame.data(), frame.size(), &decoded, &error),
        "AI_SUBSCRIBE header decodes");
  Check(decoded.opcode == uvnvr::proto::kOpAiSubscribe, "decoded opcode");
  Check(decoded.request_id == 7, "decoded request id");
  Check(decoded.session_id == 0x1122334455667788ull, "decoded session");
  Check(decoded.payload_len == subscription.size(), "decoded payload length");
  Check(uvnvr::proto::VerifyFrame(frame.data(), frame.size(), decoded), "decoded CRC");

  // A corrupted byte must fail the CRC, including in an AI event frame.
  uvnvr::proto::Header event;
  event.opcode = uvnvr::proto::kOpAiEvent;
  const std::vector<uint8_t> event_frame =
      uvnvr::proto::EncodeFrame(event, subscription.data(), subscription.size());
  std::vector<uint8_t> corrupted = event_frame;
  corrupted[uvnvr::proto::kHeaderSize + 1] ^= 0x01;
  Check(!uvnvr::proto::VerifyFrame(corrupted.data(), corrupted.size(), event),
        "corrupted AI_EVENT frame is rejected");
}

} // namespace

int main() {
  TestPlateEvent();
  TestFaceEvent();
  TestFraming();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("ai_event_json_test: all checks passed\n");
  return 0;
}
