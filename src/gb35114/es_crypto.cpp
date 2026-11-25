#include "gb35114/es_crypto.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <openssl/crypto.h>

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

void WriteBe64(gm::Bytes* out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    (*out)[offset + static_cast<std::size_t>(i)] =
        static_cast<unsigned char>((value >> (56 - 8 * i)) & 0xFF);
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

std::uint64_t ReadBe64(const gm::Bytes& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value = (value << 8) | data[offset + static_cast<std::size_t>(i)];
  return value;
}

bool CheckKeys(const VideoKeys& keys, std::string* error) {
  if (keys.video_key.size() != gm::kSm4KeySize || keys.iv_seed.size() != gm::kSm4IvSize ||
      keys.mac_key.size() != gm::kSm4KeySize) {
    if (error)
      *error = "video keys are not derived (expected 16-byte key, IV seed and MAC key)";
    return false;
  }
  return true;
}

bool DeriveFrameIv(const VideoKeys& keys, std::uint64_t sequence, const gm::Bytes& nonce,
                   gm::Bytes* iv, std::string* error) {
  if (nonce.size() != kSealedFrameNonceSize) {
    if (error)
      *error = "sealed frame nonce must be 8 bytes";
    return false;
  }
  gm::Bytes material;
  material.reserve(32);
  static constexpr std::string_view kDomain = "GB35114/iv/v1";
  material.insert(material.end(), kDomain.begin(), kDomain.end());
  material.insert(material.end(), keys.iv_seed.begin(), keys.iv_seed.end());
  material.insert(material.end(), nonce.begin(), nonce.end());
  for (int shift = 56; shift >= 0; shift -= 8)
    material.push_back(static_cast<unsigned char>((sequence >> shift) & 0xFF));

  const gm::Bytes digest = gm::Sm3(material.data(), material.size());
  if (digest.size() != gm::kSm3DigestSize) {
    if (error)
      *error = "cannot derive the sealed frame IV";
    return false;
  }
  iv->assign(digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(gm::kSm4IvSize));
  return true;
}

bool ComputeTag(const gm::Bytes& mac_key, const gm::Bytes& sealed, gm::Bytes* tag,
                std::string* error) {
  gm::Bytes authenticated;
  authenticated.reserve(kSealedFramePrefixSize + (sealed.size() - kSealedFrameHeaderSize));
  authenticated.insert(authenticated.end(), sealed.begin(),
                       sealed.begin() + static_cast<std::ptrdiff_t>(kSealedFramePrefixSize));
  authenticated.insert(authenticated.end(),
                       sealed.begin() + static_cast<std::ptrdiff_t>(kSealedFrameHeaderSize),
                       sealed.end());
  const gm::Bytes full = gm::HmacSm3(mac_key, authenticated);
  if (full.size() < kSealedFrameTagSize) {
    if (error)
      *error = "cannot compute the sealed frame authentication tag";
    return false;
  }
  tag->assign(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(kSealedFrameTagSize));
  return true;
}

bool ValidateHeader(const gm::Bytes& sealed, std::size_t offset, std::size_t* frame_size,
                    std::uint32_t* key_id, std::uint64_t* sequence, std::uint64_t* plain_size,
                    std::uint8_t* flags, std::string* error) {
  if (offset > sealed.size()) {
    if (error)
      *error = "sealed frame offset is past the end of the stream";
    return false;
  }
  if (sealed.size() - offset < kSealedFrameHeaderSize) {
    if (error)
      *error = "sealed frame header is truncated";
    return false;
  }
  if (std::memcmp(sealed.data() + offset, kSealedFrameMagic.data(), kSealedFrameMagic.size()) !=
      0) {
    if (error)
      *error = "sealed frame magic does not match";
    return false;
  }
  if (sealed[offset + 4] != kSealedFrameVersion) {
    if (error)
      *error = "unsupported sealed frame version";
    return false;
  }
  if (ReadBe16(sealed, offset + 6) != kSealedFrameHeaderSize) {
    if (error)
      *error = "unsupported sealed frame header size";
    return false;
  }
  const std::uint64_t payload_size = ReadBe64(sealed, offset + 20);
  if (payload_size > kMaxEsFrameSize) {
    if (error)
      *error = "sealed frame payload exceeds the size limit";
    return false;
  }
  const std::uint64_t total = static_cast<std::uint64_t>(kSealedFrameHeaderSize) + payload_size;
  if (total < kSealedFrameHeaderSize ||
      static_cast<std::uint64_t>(sealed.size() - offset) < total) {
    if (error)
      *error = "sealed frame is truncated";
    return false;
  }
  *frame_size = static_cast<std::size_t>(total);
  *key_id = ReadBe32(sealed, offset + 8);
  *sequence = ReadBe64(sealed, offset + 12);
  *plain_size = payload_size;
  *flags = sealed[offset + 5];
  return true;
}

class BitReader {
public:
  BitReader(const gm::Bytes& data, std::size_t offset) : data_(data), byte_(offset), bit_(0) {}

  bool ReadBit(unsigned* value) {
    if (byte_ >= data_.size())
      return false;
    *value = (data_[byte_] >> (7 - bit_)) & 1;
    if (++bit_ == 8) {
      bit_ = 0;
      ++byte_;
    }
    return true;
  }

  bool ReadBits(int count, std::uint32_t* value) {
    std::uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
      unsigned bit = 0;
      if (!ReadBit(&bit))
        return false;
      result = (result << 1) | bit;
    }
    *value = result;
    return true;
  }

  // ue(v) from H.264 9.1.
  bool ReadUnsignedExpGolomb(std::uint32_t* value) {
    int leading_zeros = 0;
    while (true) {
      unsigned bit = 0;
      if (!ReadBit(&bit))
        return false;
      if (bit != 0)
        break;
      if (++leading_zeros > 31)
        return false;
    }
    std::uint32_t suffix = 0;
    if (leading_zeros > 0 && !ReadBits(leading_zeros, &suffix))
      return false;
    const std::uint64_t result = ((static_cast<std::uint64_t>(1) << leading_zeros) - 1) + suffix;
    if (result > std::numeric_limits<std::uint32_t>::max())
      return false;
    *value = static_cast<std::uint32_t>(result);
    return true;
  }

private:
  const gm::Bytes& data_;
  std::size_t byte_;
  int bit_;
};

