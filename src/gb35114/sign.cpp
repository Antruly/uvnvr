#include "gb35114/sign.h"

#include "crypto/cert.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace uvnvr::gb35114 {
namespace {

constexpr std::uint8_t kTsSyncByte = 0x47;
constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint8_t kTsStreamTypeH264 = 0x1B;
constexpr std::uint8_t kTsStreamTypeH265 = 0x24;
constexpr std::size_t kMaxPesAssembly = 16U * 1024U * 1024U;

void PutU16(gm::Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<unsigned char>(value >> 8));
  out->push_back(static_cast<unsigned char>(value));
}

void PutU32(gm::Bytes* out, std::uint32_t value) {
  out->push_back(static_cast<unsigned char>(value >> 24));
  out->push_back(static_cast<unsigned char>(value >> 16));
  out->push_back(static_cast<unsigned char>(value >> 8));
  out->push_back(static_cast<unsigned char>(value));
}

void PutU64(gm::Bytes* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out->push_back(static_cast<unsigned char>(value >> shift));
}

std::uint16_t ReadU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

std::uint64_t ReadU64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value = (value << 8) | data[i];
  return value;
}

std::string HexBytes(const gm::Bytes& bytes) {
  return gm::ToHex(bytes);
}

// ---------------------------------------------------------------------------
// H.264 Annex B access-unit splitting
// ---------------------------------------------------------------------------

bool ParseStartCode(const std::uint8_t* data, std::size_t size, std::size_t from,
                    std::size_t* position, std::size_t* prefix) {
  if (prefix)
    *prefix = 0;
  if (!position || size < 3)
    return false;
  for (std::size_t i = from; i + 2 < size; ++i) {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    if (i > 0 && data[i - 1] == 0) {
      *position = i - 1;
      if (prefix)
        *prefix = 4;
    } else {
      *position = i;
      if (prefix)
        *prefix = 3;
    }
    return true;
  }
  return false;
}

class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool ReadBit(bool* bit) {
    if (bit_ >= size_ * 8)
      return false;
    *bit = ((data_[bit_ / 8] >> (7 - (bit_ % 8))) & 1) != 0;
    ++bit_;
    return true;
  }

  bool ReadUE(std::uint32_t* value) {
    std::uint32_t leading_zeroes = 0;
    bool bit = false;
    while (true) {
      if (!ReadBit(&bit))
        return false;
      if (bit)
        break;
      ++leading_zeroes;
      if (leading_zeroes > 31)
        return false;
    }
    std::uint32_t suffix = 0;
    for (std::uint32_t i = 0; i < leading_zeroes; ++i) {
      if (!ReadBit(&bit))
        return false;
      suffix = (suffix << 1) | (bit ? 1U : 0U);
    }
    *value = ((1U << leading_zeroes) - 1U) + suffix;
    return true;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t bit_ = 0;
};

// Removes H.264 emulation-prevention bytes so Exp-Golomb fields can be read
// from the slice header. Only the first few bytes are needed.
gm::Bytes NalRbsp(const std::uint8_t* nal, std::size_t size, std::size_t header_size) {
  gm::Bytes rbsp;
  if (size <= header_size)
    return rbsp;
  rbsp.reserve(size - header_size);
  int zeroes = 0;
  for (std::size_t i = header_size; i < size; ++i) {
    const std::uint8_t byte = nal[i];
    if (zeroes >= 2 && byte == 0x03) {
      zeroes = 0;
      continue;
    }
    rbsp.push_back(byte);
    zeroes = byte == 0 ? zeroes + 1 : 0;
  }
  return rbsp;
}

// first_mb_in_slice == 0 marks the first slice of a picture. slice_type 2/4
// (all slices of the picture are I/SI) marks a non-IDR I frame; type 5 NALs
// are IDR and are always key frames.
void DescribeSlice(const std::uint8_t* nal, std::size_t size, std::size_t header_size,
                   bool* first_slice, bool* i_slice) {
  *first_slice = true;
  *i_slice = false;
  // `header_size` here is the start-code length; skip it and the one-byte
  // H.264 NAL header before reading the slice header RBSP.
  const std::size_t rbsp_offset = header_size + 1;
  if (size <= rbsp_offset)
    return;
  const gm::Bytes rbsp = NalRbsp(nal, size, rbsp_offset);
  BitReader bits(rbsp.data(), rbsp.size());
  std::uint32_t first_mb = 0;
  std::uint32_t slice_type = 0;
  if (!bits.ReadUE(&first_mb) || !bits.ReadUE(&slice_type))
    return;
  *first_slice = first_mb == 0;
  *i_slice = slice_type % 5 == 2 || slice_type % 5 == 4;
}

