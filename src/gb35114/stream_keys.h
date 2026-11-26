#pragma once

// GB 35114 B-level stream key distribution: the platform generates a random VKEK and
// sends it to the device wrapped with the device's SM2 public key. The device
// unwraps it with its private key and both sides derive the same per-stream
// working keys with HKDF-SM3.
//
// The envelope below is the SIP-carried form of the wrapped key. It is
// deliberately self-describing (version, algorithm, key id, salt) and carries
// no secret in clear: only the SM2-wrapped VKEK does.
//
//   offset  size  field
//   0       4     magic "GVKE"
//   4       1     version (1)
//   5       1     key wrapping algorithm (1 = SM2 C1||C3||C2)
//   6       2     header size, big endian (40)
//   8       4     key id, big endian
//   12      16    salt (HKDF extract salt; not secret)
//   28      12    reserved, zero
//   40      4     wrapped VKEK length, big endian
//   44      n     wrapped VKEK (SM2 ciphertext, standard C1||C3||C2)
//
// Work-key derivation (HKDF-SM3, RFC 5869 construction with SM3):
//
//   PRK        = HMAC-SM3(salt, VKEK)
//   video-key  = HKDF-Expand(PRK, "GB35114/v1/video-key" | 0x00 | key_id |
//                            0x00 | stream_id, 16)
//   iv-seed    = HKDF-Expand(PRK, "GB35114/v1/iv-seed"  | ... , 16)
//   mac-key    = HKDF-Expand(PRK, "GB35114/v1/mac-key"  | ... , 16)
//
// The labels and the stream binding keep one VKEK from producing the same key
// on two channels. The exact derivation is a UvNVR profile until the GB 35114
// wire-format text is available; see docs/gb35114-b-level.md.

#include "crypto/sm.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb35114 {

inline constexpr std::size_t kStreamVkekSize = 16;
inline constexpr std::size_t kStreamKeySaltSize = 16;
inline constexpr std::string_view kStreamKeyEnvelopeMagic = "GVKE";
inline constexpr std::uint8_t kStreamKeyEnvelopeVersion = 1;
inline constexpr std::uint8_t kStreamKeyEnvelopeAlgorithmSm2 = 1;
inline constexpr std::size_t kStreamKeyEnvelopeHeaderSize = 40;
inline constexpr std::size_t kStreamKeyEnvelopeLengthOffset = 40;

struct StreamKeyEnvelope {
  std::uint32_t key_id = 0;
  gm::Bytes salt;
  gm::Bytes wrapped_vkek;
};

// The three keys both sides derive from a VKEK. `video_key` drives SM4-CTR,
// `iv_seed` binds per-frame IVs to this key/stream, and `mac_key` keys the
// frame authenticity tag.
struct VideoKeys {
  std::uint32_t key_id = 0;
  gm::Bytes video_key;
  gm::Bytes iv_seed;
  gm::Bytes mac_key;
};

// Wraps `vkek` for the holder of `device_public_key_pem`. `vkek` and `salt`
// must both be 16 bytes. The result is the envelope described above.
bool WrapStreamVkek(std::string_view device_public_key_pem, std::uint32_t key_id, const gm::Bytes& vkek,
              const gm::Bytes& salt, gm::Bytes* envelope, std::string* error = nullptr);
bool WrapStreamVkekForCertificate(std::string_view device_certificate_pem, std::uint32_t key_id,
                            const gm::Bytes& vkek, const gm::Bytes& salt, gm::Bytes* envelope,
                            std::string* error = nullptr);

// Generates a random VKEK and salt, then wraps it. The caller (the platform)
// keeps `vkek` and `salt`; the device only receives `envelope`.
bool CreateStreamKeyEnvelope(std::string_view device_public_key_pem, std::uint32_t key_id,
                        gm::Bytes* envelope, gm::Bytes* vkek, gm::Bytes* salt,
                        std::string* error = nullptr);
bool CreateStreamKeyEnvelopeForCertificate(std::string_view device_certificate_pem, std::uint32_t key_id,
                                      gm::Bytes* envelope, gm::Bytes* vkek, gm::Bytes* salt,
                                      std::string* error = nullptr);

// Parses and validates the public envelope header.
bool ParseStreamKeyEnvelope(const gm::Bytes& envelope, StreamKeyEnvelope* parsed,
                       std::string* error = nullptr);

// Device side: unwraps the VKEK with the device SM2 private key.
bool UnwrapStreamVkek(std::string_view device_private_key_pem, const gm::Bytes& envelope, gm::Bytes* vkek,
                StreamKeyEnvelope* parsed, std::string* error = nullptr);

// Derives the per-stream working keys. Both arguments must come from the same
// VKEK exchange; `stream_id` is the GB/T 28181 channel id (or another stable
// stream identifier).
bool DeriveStreamKeys(const gm::Bytes& vkek, std::uint32_t key_id, const gm::Bytes& salt,
                     std::string_view stream_id, VideoKeys* keys, std::string* error = nullptr);
bool DeriveStreamKeys(const gm::Bytes& vkek, const StreamKeyEnvelope& envelope,
                     std::string_view stream_id, VideoKeys* keys, std::string* error = nullptr);

} // namespace uvnvr::gb35114
