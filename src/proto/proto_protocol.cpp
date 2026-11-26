#include "proto/proto_protocol.h"

#include <array>
#include <cstring>

namespace uvnvr::proto {
namespace {

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
  return static_cast<uint64_t>(ReadU32(p)) | (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
}

uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

void WriteU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xff);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void WriteU64(uint8_t* p, uint64_t value) {
  WriteU32(p, static_cast<uint32_t>(value & 0xffffffffu));
  WriteU32(p + 4, static_cast<uint32_t>(value >> 32));
}

void WriteU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xff);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

// A small table-driven CRC32 (IEEE 802.3, reflected). Implemented locally so
// the protocol does not depend on zlib's internal state or ABI.
std::array<uint32_t, 256> MakeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    table[i] = crc;
  }
  return table;
}

const std::array<uint32_t, 256>& CrcTable() {
  static const std::array<uint32_t, 256> table = MakeCrcTable();
  return table;
}

} // namespace

uint32_t Crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  const auto& table = CrcTable();
  for (size_t i = 0; i < len; ++i)
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> EncodeFrame(const Header& header, const void* payload, size_t payload_len) {
  std::vector<uint8_t> frame(kHeaderSize + payload_len);
  uint8_t* p = frame.data();
  WriteU32(p + 0, kMagic);
  p[4] = kVersion;
  p[5] = static_cast<uint8_t>(kHeaderSize);
  WriteU16(p + 6, header.flags);
  WriteU16(p + 8, header.opcode);
  WriteU16(p + 10, 0);
  WriteU64(p + 12, header.session_id);
  WriteU32(p + 20, header.request_id);
  WriteU32(p + 24, static_cast<uint32_t>(payload_len));
  WriteU32(p + 28, 0);
  if (payload_len != 0 && payload != nullptr)
    std::memcpy(p + kHeaderSize, payload, payload_len);

  const uint32_t crc = Crc32(p, frame.size());
  WriteU32(p + 28, crc);
  return frame;
}

bool DecodeHeader(const uint8_t* data, size_t len, Header* header, std::string* error) {
  if (len < kHeaderSize) {
    if (error)
      *error = "truncated header";
    return false;
  }
  if (ReadU32(data) != kMagic) {
    if (error)
      *error = "bad magic";
    return false;
  }
  if (data[4] != kVersion) {
    if (error)
      *error = "unsupported protocol version";
    return false;
  }
  if (data[5] != kHeaderSize) {
    if (error)
      *error = "unsupported header length";
    return false;
  }
  if (ReadU16(data + 10) != 0) {
    if (error)
      *error = "reserved header field is not zero";
    return false;
  }
  header->flags = ReadU16(data + 6);
  header->opcode = ReadU16(data + 8);
  header->session_id = ReadU64(data + 12);
  header->request_id = ReadU32(data + 20);
  header->payload_len = ReadU32(data + 24);
  if (header->payload_len > kMaxPayload) {
    if (error)
      *error = "payload exceeds the maximum frame size";
    return false;
  }
  return true;
}

bool VerifyFrame(const uint8_t* data, size_t len, const Header& header) {
  if (len < kHeaderSize + header.payload_len)
    return false;
  std::vector<uint8_t> copy(data, data + kHeaderSize);
  for (size_t i = 28; i < 32; ++i)
    copy[i] = 0;
  uint32_t crc = 0xFFFFFFFFu;
  const auto& table = CrcTable();
  for (uint8_t byte : copy)
    crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  for (size_t i = kHeaderSize; i < kHeaderSize + header.payload_len; ++i)
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  crc ^= 0xFFFFFFFFu;
  return crc == ReadU32(data + 28);
}

const char* OpcodeName(uint16_t opcode) {
  switch (opcode) {
  case kOpHello:
    return "HELLO";
  case kOpHelloAck:
    return "HELLO_ACK";
  case kOpAuth:
    return "AUTH";
  case kOpAuthAck:
    return "AUTH_ACK";
  case kOpPing:
    return "PING";
  case kOpPong:
    return "PONG";
  case kOpLogout:
    return "LOGOUT";
  case kOpSessionRenew:
    return "SESSION_RENEW";
  case kOpSystemInfo:
    return "SYSTEM_INFO";
  case kOpChannelList:
    return "CHANNEL_LIST";
  case kOpRecordingList:
    return "RECORDING_LIST";
  case kOpRecordingDays:
    return "RECORDING_DAYS";
  case kOpLiveOpen:
    return "LIVE_OPEN";
  case kOpLiveAck:
    return "LIVE_ACK";
  case kOpLiveFrame:
    return "LIVE_FRAME";
  case kOpLiveClose:
    return "LIVE_CLOSE";
  case kOpPtzControl:
    return "PTZ_CONTROL";
  case kOpDownloadOpen:
    return "DOWNLOAD_OPEN";
  case kOpDownloadBegin:
    return "DOWNLOAD_BEGIN";
  case kOpDownloadChunk:
    return "DOWNLOAD_CHUNK";
  case kOpDownloadDone:
    return "DOWNLOAD_DONE";
  case kOpAiSubscribe:
    return "AI_SUBSCRIBE";
  case kOpAiSubscribeAck:
    return "AI_SUBSCRIBE_ACK";
  case kOpAiEvent:
    return "AI_EVENT";
  case kOpAiUnsubscribe:
    return "AI_UNSUBSCRIBE";
  default:
    return "UNKNOWN";
  }
}

} // namespace uvnvr::proto