class H264Parser {
 public:
  bool PushEs(const std::uint8_t* data, std::size_t size, bool has_pts, std::int64_t pts,
              std::vector<AccessUnit>* out, std::string* error) {
    incoming_has_pts_ = has_pts;
    incoming_pts_ = pts;
    const std::size_t previous_size = buffer_.size();
    if (size != 0) {
      if (!data) {
        SetError(error, "null elementary-stream buffer");
        return false;
      }
      buffer_.insert(buffer_.end(), data, data + size);
    }
    return Process(false, out, error, previous_size);
  }

  bool Flush(std::vector<AccessUnit>* out, std::string* error) {
    incoming_has_pts_ = retained_has_pts_;
    incoming_pts_ = retained_pts_;
    return Process(true, out, error, buffer_.size());
  }

  void Reset() {
    buffer_.clear();
    current_.clear();
    current_has_vcl_ = false;
    current_keyframe_ = false;
    current_has_pts_ = false;
    current_pts_ = 0;
    incoming_has_pts_ = false;
    incoming_pts_ = 0;
    retained_has_pts_ = false;
    retained_pts_ = 0;
  }

 private:
  static void SetError(std::string* error, std::string message) {
    if (error)
      *error = std::move(message);
  }

  bool Process(bool at_end, std::vector<AccessUnit>* out, std::string* error,
               std::size_t previous_size) {
    if (buffer_.empty()) {
      if (at_end)
        Emit(out);
      return true;
    }

    std::size_t position = 0;
    std::size_t prefix = 0;
    if (!ParseStartCode(buffer_.data(), buffer_.size(), 0, &position, &prefix)) {
      if (at_end) {
        buffer_.clear();
        Emit(out);
        return true;
      }
      // Keep a possible start code that straddles the chunk boundary.
      if (buffer_.size() > 3)
        buffer_.erase(buffer_.begin(), buffer_.end() - 3);
      return true;
    }
    if (position > 0) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(position));
      previous_size = previous_size > position ? previous_size - position : 0;
    }

    std::size_t cursor = 0;
    std::size_t cursor_prefix = prefix;
    while (true) {
      std::size_t next = 0;
      std::size_t next_prefix = 0;
      if (!ParseStartCode(buffer_.data(), buffer_.size(), cursor + cursor_prefix, &next,
                          &next_prefix))
        break;
      if (next <= cursor)
        break;
      const bool nal_has_pts = cursor < previous_size ? retained_has_pts_ : incoming_has_pts_;
      const std::int64_t nal_pts = cursor < previous_size ? retained_pts_ : incoming_pts_;
      AppendNal(buffer_.data() + cursor, next - cursor, nal_has_pts, nal_pts, out, error);
      cursor = next;
      cursor_prefix = next_prefix;
    }

    if (at_end) {
      if (cursor < buffer_.size()) {
        const bool nal_has_pts = cursor < previous_size ? retained_has_pts_ : incoming_has_pts_;
        const std::int64_t nal_pts = cursor < previous_size ? retained_pts_ : incoming_pts_;
        AppendNal(buffer_.data() + cursor, buffer_.size() - cursor, nal_has_pts, nal_pts, out,
                  error);
      }
      Emit(out);
      buffer_.clear();
      retained_has_pts_ = false;
      retained_pts_ = 0;
    } else if (cursor > 0) {
      retained_has_pts_ = cursor < previous_size ? retained_has_pts_ : incoming_has_pts_;
      retained_pts_ = cursor < previous_size ? retained_pts_ : incoming_pts_;
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor));
    } else {
      retained_has_pts_ = cursor < previous_size ? retained_has_pts_ : incoming_has_pts_;
      retained_pts_ = cursor < previous_size ? retained_pts_ : incoming_pts_;
    }
    return true;
  }

  void Emit(std::vector<AccessUnit>* out) {
    if (!current_.empty() && current_has_vcl_) {
      AccessUnit unit;
      unit.data = std::move(current_);
      unit.key_frame = current_keyframe_;
      unit.has_pts = current_has_pts_;
      unit.pts = current_pts_;
      out->push_back(std::move(unit));
    }
    current_.clear();
    current_has_vcl_ = false;
    current_keyframe_ = false;
    current_has_pts_ = false;
    current_pts_ = 0;
  }

  void AppendNal(const std::uint8_t* nal, std::size_t size, bool nal_has_pts,
                 std::int64_t nal_pts, std::vector<AccessUnit>* out, std::string*) {
    std::size_t prefix = 0;
    std::size_t ignored = 0;
    if (!ParseStartCode(nal, size, 0, &ignored, &prefix) || size <= prefix)
      return;
    const std::uint8_t type = nal[prefix] & 0x1F;
    const bool vcl = type >= 1 && type <= 5;

    if (type == 9) {  // access unit delimiter
      if (current_has_vcl_)
        Emit(out);
    }

    if (vcl) {
      bool first_slice = true;
      bool i_slice = false;
      DescribeSlice(nal, size, prefix, &first_slice, &i_slice);
      if (first_slice && current_has_vcl_)
        Emit(out);
      if (!current_has_vcl_) {
        current_has_pts_ = nal_has_pts;
        current_pts_ = nal_pts;
      }
      current_.insert(current_.end(), nal, nal + size);
      current_has_vcl_ = true;
      if (type == 5 || i_slice)
        current_keyframe_ = true;
      return;
    }

    // Parameter sets, SEI and other non-VCL NALs that follow a picture belong
    // to the next access unit (SPS/PPS/SEI precede an IDR in practice).
    if (current_has_vcl_)
      Emit(out);
    if (current_.empty()) {
      current_has_pts_ = nal_has_pts;
      current_pts_ = nal_pts;
    }
    current_.insert(current_.end(), nal, nal + size);
  }

  gm::Bytes buffer_;
  gm::Bytes current_;
  bool current_has_vcl_ = false;
  bool current_keyframe_ = false;
  bool current_has_pts_ = false;
  std::int64_t current_pts_ = 0;
  bool incoming_has_pts_ = false;
  std::int64_t incoming_pts_ = 0;
  bool retained_has_pts_ = false;
  std::int64_t retained_pts_ = 0;
};

