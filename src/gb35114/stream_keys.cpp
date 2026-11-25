#include "gb35114/stream_keys.h"

#include <algorithm>
#include <cstring>

namespace uvnvr::gb35114 {
namespace {

void WriteBe16(gm::Bytes* out, std::size_t offset, std::uint16_t value) {
  (*out)[offset] = static_cast<unsigned char>((value >> 8) & 0xFF);
  (*out)[offset + 1] = static_cast<unsigned char>(value & 0xFF);
}

void WriteBe32(gm::Bytes* out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    (*out)[offset + static_cast<std::size_t>(i)] =
        static_cast<unsigned char>((value >> (24 - 8 * i)) & 0xFF);
}

std::uint16_t ReadBe16(const gm::Bytes& data, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) |
                                    data[offset + 1]);
}

std::uint32_t ReadBe32(const gm::Bytes& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i)
    value = (value << 8) | data[offset + static_cast<std::size_t>(i)];
  return value;
}

gm::Bytes MakeInfo(std::string_view label, std::uint32_t key_id, std::string_view stream_id) {
  gm::Bytes info(label.begin(), label.end());
  info.push_back(0);
  for (int shift = 24; shift >= 0; shift -= 8)
    info.push_back(static_cast<unsigned char>((key_id >> shift) & 0xFF));
  info.push_back(0);
  info.insert(info.end(), stream_id.begin(), stream_id.end());
  return info;
}

// HKDF-Expand (RFC 5869) with SM3 as the hash. Only 16-byte outputs are used
// here, so the counter stays at 1, but the loop is kept general.
bool HkdfExpandSm3(const gm::Bytes& prk, const gm::Bytes& info, std::size_t length,
                   gm::Bytes* output, std::string* error) {
  if (length > 255 * gm::kSm3DigestSize) {
    if (error)
      *error = "HKDF-SM3 output is too large";
    return false;
  }
  output->clear();
  gm::Bytes previous;
  std::uint8_t counter = 1;
  while (output->size() < length) {
    gm::Bytes input = previous;
    input.insert(input.end(), info.begin(), info.end());
    input.push_back(counter);
    previous = gm::HmacSm3(prk, input);
    if (previous.empty()) {
      if (error)
        *error = "HKDF-SM3 expansion failed";
      return false;
    }
    const std::size_t take = std::min(length - output->size(), previous.size());
    output->insert(output->end(), previous.begin(), previous.begin() + take);
    ++counter;
  }
  return true;
}

bool DeriveOne(const gm::Bytes& prk, std::string_view label, std::uint32_t key_id,
               std::string_view stream_id, gm::Bytes* key, std::string* error) {
  return HkdfExpandSm3(prk, MakeInfo(label, key_id, stream_id), gm::kSm4KeySize, key, error);
}

bool WrapWith(std::string_view public_key_pem, bool is_certificate, std::uint32_t key_id,
              const gm::Bytes& vkek, const gm::Bytes& salt, gm::Bytes* envelope,
              std::string* error) {
  if (!envelope) {
    if (error)
      *error = "VKEK envelope output is null";
    return false;
  }
  if (vkek.size() != kStreamVkekSize) {
    if (error)
      *error = "VKEK must be 16 bytes";
    return false;
  }
  if (salt.size() != kStreamKeySaltSize) {
    if (error)
      *error = "VKEK salt must be 16 bytes";
    return false;
  }

  gm::Bytes wrapped;
  const bool wrapped_ok = is_certificate
                              ? gm::Sm2EncryptCertificate(public_key_pem, vkek, &wrapped, error)
                              : gm::Sm2Encrypt(public_key_pem, vkek, &wrapped, error);
  if (!wrapped_ok)
    return false;

  gm::Bytes out(kStreamKeyEnvelopeHeaderSize + 4, 0);
  std::memcpy(out.data(), kStreamKeyEnvelopeMagic.data(), kStreamKeyEnvelopeMagic.size());
  out[4] = kStreamKeyEnvelopeVersion;
  out[5] = kStreamKeyEnvelopeAlgorithmSm2;
  WriteBe16(&out, 6, static_cast<std::uint16_t>(kStreamKeyEnvelopeHeaderSize));
  WriteBe32(&out, 8, key_id);
  std::memcpy(out.data() + 12, salt.data(), salt.size());
  WriteBe32(&out, kStreamKeyEnvelopeLengthOffset, static_cast<std::uint32_t>(wrapped.size()));
  out.insert(out.end(), wrapped.begin(), wrapped.end());

  *envelope = std::move(out);
  return true;
}

bool CreateWith(std::string_view public_key_pem, bool is_certificate, std::uint32_t key_id,
                gm::Bytes* envelope, gm::Bytes* vkek, gm::Bytes* salt, std::string* error) {
  if (!envelope) {
    if (error)
      *error = "VKEK envelope output is null";
    return false;
  }
  gm::Bytes local_vkek;
  gm::Bytes local_salt;
  if (!gm::RandomBytes(kStreamVkekSize, &local_vkek, error) ||
      !gm::RandomBytes(kStreamKeySaltSize, &local_salt, error)) {
    return false;
  }
  if (!WrapWith(public_key_pem, is_certificate, key_id, local_vkek, local_salt, envelope, error))
    return false;
  if (vkek)
    *vkek = std::move(local_vkek);
  if (salt)
    *salt = std::move(local_salt);
  return true;
}

} // namespace

bool WrapStreamVkek(std::string_view device_public_key_pem, std::uint32_t key_id, const gm::Bytes& vkek,
              const gm::Bytes& salt, gm::Bytes* envelope, std::string* error) {
  return WrapWith(device_public_key_pem, false, key_id, vkek, salt, envelope, error);
}