struct NalUnit {
  std::size_t begin = 0;
  std::size_t header = 0;
  std::size_t end = 0;
  std::uint8_t type = 0;
  bool vcl = false;
  bool first_mb_zero = false;
};

std::vector<NalUnit> FindNalUnits(const gm::Bytes& data) {
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i + 3 <= data.size();) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      starts.push_back(i);
      i += 3;
    } else {
      ++i;
    }
  }

  std::vector<NalUnit> units;
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const std::size_t begin = starts[index];
    const std::size_t header = begin + 3;
    if (header >= data.size())
      continue;
    NalUnit unit;
    unit.begin = begin;
    unit.header = header;
    unit.end = index + 1 < starts.size() ? starts[index + 1] : data.size();
    unit.type = static_cast<std::uint8_t>(data[header] & 0x1F);
    unit.vcl = unit.type >= 1 && unit.type <= 5;
    if (unit.vcl && header + 1 < unit.end) {
      BitReader reader(data, header + 1);
      std::uint32_t first_mb = 0;
      if (reader.ReadUnsignedExpGolomb(&first_mb))
        unit.first_mb_zero = first_mb == 0;
    }
    units.push_back(unit);
  }
  return units;
}

bool StartsNewAccessUnit(const NalUnit& unit, bool current_has_vcl) {
  if (unit.type == 9) // access unit delimiter
    return true;
  if ((unit.type == 7 || unit.type == 8) && current_has_vcl) // SPS/PPS before a picture
    return true;
  if (unit.type == 6 && current_has_vcl) // SEI belonging to the next picture
    return true;
  return unit.vcl && unit.first_mb_zero && current_has_vcl;
}

} // namespace

std::size_t SealedFrameSize(std::size_t plaintext_size) {
  return kSealedFrameHeaderSize + plaintext_size;
}