// ---------------------------------------------------------------------------
// MPEG-TS demuxer (PAT -> PMT -> video PES)
// ---------------------------------------------------------------------------

std::int64_t DecodePts(const std::uint8_t* data) {
  const std::uint64_t value = (static_cast<std::uint64_t>(data[0] & 0x0E) << 29) |
                              (static_cast<std::uint64_t>(data[1]) << 22) |
                              (static_cast<std::uint64_t>(data[2] & 0xFE) << 14) |
                              (static_cast<std::uint64_t>(data[3]) << 7) |
                              (static_cast<std::uint64_t>(data[4]) >> 1);
  return static_cast<std::int64_t>(value);
}

class TsDemuxer {
 public:
  bool Push(const std::uint8_t* data, std::size_t size, std::vector<AccessUnit>* out,
            std::string* error) {
    if (size != 0) {
      if (!data) {
        SetError(error, "null transport-stream buffer");
        return false;
      }
      buffer_.insert(buffer_.end(), data, data + size);
    }

    std::size_t position = 0;
    while (buffer_.size() - position >= kTsPacketSize) {
      if (buffer_[position] != kTsSyncByte) {
        const std::size_t synced = Resync(position);
        if (synced == std::string::npos) {
          if (buffer_.size() > kTsPacketSize - 1)
            buffer_.erase(buffer_.begin(),
                          buffer_.end() - static_cast<std::ptrdiff_t>(kTsPacketSize - 1));
          return true;
        }
        position = synced;
        if (buffer_.size() - position < kTsPacketSize)
          break;
      }
      if (!ParsePacket(buffer_.data() + position, out, error))
        return false;
      ++stats_packets_;
      position += kTsPacketSize;
    }
    if (position > 0)
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(position));
    return true;
  }

  bool Flush(std::vector<AccessUnit>* out, std::string* error) {
    if (!FinishPes(out, error))
      return false;
    if (!parser_.Flush(out, error))
      return false;
    buffer_.clear();
    return true;
  }

  void Reset() {
    buffer_.clear();
    pes_.clear();
    pmt_pid_ = -1;
    video_pid_ = -1;
    h265_ = false;
    parser_.Reset();
    stats_packets_ = 0;
  }

  std::uint64_t packets() const { return stats_packets_; }

 private:
  static void SetError(std::string* error, std::string message) {
    if (error)
      *error = std::move(message);
  }

  // Finds the next 188-byte aligned sync byte with a confirming sync byte in
  // the following packet. Returns std::string::npos when the buffer is too
  // short to confirm alignment.
  std::size_t Resync(std::size_t from) const {
    for (std::size_t i = from; i + kTsPacketSize <= buffer_.size(); ++i) {
      if (buffer_[i] != kTsSyncByte)
        continue;
      if (i + 2 * kTsPacketSize > buffer_.size() || buffer_[i + kTsPacketSize] == kTsSyncByte)
        return i;
    }
    return std::string::npos;
  }

  bool ParsePacket(const std::uint8_t* packet, std::vector<AccessUnit>* out, std::string* error) {
    const std::uint8_t b1 = packet[1];
    const std::uint8_t b2 = packet[2];
    const std::uint8_t b3 = packet[3];
    if ((b1 & 0x80) != 0)  // transport_error_indicator
      return true;
    const bool pusi = (b1 & 0x40) != 0;
    const int pid = ((b1 & 0x1F) << 8) | b2;
    const int adaptation = (b3 >> 4) & 0x03;
    std::size_t offset = 4;
    if ((adaptation & 0x02) != 0) {
      const std::uint8_t length = packet[4];
      offset = 5 + static_cast<std::size_t>(length);
      if (offset > kTsPacketSize)
        return true;
    }
    if ((adaptation & 0x01) == 0 || offset >= kTsPacketSize)
      return true;
    const std::uint8_t* payload = packet + offset;
    const std::size_t length = kTsPacketSize - offset;

    if (pid == 0 && pusi)
      return ParsePat(payload, length, error);
    if (pid == pmt_pid_ && pusi)
      return ParsePmt(payload, length, error);
    if (pid == video_pid_) {
      if (pusi) {
        if (!FinishPes(out, error))
          return false;
        pes_.assign(payload, payload + length);
      } else if (pes_.size() + length > kMaxPesAssembly) {
        SetError(error, "video PES exceeds the 16 MiB assembly limit");
        pes_.clear();
        return false;
      } else {
        pes_.insert(pes_.end(), payload, payload + length);
      }
    }
    return true;
  }

  const std::uint8_t* SectionStart(const std::uint8_t* payload, std::size_t length,
                                   std::size_t* start, std::size_t* end) {
    if (length == 0)
      return nullptr;
    const std::size_t pointer = payload[0];
    if (1 + pointer + 3 > length)
      return nullptr;
    const std::size_t section = 1 + pointer;
    const std::size_t section_length = ((payload[section + 1] & 0x0F) << 8) | payload[section + 2];
    const std::size_t section_end = std::min(length, section + 3 + section_length);
    if (section_end < section + 3)
      return nullptr;
    *start = section;
    *end = section_end;
    return payload;
  }

  bool ParsePat(const std::uint8_t* payload, std::size_t length, std::string*) {
    std::size_t start = 0;
    std::size_t end = 0;
    if (!SectionStart(payload, length, &start, &end))
      return true;
    if (payload[start] != 0x00 || end < start + 12)
      return true;
    // The CRC is the last four bytes of the section.
    for (std::size_t i = start + 8; i + 4 <= end - 4; i += 4) {
      const std::uint16_t program = ReadU16(payload + i);
      const std::uint16_t pid = static_cast<std::uint16_t>(((payload[i + 2] & 0x1F) << 8) |
                                                           payload[i + 3]);
      if (program != 0) {
        pmt_pid_ = pid;
        break;
      }
    }
    return true;
  }

  bool ParsePmt(const std::uint8_t* payload, std::size_t length, std::string*) {
    std::size_t start = 0;
    std::size_t end = 0;
    if (!SectionStart(payload, length, &start, &end))
      return true;
    if (payload[start] != 0x02 || end < start + 12)
      return true;
    const std::size_t program_info_length =
        ((payload[start + 10] & 0x0F) << 8) | payload[start + 11];
    std::size_t i = start + 12 + program_info_length;
    if (i > end)
      return true;
    while (i + 5 <= end - 4) {
      const std::uint8_t stream_type = payload[i];
      const std::uint16_t pid = static_cast<std::uint16_t>(((payload[i + 1] & 0x1F) << 8) |
                                                           payload[i + 2]);
      const std::size_t es_info_length =
          ((payload[i + 3] & 0x0F) << 8) | payload[i + 4];
      if (stream_type == kTsStreamTypeH264) {
        video_pid_ = pid;
        h265_ = false;
      } else if (stream_type == kTsStreamTypeH265) {
        video_pid_ = pid;
        h265_ = true;
      }
      i += 5 + es_info_length;
    }
    return true;
  }

  bool FinishPes(std::vector<AccessUnit>* out, std::string* error) {
    if (pes_.empty())
      return true;
    if (pes_.size() < 9 || pes_[0] != 0 || pes_[1] != 0 || pes_[2] != 1) {
      pes_.clear();
      return true;
    }
    if (h265_) {
      pes_.clear();
      SetError(error, "H.265 video is not supported by the GB 35114 A-level signer yet");
      return false;
    }
    const std::uint8_t stream_id = pes_[3];
    const std::size_t packet_length = (static_cast<std::size_t>(pes_[4]) << 8) | pes_[5];
    if (packet_length != 0) {
      const std::size_t total = 6 + packet_length;
      if (pes_.size() > total)
        pes_.resize(total);
    }
    if (stream_id < 0xE0 || stream_id > 0xEF) {
      pes_.clear();
      return true;
    }
    if (pes_.size() < 9) {
      pes_.clear();
      return true;
    }
    const std::uint8_t flags = pes_[7];
    const std::size_t header_length = pes_[8];
    const std::size_t payload_offset = 9 + header_length;
    if (payload_offset > pes_.size()) {
      pes_.clear();
      return true;
    }
    bool has_pts = false;
    std::int64_t pts = 0;
    if ((flags & 0x80) != 0 && pes_.size() >= 14) {
      pts = DecodePts(pes_.data() + 9);
      has_pts = true;
    }
    const bool ok = parser_.PushEs(pes_.data() + payload_offset, pes_.size() - payload_offset,
                                   has_pts, pts, out, error);
    pes_.clear();
    return ok;
  }

  gm::Bytes buffer_;
  gm::Bytes pes_;
  H264Parser parser_;
  int pmt_pid_ = -1;
  int video_pid_ = -1;
  bool h265_ = false;
  std::uint64_t stats_packets_ = 0;
};

