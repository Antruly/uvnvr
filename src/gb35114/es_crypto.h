#pragma once

// GB 35114 B-level video encryption at the elementary-stream (ES) layer.
//
// Each H.264 access unit is sealed as a self-delimiting frame:
//
//   offset  size  field
//   0       4     magic "GBS1"
//   4       1     version (1)
//   5       1     flags (bit 0 = keyframe)
//   6       2     header size, big endian (56, tag included)
//   8       4     key id, big endian
//   12      8     sequence number, big endian
//   20      8     plaintext length, big endian
//   28      8     nonce
//   36      4     reserved, zero
//   40      16    HMAC-SM3 tag (truncated) over [0,40) || ciphertext
//   56      n     SM4-CTR ciphertext (same length as the access unit)
//
// The per-frame IV is SM3("GB35114/iv/v1" || iv_seed || nonce || sequence)[0..16).
// SM4-CTR is length preserving, so a frame occupies a constant amount of space
// relative to its plaintext; the tag makes a wrong key or a modified frame fail
// before any bytes reach the decoder.
//
// Encryption happens before PS muxing and before RTP packetisation. The sealed
// frames are an opaque byte stream for those layers, and because every frame
// carries its own length the receiver can walk them without relying on PES or
// RTP boundaries. Keeping the ciphertext self-delimiting is what lets this
// module compose with the GB/T 28181 PS/RTP pipeline without changing it.

#include "gb35114/vkek.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr::gb35114 {

inline constexpr std::string_view kSealedFrameMagic = "GBS1";
inline constexpr std::uint8_t kSealedFrameVersion = 1;
inline constexpr std::size_t kSealedFramePrefixSize = 40;
inline constexpr std::size_t kSealedFrameTagSize = 16;
inline constexpr std::size_t kSealedFrameHeaderSize = kSealedFramePrefixSize + kSealedFrameTagSize;
inline constexpr std::size_t kSealedFrameNonceSize = 8;
inline constexpr std::size_t kMaxEsFrameSize = 64u * 1024u * 1024u;
inline constexpr std::uint8_t kSealedFrameFlagKeyframe = 0x01;

struct SealedFrameInfo {
  std::uint32_t key_id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t plain_size = 0;
  bool keyframe = false;
};

std::size_t SealedFrameSize(std::size_t plaintext_size);

// Seals one access unit. `nonce` must be kSealedFrameNonceSize bytes and must
// not repeat for the same key and sequence; callers that do not have a natural
// nonce should use SealEsStream(), which generates one per frame.
bool SealEsFrame(const VideoKeys& keys, std::uint64_t sequence, bool keyframe,
                 const gm::Bytes& nonce, const gm::Bytes& es, gm::Bytes* sealed,
                 std::string* error = nullptr);
bool OpenEsFrame(const VideoKeys& keys, const gm::Bytes& sealed, gm::Bytes* es,
                 SealedFrameInfo* info = nullptr, std::string* error = nullptr);

// Returns the total size of the sealed frame starting at `offset` without
// decrypting it. Used to walk a sealed stream and by transport adapters.
bool NextSealedFrameSize(const gm::Bytes& sealed, std::size_t offset, std::size_t* frame_size,
                         std::string* error = nullptr);

// Reads the public header of the frame at `offset` (no key required). Useful
// for diagnostics and for platform-side stream inspection.
bool ReadSealedFrameInfo(const gm::Bytes& sealed, std::size_t offset, SealedFrameInfo* info,
                         std::size_t* frame_size, std::string* error = nullptr);

struct EsAccessUnit {
  gm::Bytes bytes;
  bool keyframe = false;
};

// Splits an Annex B H.264 stream into access units, preserving every input
// byte (including start codes and parameter sets). The splitter uses AUD,
// SPS/PPS and the first_mb_in_slice field of VCL NAL units, so a picture made
// of several slices stays one access unit.
std::vector<EsAccessUnit> SplitH264AccessUnits(const gm::Bytes& annex_b);

// Seals every access unit of an Annex B stream, prefixing one header per
// access unit. The output is a sequence of self-delimiting sealed frames.
bool SealEsStream(const VideoKeys& keys, const gm::Bytes& annex_b, gm::Bytes* sealed,
                  std::size_t* frame_count = nullptr, std::string* error = nullptr);
bool OpenEsStream(const VideoKeys& keys, const gm::Bytes& sealed, gm::Bytes* annex_b,
                  std::size_t* frame_count = nullptr, std::string* error = nullptr);

} // namespace uvnvr::gb35114