bool SealEsFrame(const VideoKeys& keys, std::uint64_t sequence, bool keyframe,
                 const gm::Bytes& nonce, const gm::Bytes& es, gm::Bytes* sealed,
                 std::string* error) {
  if (!sealed) {
    if (error)
      *error = "sealed frame output is null";
    return false;
  }
  if (!CheckKeys(keys, error))
    return false;
  if (es.empty()) {
    if (error)
      *error = "cannot seal an empty access unit";
    return false;
  }
  if (es.size() > kMaxEsFrameSize) {
    if (error)
      *error = "access unit exceeds the sealed frame size limit";
    return false;
  }

  gm::Bytes iv;
  if (!DeriveFrameIv(keys, sequence, nonce, &iv, error))
    return false;

  gm::Bytes out(kSealedFrameHeaderSize, 0);
  std::memcpy(out.data(), kSealedFrameMagic.data(), kSealedFrameMagic.size());
  out[4] = kSealedFrameVersion;
  out[5] = keyframe ? kSealedFrameFlagKeyframe : 0;
  WriteBe16(&out, 6, static_cast<std::uint16_t>(kSealedFrameHeaderSize));
  WriteBe32(&out, 8, keys.key_id);
  WriteBe64(&out, 12, sequence);
  WriteBe64(&out, 20, static_cast<std::uint64_t>(es.size()));
  std::memcpy(out.data() + 28, nonce.data(), nonce.size());
  // Bytes 36..40 are reserved and stay zero. The tag at 40..56 is filled below.
  out.resize(kSealedFrameHeaderSize + es.size());

  gm::Bytes ciphertext;
  if (!gm::Sm4CtrCrypt(keys.video_key, iv, 0, es, &ciphertext, error))
    return false;
  std::memcpy(out.data() + kSealedFrameHeaderSize, ciphertext.data(), ciphertext.size());

  gm::Bytes tag;
  if (!ComputeTag(keys.mac_key, out, &tag, error))
    return false;
  std::memcpy(out.data() + kSealedFramePrefixSize, tag.data(), tag.size());

  *sealed = std::move(out);
  return true;
}

bool OpenEsFrame(const VideoKeys& keys, const gm::Bytes& sealed, gm::Bytes* es,
                 SealedFrameInfo* info, std::string* error) {
  if (!es) {
    if (error)
      *error = "access unit output is null";
    return false;
  }
  if (!CheckKeys(keys, error))
    return false;

  std::size_t frame_size = 0;
  std::uint32_t key_id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t plain_size = 0;
  std::uint8_t flags = 0;
  if (!ValidateHeader(sealed, 0, &frame_size, &key_id, &sequence, &plain_size, &flags, error))
    return false;
  if (frame_size != sealed.size()) {
    if (error)
      *error = "sealed frame buffer contains trailing bytes";
    return false;
  }
  if (key_id != keys.key_id) {
    if (error)
      *error = "sealed frame uses key id " + std::to_string(key_id) + " but the platform holds " +
               std::to_string(keys.key_id);
    return false;
  }

  gm::Bytes tag;
  if (!ComputeTag(keys.mac_key, sealed, &tag, error))
    return false;
  if (CRYPTO_memcmp(tag.data(), sealed.data() + kSealedFramePrefixSize, kSealedFrameTagSize) != 0) {
    if (error)
      *error = "sealed frame authentication failed (wrong key or corrupted data)";
    return false;
  }

  const gm::Bytes nonce(sealed.begin() + 28, sealed.begin() + 28 + kSealedFrameNonceSize);
  gm::Bytes iv;
  if (!DeriveFrameIv(keys, sequence, nonce, &iv, error))
    return false;

  const gm::Bytes ciphertext(sealed.begin() + static_cast<std::ptrdiff_t>(kSealedFrameHeaderSize),
                             sealed.end());
  gm::Bytes plaintext;
  if (!gm::Sm4CtrCrypt(keys.video_key, iv, 0, ciphertext, &plaintext, error))
    return false;
  if (plaintext.size() != plain_size) {
    if (error)
      *error = "sealed frame plaintext length does not match its header";
    return false;
  }

  if (info) {
    info->key_id = key_id;
    info->sequence = sequence;
    info->plain_size = plain_size;
    info->keyframe = (flags & kSealedFrameFlagKeyframe) != 0;
  }
  *es = std::move(plaintext);
  return true;
}