std::uint64_t ElapsedNs(const std::chrono::steady_clock::time_point& start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                           start)
          .count());
}

}  // namespace

// ---------------------------------------------------------------------------
// Signature records
// ---------------------------------------------------------------------------

gm::Bytes SerializeFrameSignature(const FrameSignature& signature, std::string* error) {
  if (signature.digest.size() != kSignatureDigestSize) {
    if (error)
      *error = "signature record digest must be 32 bytes";
    return {};
  }
  if (signature.signature.empty()) {
    if (error)
      *error = "signature record has no SM2 signature";
    return {};
  }
  if (signature.signature.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (error)
      *error = "signature record signature is too large";
    return {};
  }
  gm::Bytes out;
  out.reserve(kSignatureHeaderSize + signature.signature.size());
  static const unsigned char kMagic[8] = {'G', 'B', '3', '5', '1', '1', '4', 'S'};
  out.insert(out.end(), kMagic, kMagic + 8);
  PutU16(&out, kSignatureVersion);
  PutU16(&out, static_cast<std::uint16_t>(kSignatureHeaderSize));
  PutU32(&out, signature.flags);
  PutU64(&out, static_cast<std::uint64_t>(signature.channel_id));
  PutU64(&out, signature.frame_index);
  PutU64(&out, static_cast<std::uint64_t>(signature.pts));
  PutU64(&out, signature.payload_size);
  out.insert(out.end(), signature.digest.begin(), signature.digest.end());
  PutU32(&out, static_cast<std::uint32_t>(signature.signature.size()));
  PutU32(&out, 0);
  out.insert(out.end(), signature.signature.begin(), signature.signature.end());
  return out;
}