bool WrapStreamVkekForCertificate(std::string_view device_certificate_pem, std::uint32_t key_id,
                            const gm::Bytes& vkek, const gm::Bytes& salt, gm::Bytes* envelope,
                            std::string* error) {
  return WrapWith(device_certificate_pem, true, key_id, vkek, salt, envelope, error);
}

bool CreateStreamKeyEnvelope(std::string_view device_public_key_pem, std::uint32_t key_id,
                        gm::Bytes* envelope, gm::Bytes* vkek, gm::Bytes* salt, std::string* error) {
  return CreateWith(device_public_key_pem, false, key_id, envelope, vkek, salt, error);
}

bool CreateStreamKeyEnvelopeForCertificate(std::string_view device_certificate_pem, std::uint32_t key_id,
                                      gm::Bytes* envelope, gm::Bytes* vkek, gm::Bytes* salt,
                                      std::string* error) {
  return CreateWith(device_certificate_pem, true, key_id, envelope, vkek, salt, error);
}

bool ParseStreamKeyEnvelope(const gm::Bytes& envelope, StreamKeyEnvelope* parsed, std::string* error) {
  if (!parsed) {
    if (error)
      *error = "VKEK envelope output is null";
    return false;
  }
  if (envelope.size() < kStreamKeyEnvelopeHeaderSize + 4) {
    if (error)
      *error = "VKEK envelope is truncated";
    return false;
  }
  if (std::memcmp(envelope.data(), kStreamKeyEnvelopeMagic.data(), kStreamKeyEnvelopeMagic.size()) != 0) {
    if (error)
      *error = "VKEK envelope magic does not match";
    return false;
  }
  if (envelope[4] != kStreamKeyEnvelopeVersion) {
    if (error)
      *error = "unsupported VKEK envelope version";
    return false;
  }
  if (envelope[5] != kStreamKeyEnvelopeAlgorithmSm2) {
    if (error)
      *error = "unsupported VKEK wrapping algorithm";
    return false;
  }
  if (ReadBe16(envelope, 6) != kStreamKeyEnvelopeHeaderSize) {
    if (error)
      *error = "unsupported VKEK envelope header size";
    return false;
  }
  const std::uint32_t wrapped_length = ReadBe32(envelope, kStreamKeyEnvelopeLengthOffset);
  if (wrapped_length == 0 || envelope.size() != kStreamKeyEnvelopeHeaderSize + 4 + wrapped_length) {
    if (error)
      *error = "VKEK envelope length does not match its header";
    return false;
  }
  if (envelope[kStreamKeyEnvelopeHeaderSize + 4] != 0x04) {
    if (error)
      *error = "wrapped VKEK is not an uncompressed SM2 point";
    return false;
  }

  StreamKeyEnvelope result;
  result.key_id = ReadBe32(envelope, 8);
  result.salt.assign(envelope.begin() + 12, envelope.begin() + 12 + kStreamKeySaltSize);
  result.wrapped_vkek.assign(envelope.begin() + kStreamKeyEnvelopeHeaderSize + 4, envelope.end());
  *parsed = std::move(result);
  return true;
}

bool UnwrapStreamVkek(std::string_view device_private_key_pem, const gm::Bytes& envelope, gm::Bytes* vkek,
                StreamKeyEnvelope* parsed, std::string* error) {
  if (!vkek) {
    if (error)
      *error = "VKEK output is null";
    return false;
  }
  StreamKeyEnvelope local;
  if (!ParseStreamKeyEnvelope(envelope, &local, error))
    return false;

  gm::Bytes unwrapped;
  if (!gm::Sm2Decrypt(device_private_key_pem, local.wrapped_vkek, &unwrapped, error))
    return false;
  if (unwrapped.size() != kStreamVkekSize) {
    if (error)
      *error = "unwrapped VKEK has the wrong length";
    return false;
  }

  *vkek = std::move(unwrapped);
  if (parsed)
    *parsed = std::move(local);
  return true;
}

bool DeriveStreamKeys(const gm::Bytes& vkek, std::uint32_t key_id, const gm::Bytes& salt,
                     std::string_view stream_id, VideoKeys* keys, std::string* error) {
  if (!keys) {
    if (error)
      *error = "video key output is null";
    return false;
  }
  if (vkek.size() != kStreamVkekSize) {
    if (error)
      *error = "VKEK must be 16 bytes";
    return false;
  }
  if (salt.size() != kStreamKeySaltSize) {
    if (error)
      *error = "VKEK salt must be 16 bytes";
    return false;
  }

  const gm::Bytes prk = gm::HmacSm3(salt, vkek);
  if (prk.empty()) {
    if (error)
      *error = "HKDF-SM3 extract failed";
    return false;
  }

  VideoKeys result;
  result.key_id = key_id;
  if (!DeriveOne(prk, "GB35114/v1/video-key", key_id, stream_id, &result.video_key, error) ||
      !DeriveOne(prk, "GB35114/v1/iv-seed", key_id, stream_id, &result.iv_seed, error) ||
      !DeriveOne(prk, "GB35114/v1/mac-key", key_id, stream_id, &result.mac_key, error)) {
    return false;
  }
  *keys = std::move(result);
  return true;
}

bool DeriveStreamKeys(const gm::Bytes& vkek, const StreamKeyEnvelope& envelope,
                     std::string_view stream_id, VideoKeys* keys, std::string* error) {
  return DeriveStreamKeys(vkek, envelope.key_id, envelope.salt, stream_id, keys, error);
}

} // namespace uvnvr::gb35114
