// GB/T 28181 Appendix C program stream encapsulation.
//
// The layout is MPEG-2 PS: a 14-byte pack header, an optional system header
// and PSM at stream start/key frames, then one or more PES packets. PTS/DTS
// are 90 kHz and encoded in the five-byte MPEG-2 form. H.264 uses stream_type
// 0x1B, H.265 uses 0x24 (ISO/IEC 13818-1). The PSM carries a CRC-32/MPEG-2 as
// required by the standard; `ParsePsPack` verifies it.

#include "gb28181/ps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace uvnvr::gb28181 {
namespace {

constexpr std::uint8_t kStartCodePrefix[4] = {0x00, 0x00, 0x01, 0x00};
constexpr std::uint64_t kPtsMask = (std::uint64_t{1} << 33) - 1;

void AppendBe16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value >> 8));
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendBe32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>(value >> 24));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendStartCode(std::vector<std::uint8_t>* out, std::uint8_t last) {
  out->push_back(kStartCodePrefix[0]);
  out->push_back(kStartCodePrefix[1]);
  out->push_back(kStartCodePrefix[2]);
  out->push_back(last);
}

void AppendBytes(std::vector<std::uint8_t>* out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

std::uint16_t ReadBe16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t ReadBe32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

// MSB-first bit writer for the fixed-width pack/system/PTS fields.
class BitWriter {
 public:
  void Put(std::uint32_t value, int bits) {
    for (int bit = bits - 1; bit >= 0; --bit) {
      current_ = static_cast<std::uint8_t>((current_ << 1) | ((value >> bit) & 1u));
      if (++used_ == 8) {
        bytes_.push_back(current_);
        current_ = 0;
        used_ = 0;
      }
    }
  }

  void PutBytes(const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      Put(data[i], 8);
  }

  bool Aligned() const { return used_ == 0; }

  std::vector<std::uint8_t> Finish() {
    if (used_ != 0) {
      current_ = static_cast<std::uint8_t>(current_ << (8 - used_));
      bytes_.push_back(current_);
      current_ = 0;
      used_ = 0;
    }
    return std::move(bytes_);
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint8_t current_ = 0;
  int used_ = 0;
};

// Pack header: 4-byte start code plus 80 bits, i.e. 14 bytes in total.
void AppendPackHeader(std::vector<std::uint8_t>* out, std::uint64_t scr_base,
                      std::uint32_t mux_rate) {
  AppendStartCode(out, 0xBA);
  BitWriter bits;
  bits.Put(0b01, 2);                                       // MPEG-2 marker
  bits.Put(static_cast<std::uint32_t>((scr_base >> 30) & 0x7), 3);
  bits.Put(1, 1);
  bits.Put(static_cast<std::uint32_t>((scr_base >> 15) & 0x7FFF), 15);
  bits.Put(1, 1);
  bits.Put(static_cast<std::uint32_t>(scr_base & 0x7FFF), 15);
  bits.Put(1, 1);
  bits.Put(0, 9);                                          // SCR extension
  bits.Put(1, 1);
  bits.Put(mux_rate & 0x3FFFFF, 22);
  bits.Put(1, 1);
  bits.Put(1, 1);
  bits.Put(0x1F, 5);                                       // reserved
  bits.Put(0, 3);                                          // stuffing length
  std::vector<std::uint8_t> body = bits.Finish();
  AppendBytes(out, body.data(), body.size());
}

// System header with a single video stream entry. The 16-bit length covers
// the six fixed bytes and the three-byte stream entry.
void AppendSystemHeader(std::vector<std::uint8_t>* out, std::uint8_t stream_id,
                        std::uint32_t mux_rate) {
  AppendStartCode(out, 0xBB);
  AppendBe16(out, 9);
  BitWriter bits;
  bits.Put(1, 1);                                          // marker
  bits.Put(mux_rate & 0x3FFFFF, 22);                       // rate_bound
  bits.Put(1, 1);                                          // marker
  bits.Put(0, 6);                                          // audio_bound
  bits.Put(0, 1);                                          // fixed_flag
  bits.Put(0, 1);                                          // CSPS_flag
  bits.Put(1, 1);                                          // audio_lock_flag
  bits.Put(1, 1);                                          // video_lock_flag
  bits.Put(1, 1);                                          // marker
  bits.Put(1, 5);                                          // video_bound
  bits.Put(0, 1);                                          // packet_rate_restriction
  bits.Put(0x7F, 7);                                       // reserved
  std::vector<std::uint8_t> body = bits.Finish();
  AppendBytes(out, body.data(), body.size());
  out->push_back(stream_id);
  out->push_back(0xE0);                                    // '11' + scale=1 + bound hi
  out->push_back(0x00);                                    // bound low
}

// Program stream map with one elementary stream. The 16-bit length includes
// the four-byte CRC that follows the entries.
void AppendPsm(std::vector<std::uint8_t>* out, PsVideoCodec codec,
               std::uint8_t stream_id) {
  std::vector<std::uint8_t> body;
  body.push_back(0xE0);                                    // current_next=1, version=0
  body.push_back(0xFF);                                    // reserved + marker
  AppendBe16(&body, 0);                                    // program_stream_info_length
  AppendBe16(&body, 4);                                    // one four-byte entry
  body.push_back(PsStreamType(codec));
  body.push_back(stream_id);
  AppendBe16(&body, 0);                                    // elementary_stream_info_length

  const std::uint32_t crc = Crc32Mpeg2(body.data(), body.size());
  AppendStartCode(out, 0xBC);
  AppendBe16(out, static_cast<std::uint16_t>(body.size() + 4));
  AppendBytes(out, body.data(), body.size());
  AppendBe32(out, crc);
}

void AppendPtsDts(BitWriter* bits, bool with_dts, std::uint64_t pts,
                  std::uint64_t dts) {
  // PTS only uses prefix 0010; with DTS, PTS uses 0011 and DTS 0001.
  bits->Put(with_dts ? 0b0011 : 0b0010, 4);
  bits->Put(static_cast<std::uint32_t>((pts >> 30) & 0x7), 3);
  bits->Put(1, 1);
  bits->Put(static_cast<std::uint32_t>((pts >> 15) & 0x7FFF), 15);
  bits->Put(1, 1);
  bits->Put(static_cast<std::uint32_t>(pts & 0x7FFF), 15);
  bits->Put(1, 1);
  if (!with_dts)
    return;
  bits->Put(0b0001, 4);
  bits->Put(static_cast<std::uint32_t>((dts >> 30) & 0x7), 3);
  bits->Put(1, 1);
  bits->Put(static_cast<std::uint32_t>((dts >> 15) & 0x7FFF), 15);
  bits->Put(1, 1);
  bits->Put(static_cast<std::uint32_t>(dts & 0x7FFF), 15);
  bits->Put(1, 1);
}

void AppendPes(std::vector<std::uint8_t>* out, std::uint8_t stream_id,
               const std::uint8_t* payload, std::size_t payload_size,
               bool with_timestamps, bool with_dts, std::uint64_t pts,
               std::uint64_t dts, bool data_alignment) {
  std::vector<std::uint8_t> header;
  if (with_timestamps) {
    BitWriter bits;
    AppendPtsDts(&bits, with_dts, pts, dts);
    header = bits.Finish();
  }

  const std::size_t header_size = 3 + header.size();
  const std::size_t packet_length = header_size + payload_size;
  AppendStartCode(out, stream_id);
  AppendBe16(out, static_cast<std::uint16_t>(packet_length));

  // '10' marker, no scrambling/priority, data_alignment_indicator on the PES
  // that begins an access unit.
  out->push_back(static_cast<std::uint8_t>(0x80 | (data_alignment ? 0x04 : 0x00)));
  out->push_back(with_timestamps ? (with_dts ? 0xC0 : 0x80) : 0x00);
  out->push_back(static_cast<std::uint8_t>(header.size()));
  AppendBytes(out, header.data(), header.size());
  AppendBytes(out, payload, payload_size);
}

bool ReadAnnexBStartCode(const std::uint8_t* data, std::size_t size, std::size_t offset,
                         std::size_t* start_code_offset, std::size_t* start_code_size) {
  for (std::size_t i = offset; i + 3 <= size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      if (i > offset && data[i - 1] == 0) {
        *start_code_offset = i - 1;
        *start_code_size = 4;
      } else {
        *start_code_offset = i;
        *start_code_size = 3;
      }
      return true;
    }
  }
  return false;
}

// Removes 0x03 emulation-prevention bytes and returns only the RBSP bytes.
std::vector<std::uint8_t> RemoveEmulationPrevention(const std::uint8_t* data,
                                                     std::size_t size) {
  std::vector<std::uint8_t> rbsp;
  rbsp.reserve(size);
  int zeros = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = data[i];
    if (zeros >= 2 && byte == 0x03) {
      zeros = 0;
      continue;
    }
    rbsp.push_back(byte);
    zeros = (byte == 0) ? zeros + 1 : 0;
  }
  return rbsp;
}

