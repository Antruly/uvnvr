#pragma once

// MPEG-TS demuxer and the shared PS/RTP pipeline.
//
// The live preview path (MediaService raw TS) and the playback path (recorded
// TS segments) both deliver MPEG-TS. This file is the single place that turns
// those bytes into GB/T 28181 PS access units, so the two paths cannot diverge
// in timestamps, key-frame handling or PS header layout:
//
//   TS (PAT/PMT/PES) -> H.264/H.265 access units -> PsMuxer -> RTP sender
//
// `TsDemuxer` is deliberately narrow: one video elementary stream, PTS/DTS from
// the PES header and Annex B access-unit splitting delegated to ps.cpp. It
// handles TS packets split across read chunks, which is what a file reader and
// a pipe both produce.

#include "gb28181/ps.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uvnvr::gb28181 {

// One video access unit extracted from the transport stream.
struct EsFrame {
  std::vector<std::uint8_t> data;
  PsVideoCodec codec = PsVideoCodec::kH264;
  std::int64_t pts_90k = 0;
  bool has_pts = false;
  bool keyframe = false;
};

class TsDemuxer {
public:
  TsDemuxer() = default;

  // Appends `size` bytes and returns every complete access unit. Frames may be
  // empty when a chunk ends in the middle of a PES packet.
  bool Push(const std::uint8_t* data, std::size_t size, std::vector<EsFrame>* frames,
            std::string* error = nullptr);

  // Flushes the PES being assembled (end of file). `frames` may be null when
  // the caller only wants to drop the partial state.
  bool Flush(std::vector<EsFrame>* frames, std::string* error = nullptr);

  void Reset();

  bool has_video() const {
    return video_pid_ != 0 && have_codec_;
  }
  PsVideoCodec codec() const {
    return codec_;
  }
  std::uint16_t video_pid() const {
    return video_pid_;
  }
  std::uint16_t pmt_pid() const {
    return pmt_pid_;
  }

private:
  void ConsumePacket(const std::uint8_t* packet, std::vector<EsFrame>* frames);
  void HandlePsi(const std::uint8_t* payload, std::size_t size);
  void HandlePesPayload(const std::uint8_t* data, std::size_t size, bool start);
  void FlushPes(std::vector<EsFrame>* frames);
  static std::size_t PsiSectionLength(const std::uint8_t* payload, std::size_t size);
  bool ParsePat(const std::uint8_t* section, std::size_t size);
  bool ParsePmt(const std::uint8_t* section, std::size_t size);

  std::vector<std::uint8_t> pending_;
  std::vector<std::uint8_t> pes_;
  std::uint16_t pmt_pid_ = 0;
  std::uint16_t video_pid_ = 0;
  PsVideoCodec codec_ = PsVideoCodec::kH264;
  bool have_codec_ = false;
  bool have_pmt_ = false;

  // PES assembly state.
  std::int64_t pes_pts_ = 0;
  bool pes_has_pts_ = false;
  std::size_t pes_expected_ = 0; // 0 = unbounded (PES_packet_length == 0)
  bool pes_in_progress_ = false;
};

// Sends one PS pack (one access unit) to the RTP media session.
using PsFrameSender =
    std::function<bool(const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp_90k,
                       bool keyframe, std::string* error)>;

struct PsRtpPipelineConfig {
  // Playback rebases timestamps so the first sent frame starts at
  // `timestamp_offset_90k`; live keeps the source timestamps.
  bool rebase_timestamps = false;
  std::int64_t timestamp_offset_90k = 0;
  // Used for access units without a PES PTS and for subsequent units inside
  // one PES packet.
  std::int64_t default_frame_interval_90k = 3000;
  PsMuxerConfig muxer;
};

class PsRtpPipeline {
public:
  explicit PsRtpPipeline(PsFrameSender sender, PsRtpPipelineConfig config = {});

  bool PushTs(const std::uint8_t* data, std::size_t size, std::string* error = nullptr);
  bool PushFrame(const EsFrame& frame, std::string* error = nullptr);
  bool Flush(std::string* error = nullptr);
  void Reset();

  std::uint64_t frames_sent() const {
    return frames_sent_;
  }
  std::uint64_t keyframes_sent() const {
    return keyframes_sent_;
  }
  PsVideoCodec codec() const {
    return codec_;
  }

private:
  PsRtpPipelineConfig config_;
  PsFrameSender sender_;
  TsDemuxer demuxer_;
  std::unique_ptr<PsMuxer> muxer_;
  PsVideoCodec codec_ = PsVideoCodec::kH264;
  bool have_codec_ = false;
  bool have_base_ = false;
  std::int64_t base_pts_ = 0;
  std::int64_t next_synthetic_pts_ = 0;
  std::uint64_t frames_sent_ = 0;
  std::uint64_t keyframes_sent_ = 0;
  std::vector<std::uint8_t> pack_;
};

} // namespace uvnvr::gb28181