bool ReadSealedFrameInfo(const gm::Bytes& sealed, std::size_t offset, SealedFrameInfo* info,
                         std::size_t* frame_size, std::string* error) {
  if (!info || !frame_size) {
    if (error)
      *error = "sealed frame info output is null";
    return false;
  }
  std::size_t size = 0;
  std::uint32_t key_id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t plain_size = 0;
  std::uint8_t flags = 0;
  if (!ValidateHeader(sealed, offset, &size, &key_id, &sequence, &plain_size, &flags, error))
    return false;
  info->key_id = key_id;
  info->sequence = sequence;
  info->plain_size = plain_size;
  info->keyframe = (flags & kSealedFrameFlagKeyframe) != 0;
  *frame_size = size;
  return true;
}

bool NextSealedFrameSize(const gm::Bytes& sealed, std::size_t offset, std::size_t* frame_size,
                         std::string* error) {
  SealedFrameInfo info;
  return ReadSealedFrameInfo(sealed, offset, &info, frame_size, error);
}

std::vector<EsAccessUnit> SplitH264AccessUnits(const gm::Bytes& annex_b) {
  std::vector<EsAccessUnit> access_units;
  if (annex_b.empty())
    return access_units;

  const std::vector<NalUnit> nals = FindNalUnits(annex_b);
  if (nals.empty()) {
    access_units.push_back({annex_b, false});
    return access_units;
  }

  std::size_t au_begin = 0;
  bool au_has_vcl = false;
  bool au_keyframe = false;
  for (std::size_t index = 0; index < nals.size(); ++index) {
    const NalUnit& unit = nals[index];
    if (index > 0 && StartsNewAccessUnit(unit, au_has_vcl)) {
      EsAccessUnit completed;
      completed.bytes.assign(annex_b.begin() + static_cast<std::ptrdiff_t>(au_begin),
                             annex_b.begin() + static_cast<std::ptrdiff_t>(unit.begin));
      completed.keyframe = au_keyframe;
      if (!completed.bytes.empty())
        access_units.push_back(std::move(completed));
      au_begin = unit.begin;
      au_has_vcl = false;
      au_keyframe = false;
    }
    if (unit.vcl) {
      au_has_vcl = true;
      if (unit.type == 5)
        au_keyframe = true;
    }
  }

  EsAccessUnit last;
  last.bytes.assign(annex_b.begin() + static_cast<std::ptrdiff_t>(au_begin), annex_b.end());
  last.keyframe = au_keyframe;
  if (!last.bytes.empty())
    access_units.push_back(std::move(last));
  return access_units;
}

bool SealEsStream(const VideoKeys& keys, const gm::Bytes& annex_b, gm::Bytes* sealed,
                  std::size_t* frame_count, std::string* error) {
  if (!sealed) {
    if (error)
      *error = "sealed stream output is null";
    return false;
  }
  if (!CheckKeys(keys, error))
    return false;

  gm::Bytes output;
  std::size_t count = 0;
  const std::vector<EsAccessUnit> access_units = SplitH264AccessUnits(annex_b);
  for (const EsAccessUnit& unit : access_units) {
    gm::Bytes nonce;
    if (!gm::RandomBytes(kSealedFrameNonceSize, &nonce, error))
      return false;
    gm::Bytes frame;
    if (!SealEsFrame(keys, count, unit.keyframe, nonce, unit.bytes, &frame, error))
      return false;
    output.insert(output.end(), frame.begin(), frame.end());
    ++count;
  }

  *sealed = std::move(output);
  if (frame_count)
    *frame_count = count;
  return true;
}

bool OpenEsStream(const VideoKeys& keys, const gm::Bytes& sealed, gm::Bytes* annex_b,
                  std::size_t* frame_count, std::string* error) {
  if (!annex_b) {
    if (error)
      *error = "plaintext stream output is null";
    return false;
  }
  if (!CheckKeys(keys, error))
    return false;

  gm::Bytes output;
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < sealed.size()) {
    std::size_t frame_size = 0;
    if (!NextSealedFrameSize(sealed, offset, &frame_size, error))
      return false;
    gm::Bytes frame(sealed.begin() + static_cast<std::ptrdiff_t>(offset),
                    sealed.begin() + static_cast<std::ptrdiff_t>(offset + frame_size));
    gm::Bytes plaintext;
    if (!OpenEsFrame(keys, frame, &plaintext, nullptr, error))
      return false;
    output.insert(output.end(), plaintext.begin(), plaintext.end());
    offset += frame_size;
    ++count;
  }

  *annex_b = std::move(output);
  if (frame_count)
    *frame_count = count;
  return true;
}

} // namespace uvnvr::gb35114