// Reads an unsigned Exp-Golomb code from the first bytes of the RBSP.
bool ReadUnsignedExpGolomb(const std::vector<std::uint8_t>& bytes, std::uint32_t* value) {
  std::size_t bit = 0;
  int leading_zero_bits = 0;
  while (bit < bytes.size() * 8) {
    const bool set = (bytes[bit / 8] & (0x80u >> (bit % 8))) != 0;
    ++bit;
    if (set)
      break;
    ++leading_zero_bits;
    if (leading_zero_bits > 31)
      return false;
  }
  std::uint32_t suffix = 0;
  for (int i = 0; i < leading_zero_bits; ++i) {
    if (bit >= bytes.size() * 8)
      return false;
    if (bytes[bit / 8] & (0x80u >> (bit % 8)))
      suffix |= (1u << (leading_zero_bits - 1 - i));
    ++bit;
  }
  *value = ((1u << leading_zero_bits) - 1u) + suffix;
  return true;
}

bool H264StartsNewPicture(const std::uint8_t* nal, std::size_t size) {
  if (size <= 1)
    return true;
  const std::vector<std::uint8_t> rbsp =
      RemoveEmulationPrevention(nal + 1, size - 1);
  std::uint32_t first_mb = 0;
  if (!ReadUnsignedExpGolomb(rbsp, &first_mb))
    return true;
  return first_mb == 0;
}

