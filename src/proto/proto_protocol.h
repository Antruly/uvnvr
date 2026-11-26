#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr::proto {

// Wire constants for the private protocol. Keep this file free of libuvcpp
// types so both the recorder and the SDK-side tests can include it.
constexpr uint32_t kMagic = 0x524E5655u; // wire bytes: 'U' 'V' 'N' 'R'
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr uint32_t kMaxPayload = 4u * 1024u * 1024u;
constexpr const char* kAlpn = "uvnvr/1";

enum Flags : uint16_t {
  kFlagResponse = 0x0001,
  kFlagError = 0x0002,
  kFlagFin = 0x0004,
  kFlagCompressed = 0x0008,
  kFlagEncrypted = 0x0010,
  kFlagStream = 0x0020,
};

enum Opcode : uint16_t {
  kOpHello = 0x0001,
  kOpHelloAck = 0x0002,
  kOpAuth = 0x0003,
  kOpAuthAck = 0x0004,
  kOpPing = 0x0005,
  kOpPong = 0x0006,
  kOpLogout = 0x0008,
  kOpSessionRenew = 0x0009,
  kOpSystemInfo = 0x0010,
  kOpChannelList = 0x0020,
  kOpRecordingList = 0x0021,
  kOpRecordingDays = 0x0022,
  kOpLiveOpen = 0x0030,
  kOpLiveAck = 0x0031,
  kOpLiveFrame = 0x0032,
  kOpLiveClose = 0x0033,
  kOpPtzControl = 0x0034,
  kOpDownloadOpen = 0x0040,
  kOpDownloadBegin = 0x0041,
  kOpDownloadChunk = 0x0042,
  kOpDownloadDone = 0x0043,
  // AI event subscription (additive in v1; see docs/private-protocol.md §7.3).
  kOpAiSubscribe = 0x0050,
  kOpAiSubscribeAck = 0x0051,
  kOpAiEvent = 0x0052,
  kOpAiUnsubscribe = 0x0053,
};

struct Header {
  uint16_t flags = 0;
  uint16_t opcode = 0;
  uint64_t session_id = 0;
  uint32_t request_id = 0;
  uint32_t payload_len = 0;
};

uint32_t Crc32(const uint8_t* data, size_t len);

// Serialises a complete frame: header + payload, with the CRC filled in.
std::vector<uint8_t> EncodeFrame(const Header& header, const void* payload, size_t payload_len);

// Decodes and validates the 32-byte header. Returns false and fills `error`
// for a bad magic, version, header length, reserved field or CRC.
bool DecodeHeader(const uint8_t* data, size_t len, Header* header, std::string* error);

// Recomputes the CRC of a complete frame (header + payload) and compares it
// with the field in the header.
bool VerifyFrame(const uint8_t* data, size_t len, const Header& header);

const char* OpcodeName(uint16_t opcode);

} // namespace uvnvr::proto
