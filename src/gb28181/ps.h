#pragma once

// MPEG-2 Program Stream (PS) encapsulation for GB/T 28181 media (P6.2c).
//
// GB/T 28181 carries H.264/H.265 elementary streams inside the program stream
// format described by GB/T 28181 Appendix C (which is MPEG-2 PS with a program
// stream map). The output of this module is byte-for-byte the payload that the
// RTP layer in rtp.h transports:
//
//   pack header (0x000001BA)
//   system header (0x000001BB)     first frame and each key frame
//   program stream map (0x000001BC) first frame and each key frame
//   PES packet(s) (0x000001E0)     one access unit, split at 64 KiB
//
// The module is transport independent and has no libuv/OpenSSL dependency, so
// the packet layout can be unit tested and reused by both the UDP and TCP
// paths. It is compiled only when UVNVR_ENABLE_GB28181 is on (default off).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uvnvr::gb28181 {

// program_stream_map stream_type values (ISO/IEC 13818-1).
inline constexpr std::uint8_t kPsStreamTypeH264 = 0x1B;
inline constexpr std::uint8_t kPsStreamTypeH265 = 0x24;

enum class PsVideoCodec {
  kH264,
  kH265,
};

const char* PsVideoCodecName(PsVideoCodec codec);
std::uint8_t PsStreamType(PsVideoCodec codec);

// One access unit (one video frame) of Annex B encoded H.264/H.265.
// `pts_90k` and `dts_90k` use the 90 kHz MPEG clock and are reduced modulo
// 2^33 by the muxer. When `has_dts` is false only a PTS is written.
struct PsFrame {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::int64_t pts_90k = 0;
  std::int64_t dts_90k = 0;
  bool has_dts = false;
  bool keyframe = false;
};

struct PsMuxerConfig {
  PsVideoCodec codec = PsVideoCodec::kH264;
  std::uint8_t video_stream_id = 0xE0;
  // Value written to the pack/system headers. 6106 is the common MPEG-2 PS
  // value (about 6.1 Mbit/s at 50 packets/s); decoders do not use it to pace.
  std::uint32_t program_mux_rate = 6106;
  bool include_system_header = true;
  bool include_psm = true;
  // Repeat the system header and PSM before every key frame. GB/T 28181
  // receivers use those bytes to join a stream mid-GOP, so this stays on.
  bool repeat_headers_on_keyframe = true;
  // Maximum elementary-stream bytes carried by one PES packet. The 65535-byte
  // PES_packet_length field also covers flags, PTS and DTS, so 65000 leaves
  // room for all header forms.
  std::size_t max_pes_payload = 65000;
};

// Appends one PS pack for a video access unit to `out`. The vector is cleared
// first, so each call produces exactly one RTP timestamp's worth of PS data.
// Frames larger than `max_pes_payload` become several PES packets inside the
// same pack; only the first PES carries PTS/DTS.
class PsMuxer {
 public:
  explicit PsMuxer(PsMuxerConfig config = {});

  PsMuxer(const PsMuxer&) = delete;
  PsMuxer& operator=(const PsMuxer&) = delete;

  void Reset();
  const PsMuxerConfig& config() const { return config_; }

  bool MuxFrame(const PsFrame& frame, std::vector<std::uint8_t>* out,
                std::string* error = nullptr);

 private:
  PsMuxerConfig config_;
  bool started_ = false;
};

// Parsed view of the first pack of a PS byte stream. It is intentionally a
// narrow parser: enough to validate the GB/T 28181 header layout in tests and
// diagnostics without duplicating a full demuxer.
struct PsPackInfo {
  std::size_t pack_header_size = 0;
  std::uint64_t scr_base = 0;
  std::uint32_t program_mux_rate = 0;

  bool has_system_header = false;
  std::size_t system_header_offset = 0;
  std::uint16_t system_header_length = 0;
  std::uint8_t system_header_stream_id = 0;

  bool has_psm = false;
  std::size_t psm_offset = 0;
  std::uint16_t psm_length = 0;
  std::uint8_t psm_stream_type = 0;
  std::uint8_t psm_elementary_stream_id = 0;
  std::uint32_t psm_crc_stored = 0;
  std::uint32_t psm_crc_computed = 0;
  bool psm_crc_valid = false;

  std::size_t pes_offset = 0;
  std::uint8_t pes_stream_id = 0;
  std::uint16_t pes_packet_length = 0;
  bool pes_data_alignment = false;
  bool has_pts = false;
  bool has_dts = false;
  std::int64_t pts = 0;
  std::int64_t dts = 0;
  std::size_t pes_payload_offset = 0;
  std::size_t pes_payload_size = 0;
};

bool ParsePsPack(const std::uint8_t* data, std::size_t size, PsPackInfo* info,
                 std::string* error = nullptr);

// CRC-32/MPEG-2 (polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no final
// xor, MSB first), used by the PSM and other PSI sections.
std::uint32_t Crc32Mpeg2(const std::uint8_t* data, std::size_t size);

// One Annex B access unit with its NAL units re-emitted as 4-byte start codes
// plus the NAL bytes. `keyframe` is true for H.264 IDR and H.265 IRAP pictures.
struct EsAccessUnit {
  std::vector<std::uint8_t> data;
  bool keyframe = false;
};

// Splits an Annex B H.264/H.265 elementary stream into access units. AUD NALs
// (when present) and the first-slice flags are honoured, so a multi-slice
// picture is not split and two consecutive pictures without an AUD are still
// separated. Returns false only for malformed input (for example an H.265 NAL
// shorter than its two-byte header).
bool SplitAnnexBAccessUnits(const std::uint8_t* data, std::size_t size,
                            PsVideoCodec codec, std::vector<EsAccessUnit>* units,
                            std::string* error = nullptr);

// Human-readable hex dump used by the diagnostics tool and the tests.
std::string HexDump(const std::uint8_t* data, std::size_t size,
                    std::size_t max_bytes = 256);

}  // namespace uvnvr::gb28181