bool ParseFrameSignature(const std::uint8_t* data, std::size_t size, FrameSignature* signature,
                         std::size_t* consumed, std::string* error) {
  if (consumed)
    *consumed = 0;
  if (!signature) {
    if (error)
      *error = "signature output is null";
    return false;
  }
  if (!data || size < kSignatureHeaderSize) {
    if (error)
      *error = "truncated signature record header";
    return false;
  }
  static const unsigned char kMagic[8] = {'G', 'B', '3', '5', '1', '1', '4', 'S'};
  if (std::memcmp(data, kMagic, 8) != 0) {
    if (error)
      *error = "bad signature record magic";
    return false;
  }
  const std::uint16_t version = ReadU16(data + 8);
  if (version != kSignatureVersion) {
    if (error)
      *error = "unsupported signature record version " + std::to_string(version);
    return false;
  }
  const std::uint16_t header_size = ReadU16(data + 10);
  if (header_size < kSignatureHeaderSize || header_size > size) {
    if (error)
      *error = "invalid signature record header size";
    return false;
  }
  FrameSignature result;
  result.flags = ReadU32(data + 12);
  result.channel_id = static_cast<std::int64_t>(ReadU64(data + 16));
  result.frame_index = ReadU64(data + 24);
  result.pts = static_cast<std::int64_t>(ReadU64(data + 32));
  result.payload_size = ReadU64(data + 40);
  result.digest.assign(data + 48, data + 48 + kSignatureDigestSize);
  const std::uint32_t signature_size = ReadU32(data + 80);
  if (signature_size == 0) {
    if (error)
      *error = "signature record carries an empty signature";
    return false;
  }
  if (signature_size > size - header_size) {
    if (error)
      *error = "truncated signature record signature";
    return false;
  }
  result.signature.assign(data + header_size, data + header_size + signature_size);
  *signature = std::move(result);
  if (consumed)
    *consumed = header_size + signature_size;
  return true;
}