bool H265StartsNewPicture(const std::uint8_t* nal, std::size_t size) {
  if (size <= 2)
    return true;
  return (nal[2] & 0x80) != 0;  // first_slice_segment_in_pic_flag
}

}  // namespace

const char* PsVideoCodecName(PsVideoCodec codec) {
  return codec == PsVideoCodec::kH265 ? "H.265" : "H.264";
}

std::uint8_t PsStreamType(PsVideoCodec codec) {
  return codec == PsVideoCodec::kH265 ? kPsStreamTypeH265 : kPsStreamTypeH264;
}

PsMuxer::PsMuxer(PsMuxerConfig config) : config_(config) {
  if (config_.max_pes_payload == 0 || config_.max_pes_payload > 65000)
    config_.max_pes_payload = 65000;
}

void PsMuxer::Reset() { started_ = false; }

bool PsMuxer::MuxFrame(const PsFrame& frame, std::vector<std::uint8_t>* out,
                       std::string* error) {
  if (out == nullptr) {
    if (error)
      *error = "PS output buffer is null";
    return false;
  }
  out->clear();
  if (frame.data == nullptr || frame.size == 0) {
    if (error)
      *error = "empty video frame";
    return false;
  }
  if (frame.pts_90k < 0 || (frame.has_dts && frame.dts_90k < 0)) {
    if (error)
      *error = "negative PTS/DTS is not supported";
    return false;
  }

  const std::uint64_t pts = static_cast<std::uint64_t>(frame.pts_90k) & kPtsMask;
  const std::uint64_t dts =
      (frame.has_dts ? static_cast<std::uint64_t>(frame.dts_90k) : frame.pts_90k) & kPtsMask;
  const std::uint64_t scr = (frame.has_dts ? dts : pts);

  const bool write_headers =
      !started_ || (frame.keyframe && config_.repeat_headers_on_keyframe);
  AppendPackHeader(out, scr, config_.program_mux_rate);
  if (write_headers) {
    if (config_.include_system_header)
      AppendSystemHeader(out, config_.video_stream_id, config_.program_mux_rate);
    if (config_.include_psm)
      AppendPsm(out, config_.codec, config_.video_stream_id);
  }
  started_ = true;

  std::size_t offset = 0;
  bool first = true;
  while (offset < frame.size) {
    const std::size_t chunk = std::min(config_.max_pes_payload, frame.size - offset);
    const bool with_timestamps = first;
    const bool with_dts = first && frame.has_dts;
    AppendPes(out, config_.video_stream_id, frame.data + offset, chunk, with_timestamps,
              with_dts, pts, dts, first);
    offset += chunk;
    first = false;
  }
  return true;
}