gm::Bytes BuildSignedMessage(const FrameSignature& signature) {
  gm::Bytes message;
  message.reserve(kSignatureDomain.size() + 2 + 4 + 8 * 4 + kSignatureDigestSize);
  message.insert(message.end(), kSignatureDomain.begin(), kSignatureDomain.end());
  PutU16(&message, kSignatureVersion);
  PutU32(&message, signature.flags);
  PutU64(&message, static_cast<std::uint64_t>(signature.channel_id));
  PutU64(&message, signature.frame_index);
  PutU64(&message, static_cast<std::uint64_t>(signature.pts));
  PutU64(&message, signature.payload_size);
  message.insert(message.end(), signature.digest.begin(), signature.digest.end());
  return message;
}

bool CreateFrameSignature(std::string_view private_key_pem, std::int64_t channel_id,
                          std::uint64_t frame_index, std::int64_t pts, bool has_pts,
                          bool key_frame, const void* payload, std::size_t size,
                          FrameSignature* signature, std::string* error,
                          std::uint32_t extra_flags) {
  if (!signature) {
    if (error)
      *error = "signature output is null";
    return false;
  }
  if (private_key_pem.empty()) {
    if (error)
      *error = "device SM2 private key is empty";
    return false;
  }
  FrameSignature record;
  record.channel_id = channel_id;
  record.frame_index = frame_index;
  record.pts = has_pts ? pts : 0;
  record.flags = (key_frame ? kFlagKeyFrame : 0U) | (has_pts ? kFlagHasPts : 0U) |
                 (extra_flags & kFlagAllFrames);
  record.payload_size = size;
  record.digest = gm::Sm3(payload, size);
  if (record.digest.size() != kSignatureDigestSize) {
    if (error)
      *error = "SM3 digest of the frame failed";
    return false;
  }
  const gm::Bytes message = BuildSignedMessage(record);
  const std::string_view message_view(reinterpret_cast<const char*>(message.data()),
                                      message.size());
  if (!gm::Sm2Sign(private_key_pem, message_view, &record.signature, error))
    return false;
  *signature = std::move(record);
  return true;
}