std::uint32_t Crc32Mpeg2(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<std::uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
  }
  return crc;
}

bool ParsePsPack(const std::uint8_t* data, std::size_t size, PsPackInfo* info,
                 std::string* error) {
  if (info == nullptr) {
    if (error)
      *error = "PS info is null";
    return false;
  }
  *info = PsPackInfo{};
  if (data == nullptr || size < 14) {
    if (error)
      *error = "PS pack is shorter than the 14-byte pack header";
    return false;
  }
  if (data[0] != 0 || data[1] != 0 || data[2] != 1 || data[3] != 0xBA) {
    if (error)
      *error = "missing pack header start code 0x000001BA";
    return false;
  }

  const std::uint8_t b4 = data[4];
  if ((b4 & 0xC0) != 0x40) {
    if (error)
      *error = "pack header is not MPEG-2 ('01' marker bits missing)";
    return false;
  }
  // SCR_base[32:30] | [29:28] | [27:20] | [19:15] | [14:13] | [12:5] | [4:0]
  info->scr_base = (static_cast<std::uint64_t>((b4 >> 3) & 0x7) << 30) |
                   (static_cast<std::uint64_t>((b4 & 0x3)) << 28) |
                   (static_cast<std::uint64_t>(data[5]) << 20) |
                   (static_cast<std::uint64_t>((data[6] >> 3) & 0x1F) << 15) |
                   (static_cast<std::uint64_t>((data[6] & 0x3)) << 13) |
                   (static_cast<std::uint64_t>(data[7]) << 5) |
                   static_cast<std::uint64_t>((data[8] >> 3) & 0x1F);
  // The exact 33-bit SCR reconstruction is above; the 22-bit mux rate follows
  // the 9-bit extension and marker.
  info->program_mux_rate = (static_cast<std::uint32_t>(data[10]) << 14) |
                           (static_cast<std::uint32_t>(data[11]) << 6) |
                           static_cast<std::uint32_t>(data[12] >> 2);
  const std::uint8_t stuffing = data[13] & 0x7;
  info->pack_header_size = 14 + stuffing;
  if (info->pack_header_size > size) {
    if (error)
      *error = "pack stuffing runs past the end of the buffer";
    return false;
  }

  std::size_t offset = info->pack_header_size;
  while (offset + 4 <= size) {
    if (data[offset] != 0 || data[offset + 1] != 0 || data[offset + 2] != 1)
      break;
    const std::uint8_t last = data[offset + 3];

    if (last == 0xBB) {  // system header
      if (offset + 6 > size) {
        if (error)
          *error = "truncated system header";
        return false;
      }
      const std::uint16_t length = ReadBe16(data + offset + 4);
      if (offset + 6 + length > size) {
        if (error)
          *error = "system header length runs past the end of the buffer";
        return false;
      }
      info->has_system_header = true;
      info->system_header_offset = offset;
      info->system_header_length = length;
      if (length >= 9)
        info->system_header_stream_id = data[offset + 6 + 6];
      offset += 6 + length;
      continue;
    }

    if (last == 0xBC) {  // program stream map
      if (offset + 6 > size) {
        if (error)
          *error = "truncated PSM";
        return false;
      }
      const std::uint16_t length = ReadBe16(data + offset + 4);
      if (length < 10 || offset + 6 + length > size) {
        if (error)
          *error = "PSM length is invalid";
        return false;
      }
      const std::uint8_t* body = data + offset + 6;
      const std::size_t body_size = static_cast<std::size_t>(length) - 4;
      std::size_t entry = 6;
      if (body_size < 6) {
        if (error)
          *error = "PSM body is too short";
        return false;
      }
      const std::uint16_t program_info_length = ReadBe16(body + 2);
      const std::uint16_t elementary_map_length = ReadBe16(body + 4);
      entry += program_info_length;
      const std::size_t elementary_end = entry + elementary_map_length;
      if (elementary_end > body_size) {
        if (error)
          *error = "PSM elementary stream map runs past the section";
        return false;
      }
      if (entry + 4 <= elementary_end) {
        info->psm_stream_type = body[entry];
        info->psm_elementary_stream_id = body[entry + 1];
        const std::uint16_t entry_info_length = ReadBe16(body + entry + 2);
        if (entry + 4 + entry_info_length > elementary_end) {
          if (error)
            *error = "PSM elementary stream info runs past the section";
          return false;
        }
      }
      info->has_psm = true;
      info->psm_offset = offset;
      info->psm_length = length;
      info->psm_crc_stored = ReadBe32(body + body_size);
      info->psm_crc_computed = Crc32Mpeg2(body, body_size);
      info->psm_crc_valid = info->psm_crc_stored == info->psm_crc_computed;
      offset += 6 + length;
      continue;
    }

    if (last >= 0xE0 && last <= 0xEF) {  // video PES
      if (offset + 6 > size) {
        if (error)
          *error = "truncated PES header";
        return false;
      }
      const std::uint16_t packet_length = ReadBe16(data + offset + 4);
      std::size_t end = size;
      if (packet_length != 0) {
        if (offset + 6 + packet_length > size) {
          if (error)
            *error = "PES packet length runs past the end of the buffer";
          return false;
        }
        end = offset + 6 + packet_length;
      }
      if (offset + 9 > end) {
        if (error)
          *error = "PES optional header is missing";
        return false;
      }
      const std::uint8_t flags1 = data[offset + 6];
      const std::uint8_t flags2 = data[offset + 7];
      const std::uint8_t header_data_length = data[offset + 8];
      if (offset + 9 + header_data_length > end) {
        if (error)
          *error = "PES optional header length is invalid";
        return false;
      }
      info->pes_offset = offset;
      info->pes_stream_id = last;
      info->pes_packet_length = packet_length;
      info->pes_data_alignment = (flags1 & 0x04) != 0;
      const std::uint8_t timestamp_flags = (flags2 >> 6) & 0x3;
      std::size_t payload_offset = offset + 9 + header_data_length;
      if (timestamp_flags == 0x2) {  // PTS only
        if (header_data_length < 5) {
          if (error)
            *error = "PES announces a PTS but the header is too short";
          return false;
        }
        const std::uint8_t* p = data + offset + 9;
        info->has_pts = true;
        info->pts = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>((p[0] >> 1) & 0x7) << 30) |
            (static_cast<std::uint64_t>(p[1]) << 22) |
            (static_cast<std::uint64_t>((p[2] >> 1) & 0x7F) << 15) |
            (static_cast<std::uint64_t>(p[3]) << 7) |
            static_cast<std::uint64_t>((p[4] >> 1) & 0x7F));
      } else if (timestamp_flags == 0x3) {  // PTS and DTS
        if (header_data_length < 10) {
          if (error)
            *error = "PES announces PTS/DTS but the header is too short";
          return false;
        }
        const std::uint8_t* p = data + offset + 9;
        info->has_pts = true;
        info->has_dts = true;
        info->pts = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>((p[0] >> 1) & 0x7) << 30) |
            (static_cast<std::uint64_t>(p[1]) << 22) |
            (static_cast<std::uint64_t>((p[2] >> 1) & 0x7F) << 15) |
            (static_cast<std::uint64_t>(p[3]) << 7) |
            static_cast<std::uint64_t>((p[4] >> 1) & 0x7F));
        info->dts = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>((p[5] >> 1) & 0x7) << 30) |
            (static_cast<std::uint64_t>(p[6]) << 22) |
            (static_cast<std::uint64_t>((p[7] >> 1) & 0x7F) << 15) |
            (static_cast<std::uint64_t>(p[8]) << 7) |
            static_cast<std::uint64_t>((p[9] >> 1) & 0x7F));
      }
      info->pes_payload_offset = payload_offset;
      info->pes_payload_size = end - payload_offset;
      return true;
    }
    break;
  }

  if (error)
    *error = "no elementary stream PES packet found in the PS pack";
  return false;
}