bool VerifyFrameSignature(const VerifyOptions& options, const void* payload, std::size_t size,
                          const FrameSignature& signature, std::string* error) {
  if (signature.digest.size() != kSignatureDigestSize) {
    if (error)
      *error = "signature record digest is not 32 bytes";
    return false;
  }
  if (signature.payload_size != size) {
    if (error)
      *error = "signed payload size " + std::to_string(signature.payload_size) + " does not match " +
               std::to_string(size) + " bytes of video";
    return false;
  }
  if (signature.signature.empty()) {
    if (error)
      *error = "signature record has no SM2 signature";
    return false;
  }
  const gm::Bytes digest = gm::Sm3(payload, size);
  if (digest.size() != kSignatureDigestSize) {
    if (error)
      *error = "SM3 digest of the frame failed";
    return false;
  }
  if (digest != signature.digest) {
    if (error)
      *error = "SM3 digest mismatch: signed " + HexBytes(signature.digest) + ", recomputed " +
               HexBytes(digest);
    return false;
  }
  const gm::Bytes message = BuildSignedMessage(signature);
  const std::string_view message_view(reinterpret_cast<const char*>(message.data()),
                                      message.size());
  std::string verify_error;
  bool ok = false;
  if (!options.public_key_pem.empty()) {
    ok = gm::Sm2Verify(options.public_key_pem, message_view, signature.signature, &verify_error);
  } else if (!options.certificate_pem.empty()) {
    ok = gm::Sm2VerifyCertificate(options.certificate_pem, message_view, signature.signature,
                                  &verify_error);
  } else {
    if (error)
      *error = "no verification key or certificate supplied";
    return false;
  }
  if (!ok) {
    if (error)
      *error = verify_error.empty() ? std::string("SM2 signature verification failed")
                                   : verify_error;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Incremental access-unit reader
// ---------------------------------------------------------------------------

struct AccessUnitReader::Impl {
  explicit Impl(InputFormat requested) : format(requested) {}

  InputFormat format;
  bool decided = false;
  bool transport_stream = false;
  ReaderStats stats;
  Sink sink;
  TsDemuxer demuxer;
  H264Parser annex_b;

  bool Deliver(std::vector<AccessUnit>& units, std::string* error) {
    for (auto& unit : units) {
      ++stats.access_units;
      if (unit.key_frame)
        ++stats.key_frames;
      if (sink && !sink(std::move(unit), error))
        return false;
    }
    return true;
  }

  bool Push(const std::uint8_t* data, std::size_t size, std::string* error) {
    if (size == 0)
      return true;
    if (!data) {
      if (error)
        *error = "null input buffer";
      return false;
    }
    stats.input_bytes += size;
    if (!decided) {
      transport_stream = format == InputFormat::kMpegTs ||
                         (format == InputFormat::kAuto && data[0] == kTsSyncByte);
      decided = true;
    }
    std::vector<AccessUnit> units;
    bool ok = false;
    if (transport_stream) {
      ok = demuxer.Push(data, size, &units, error);
      stats.codec = "h264";
    } else {
      ok = annex_b.PushEs(data, size, false, 0, &units, error);
    }
    if (!ok)
      return false;
    return Deliver(units, error);
  }

  bool Flush(std::string* error) {
    std::vector<AccessUnit> units;
    const bool ok = transport_stream ? demuxer.Flush(&units, error)
                                     : annex_b.Flush(&units, error);
    if (!ok)
      return false;
    return Deliver(units, error);
  }

  void Reset() {
    decided = false;
    transport_stream = false;
    stats = ReaderStats{};
    demuxer.Reset();
    annex_b.Reset();
  }
};

AccessUnitReader::AccessUnitReader(InputFormat format)
    : impl_(std::make_unique<Impl>(format)) {}

AccessUnitReader::~AccessUnitReader() = default;

void AccessUnitReader::SetSink(Sink sink) {
  impl_->sink = std::move(sink);
}

bool AccessUnitReader::Push(const std::uint8_t* data, std::size_t size, std::string* error) {
  return impl_->Push(data, size, error);
}

bool AccessUnitReader::Flush(std::string* error) {
  return impl_->Flush(error);
}

void AccessUnitReader::Reset() {
  impl_->Reset();
}

const ReaderStats& AccessUnitReader::stats() const {
  return impl_->stats;
}

// ---------------------------------------------------------------------------
// Frame signer
// ---------------------------------------------------------------------------

struct FrameSigner::Impl {
  Impl(SigningOptions requested, InputFormat format)
      : options(std::move(requested)), reader(format) {}

  bool Configure(std::string* error) {
    if (options.private_key_pem.empty()) {
      if (error)
        *error = "device SM2 private key is empty";
      return false;
    }
    if (!options.certificate_pem.empty()) {
      gm::DeviceIdentity identity;
      if (!gm::ImportPemIdentity(options.private_key_pem, options.certificate_pem, "", &identity,
                                 error))
        return false;
      options.private_key_pem = std::move(identity.private_key_pem);
    }
    // Probe once so a malformed key fails at startup instead of mid-stream.
    static const unsigned char kEmptyPayload = 0;
    FrameSignature probe;
    if (!CreateFrameSignature(options.private_key_pem, options.channel_id, 0, 0, false, false,
                              &kEmptyPayload, 0, &probe, error))
      return false;
    configured = true;
    return true;
  }

  bool OnUnit(AccessUnit&& unit, std::string* error) {
    const std::uint64_t index = next_index++;
    if (!options.sign_all_frames && !unit.key_frame)
      return true;
    const auto start = std::chrono::steady_clock::now();
    FrameSignature record;
    if (!CreateFrameSignature(options.private_key_pem, options.channel_id, index, unit.pts,
                              unit.has_pts, unit.key_frame, unit.data.data(), unit.data.size(),
                              &record, error,
                              options.sign_all_frames ? kFlagAllFrames : 0U))
      return false;
    const std::uint64_t elapsed = ElapsedNs(start);
    ++stats.signed_frames;
    stats.sign_ns_total += elapsed;
    if (stats.signed_frames == 1 || elapsed < stats.sign_ns_min)
      stats.sign_ns_min = elapsed;
    stats.sign_ns_max = std::max(stats.sign_ns_max, elapsed);
    if (sink)
      sink(record);
    return true;
  }

  SigningOptions options;
  AccessUnitReader reader;
  SigningStats stats;
  Sink sink;
  std::uint64_t next_index = 0;
  bool configured = false;
};

FrameSigner::FrameSigner(SigningOptions options, InputFormat format)
    : impl_(std::make_unique<Impl>(std::move(options), format)) {
  impl_->reader.SetSink(
      [this](AccessUnit&& unit, std::string* error) { return impl_->OnUnit(std::move(unit), error); });
}

FrameSigner::~FrameSigner() = default;

bool FrameSigner::Configure(std::string* error) {
  return impl_->Configure(error);
}

void FrameSigner::SetSink(Sink sink) {
  impl_->sink = std::move(sink);
}

bool FrameSigner::Push(const std::uint8_t* data, std::size_t size, std::string* error) {
  if (!impl_->configured) {
    if (error)
      *error = "FrameSigner::Configure() must succeed before Push()";
    return false;
  }
  const bool ok = impl_->reader.Push(data, size, error);
  impl_->stats.input_bytes = impl_->reader.stats().input_bytes;
  impl_->stats.access_units = impl_->reader.stats().access_units;
  impl_->stats.key_frames = impl_->reader.stats().key_frames;
  impl_->stats.codec = impl_->reader.stats().codec;
  return ok;
}

bool FrameSigner::Flush(std::string* error) {
  if (!impl_->configured) {
    if (error)
      *error = "FrameSigner::Configure() must succeed before Flush()";
    return false;
  }
  const bool ok = impl_->reader.Flush(error);
  impl_->stats.input_bytes = impl_->reader.stats().input_bytes;
  impl_->stats.access_units = impl_->reader.stats().access_units;
  impl_->stats.key_frames = impl_->reader.stats().key_frames;
  impl_->stats.codec = impl_->reader.stats().codec;
  return ok;
}

void FrameSigner::Reset() {
  impl_->reader.Reset();
  impl_->stats = SigningStats{};
  impl_->next_index = 0;
}

const SigningStats& FrameSigner::stats() const {
  return impl_->stats;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool ReadAccessUnitsFromFile(const std::string& path, InputFormat format,
                             std::vector<AccessUnit>* units, ReaderStats* stats,
                             std::string* error) {
  if (!units) {
    if (error)
      *error = "access-unit output is null";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error)
      *error = "cannot open " + path;
    return false;
  }
  AccessUnitReader reader(format);
  reader.SetSink([units](AccessUnit&& unit, std::string*) {
    units->push_back(std::move(unit));
    return true;
  });
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0 && !reader.Push(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                static_cast<std::size_t>(got), error))
      return false;
  }
  if (!reader.Flush(error))
    return false;
  if (stats)
    *stats = reader.stats();
  return true;
}