bool SplitAnnexBAccessUnits(const std::uint8_t* data, std::size_t size,
                            PsVideoCodec codec, std::vector<EsAccessUnit>* units,
                            std::string* error) {
  if (units == nullptr) {
    if (error)
      *error = "access unit output is null";
    return false;
  }
  units->clear();
  if (data == nullptr || size == 0)
    return true;

  struct NalUnit {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
  };
  std::vector<NalUnit> nals;
  std::size_t search = 0;
  while (search + 3 <= size) {
    std::size_t start_code = 0;
    std::size_t start_code_size = 0;
    if (!ReadAnnexBStartCode(data, size, search, &start_code, &start_code_size))
      break;
    const std::size_t nal_start = start_code + start_code_size;
    std::size_t next_start_code = size;
    std::size_t next_start_code_size = 0;
    if (nal_start < size &&
        ReadAnnexBStartCode(data, size, nal_start, &next_start_code,
                            &next_start_code_size)) {
      // The search above can find the same 4-byte start code as a 3-byte one
      // when the leading zero is at `nal_start - 1`; the helper already
      // reports the 4-byte form, so `next_start_code` is the real boundary.
      (void)next_start_code_size;
    }
    if (next_start_code > nal_start)
      nals.push_back({data + nal_start, next_start_code - nal_start});
    search = next_start_code;
    if (next_start_code == size)
      break;
  }
  if (nals.empty()) {
    if (error)
      *error = "input has no Annex B start code";
    return false;
  }

  std::vector<std::uint8_t> current;
  bool current_has_vcl = false;
  bool current_keyframe = false;

  auto append_nal = [&current](const NalUnit& nal) {
    current.push_back(0x00);
    current.push_back(0x00);
    current.push_back(0x00);
    current.push_back(0x01);
    current.insert(current.end(), nal.data, nal.data + nal.size);
  };

  auto flush = [&]() {
    if (current.empty())
      return;
    EsAccessUnit unit;
    unit.data = std::move(current);
    unit.keyframe = current_keyframe;
    units->push_back(std::move(unit));
    current.clear();
    current_has_vcl = false;
    current_keyframe = false;
  };

  for (const NalUnit& nal : nals) {
    std::uint8_t type = 0;
    bool is_vcl = false;
    bool is_aud = false;
    bool starts_new_picture = true;
    if (codec == PsVideoCodec::kH264) {
      if (nal.size < 1) {
        if (error)
          *error = "empty H.264 NAL unit";
        return false;
      }
      type = nal.data[0] & 0x1F;
      is_vcl = type >= 1 && type <= 5;
      is_aud = type == 9;
      starts_new_picture = H264StartsNewPicture(nal.data, nal.size);
    } else {
      if (nal.size < 2) {
        if (error)
          *error = "H.265 NAL unit is shorter than its two-byte header";
        return false;
      }
      type = static_cast<std::uint8_t>((nal.data[0] >> 1) & 0x3F);
      is_vcl = type <= 31;
      is_aud = type == 35;
      starts_new_picture = H265StartsNewPicture(nal.data, nal.size);
    }

    if (is_aud) {
      flush();
      append_nal(nal);
      continue;
    }

    if (is_vcl) {
      if (current_has_vcl && starts_new_picture)
        flush();
      append_nal(nal);
      current_has_vcl = true;
      if (codec == PsVideoCodec::kH264) {
        if (type == 5)
          current_keyframe = true;
      } else if (type >= 16 && type <= 23) {  // IRAP pictures, including CRA
        current_keyframe = true;
      }
      continue;
    }

    // Parameter sets, SEI and other non-VCL NALs that follow a picture belong
    // to the next access unit.
    if (current_has_vcl)
      flush();
    append_nal(nal);
  }
  flush();
  return true;
}

std::string HexDump(const std::uint8_t* data, std::size_t size,
                    std::size_t max_bytes) {
  if (data == nullptr)
    return {};
  const std::size_t limit = max_bytes == 0 ? size : std::min(size, max_bytes);
  std::string out;
  char line[128];
  for (std::size_t offset = 0; offset < limit; offset += 16) {
    const std::size_t count = std::min<std::size_t>(16, limit - offset);
    int written = std::snprintf(line, sizeof(line), "%04zx  ", offset);
    if (written < 0)
      break;
    std::string text(line);
    for (std::size_t i = 0; i < count; ++i) {
      std::snprintf(line, sizeof(line), "%02x ", data[offset + i]);
      text += line;
    }
    for (std::size_t i = count; i < 16; ++i)
      text += "   ";
    text += " |";
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t byte = data[offset + i];
      text.push_back(byte >= 0x20 && byte <= 0x7E ? static_cast<char>(byte) : '.');
    }
    text += "|\n";
    out += text;
  }
  if (limit < size) {
    std::snprintf(line, sizeof(line), "... (%zu bytes total)\n", size);
    out += line;
  }
  return out;
}

}  // namespace uvnvr::gb28181