bool SignFile(const std::string& path, const SigningOptions& options, InputFormat format,
              std::vector<FrameSignature>* signatures, SigningStats* stats, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error)
      *error = "cannot open " + path;
    return false;
  }
  FrameSigner signer(options, format);
  if (!signer.Configure(error))
    return false;
  std::vector<FrameSignature> records;
  signer.SetSink([&records](const FrameSignature& record) { records.push_back(record); });
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = input.gcount();
    if (got > 0 && !signer.Push(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                static_cast<std::size_t>(got), error))
      return false;
  }
  if (!signer.Flush(error))
    return false;
  if (signatures)
    *signatures = std::move(records);
  if (stats)
    *stats = signer.stats();
  return true;
}

bool ReadSignatureFile(const std::string& path, std::vector<FrameSignature>* signatures,
                       std::string* error) {
  if (!signatures) {
    if (error)
      *error = "signature output is null";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error)
      *error = "cannot open " + path;
    return false;
  }
  const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),
                                       std::istreambuf_iterator<char>());
  if (data.empty()) {
    if (error)
      *error = "signature file is empty";
    return false;
  }
  std::vector<FrameSignature> records;
  std::size_t offset = 0;
  while (offset < data.size()) {
    FrameSignature record;
    std::size_t consumed = 0;
    std::string parse_error;
    if (!ParseFrameSignature(data.data() + offset, data.size() - offset, &record, &consumed,
                             &parse_error)) {
      if (error)
        *error = "signature record at byte " + std::to_string(offset) + ": " + parse_error;
      return false;
    }
    if (consumed == 0) {
      if (error)
        *error = "signature record at byte " + std::to_string(offset) + " has zero length";
      return false;
    }
    records.push_back(std::move(record));
    offset += consumed;
  }
  *signatures = std::move(records);
  return true;
}

bool WriteSignatureFile(const std::string& path, const std::vector<FrameSignature>& signatures,
                        std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error)
      *error = "cannot create " + path;
    return false;
  }
  for (const auto& record : signatures) {
    const gm::Bytes bytes = SerializeFrameSignature(record, error);
    if (bytes.empty())
      return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      if (error)
        *error = "cannot write " + path;
      return false;
    }
  }
  return true;
}

bool ReadPemFile(const std::string& path, std::string* pem, std::string* error) {
  if (!pem) {
    if (error)
      *error = "PEM output is null";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error)
      *error = "cannot open " + path;
    return false;
  }
  std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (data.empty()) {
    if (error)
      *error = path + " is empty";
    return false;
  }
  *pem = std::move(data);
  return true;
}

}  // namespace uvnvr::gb35114
