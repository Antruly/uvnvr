#include "gb28181/ts_ps.h"

#include <algorithm>
#include <cstring>

namespace uvnvr::gb28181 {
namespace {

constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint8_t kSyncByte = 0x47;

std::int64_t ReadTimestamp(const std::uint8_t* data) {
  // 33-bit PTS/DTS in the five-byte PES header form.
  return (static_cast<std::int64_t>((data[0] >> 1) & 0x07) << 30) |
         (static_cast<std::int64_t>(data[1]) << 22) |
         (static_cast<std::int64_t>((data[2] >> 1) & 0x7f) << 15) |
         (static_cast<std::int64_t>(data[3]) << 7) |
         static_cast<std::int64_t>((data[4] >> 1) & 0x7f);
}

} // namespace

void TsDemuxer::Reset() {
  pending_.clear();
  pes_.clear();
  pmt_pid_ = 0;
  video_pid_ = 0;
  codec_ = PsVideoCodec::kH264;
  have_codec_ = false;
  have_pmt_ = false;
  pes_pts_ = 0;
  pes_has_pts_ = false;
  pes_expected_ = 0;
  pes_in_progress_ = false;
}

bool TsDemuxer::Push(const std::uint8_t* data, std::size_t size, std::vector<EsFrame>* frames,
                     std::string* error) {
  if (frames == nullptr) {
    if (error != nullptr)
      *error = "frame output is null";
    return false;
  }
  if (data != nullptr && size > 0)
    pending_.insert(pending_.end(), data, data + size);

  std::size_t offset = 0;
  while (pending_.size() - offset >= kTsPacketSize) {
    const std::uint8_t* packet = pending_.data() + offset;
    if (packet[0] != kSyncByte) {
      // Resynchronise on the next sync byte instead of dropping the rest of
      // the buffer. A file that starts mid-packet (or a corrupt packet) then
      // recovers without losing the following frames.
      const std::uint8_t* cursor = packet + 1;
      const std::uint8_t* end = pending_.data() + pending_.size();
      while (cursor < end && *cursor != kSyncByte)
        ++cursor;
      if (cursor == end) {
        pending_.clear();
        return true;
      }
      offset = static_cast<std::size_t>(cursor - pending_.data());
      continue;
    }
    ConsumePacket(packet, frames);
    offset += kTsPacketSize;
    if (pes_expected_ != 0 && pes_.size() >= pes_expected_) {
      FlushPes(frames);
    }
  }
  if (offset > 0)
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
  return true;
}

bool TsDemuxer::Flush(std::vector<EsFrame>* frames, std::string* error) {
  (void)error;
  if (frames != nullptr)
    FlushPes(frames);
  else
    pes_.clear();
  pes_in_progress_ = false;
  pes_expected_ = 0;
  return true;
}

void TsDemuxer::ConsumePacket(const std::uint8_t* packet, std::vector<EsFrame>* frames) {
  const bool transport_error = (packet[1] & 0x80) != 0;
  if (transport_error)
    return;
  const bool payload_unit_start = (packet[1] & 0x40) != 0;
  const std::uint16_t pid = static_cast<std::uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
  const std::uint8_t adaptation_control = (packet[3] >> 4) & 0x03;
  const bool has_payload = (adaptation_control & 0x01) != 0;
  std::size_t payload_offset = 4;
  if ((adaptation_control & 0x02) != 0) {
    const std::size_t adaptation_length = packet[4];
    payload_offset += 1 + adaptation_length;
    if (payload_offset >= kTsPacketSize)
      return;
  }
  if (!has_payload)
    return;
  const std::uint8_t* payload = packet + payload_offset;
  const std::size_t payload_size = kTsPacketSize - payload_offset;

  if (pid == 0) {
    HandlePsi(payload, payload_size);
    return;
  }
  if (pmt_pid_ != 0 && pid == pmt_pid_) {
    HandlePsi(payload, payload_size);
    return;
  }
  if (video_pid_ == 0 || pid != video_pid_)
    return;

  if (payload_unit_start) {
    // A video PES with PES_packet_length == 0 is only terminated by the next
    // payload-unit start. Emit it before starting the new one, otherwise the
    // last frame of every PES would be dropped.
    FlushPes(frames);
    // The payload begins with a pointer_field for PSI, but a PES starts with
    // its start code directly.
    if (payload_size >= 3 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1) {
      pes_.clear();
      pes_in_progress_ = true;
      pes_expected_ = 0;
      pes_pts_ = 0;
      pes_has_pts_ = false;
      pes_.insert(pes_.end(), payload, payload + payload_size);
      if (pes_.size() >= 6) {
        const std::size_t length = (static_cast<std::size_t>(pes_[4]) << 8) | pes_[5];
        if (length != 0)
          pes_expected_ = 6 + length;
      }
    }
    return;
  }
  if (!pes_in_progress_)
    return;
  pes_.insert(pes_.end(), payload, payload + payload_size);
}

void TsDemuxer::HandlePsi(const std::uint8_t* payload, std::size_t size) {
  if (size < 1)
    return;
  const std::size_t pointer = payload[0];
  if (pointer + 1 >= size)
    return;
  const std::uint8_t* section = payload + 1 + pointer;
  const std::size_t section_size = size - (1 + pointer);
  const std::size_t section_length = PsiSectionLength(section, section_size);
  if (section_length == 0 || section_length > section_size)
    return; // A PSI section split across packets is rare for PAT/PMT; retry next table.
  if (section[0] == 0x00)
    ParsePat(section, section_length);
  else if (section[0] == 0x02)
    ParsePmt(section, section_length);
}

std::size_t TsDemuxer::PsiSectionLength(const std::uint8_t* payload, std::size_t size) {
  if (size < 3)
    return 0;
  const std::size_t length = ((static_cast<std::size_t>(payload[1]) & 0x0f) << 8) | payload[2];
  return 3 + length; // table_id + length field + payload + CRC
}

bool TsDemuxer::ParsePat(const std::uint8_t* section, std::size_t size) {
  if (size < 12)
    return false;
  // table_id(1) + length(2) + tsid(2) + version(1) + section(1) + last(1)
  std::size_t offset = 8;
  const std::size_t end = size - 4; // CRC-32
  while (offset + 4 <= end) {
    const std::uint16_t program = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(section[offset]) << 8) | section[offset + 1]);
    const std::uint16_t pid =
        static_cast<std::uint16_t>(((section[offset + 2] & 0x1f) << 8) | section[offset + 3]);
    offset += 4;
    if (program != 0 && pid != 0 && pid != 0x1fff) {
      pmt_pid_ = pid;
      return true;
    }
  }
  return false;
}

bool TsDemuxer::ParsePmt(const std::uint8_t* section, std::size_t size) {
  if (size < 16)
    return false;
  // table_id(1) + length(2) + program(2) + version(1) + section(1) + last(1)
  // + PCR_PID(2) + program_info_length(2)
  std::size_t offset = 10;
  const std::size_t program_info_length =
      ((static_cast<std::size_t>(section[offset]) & 0x0f) << 8) | section[offset + 1];
  offset += 2 + program_info_length;
  const std::size_t end = size - 4;
  while (offset + 5 <= end) {
    const std::uint8_t stream_type = section[offset];
    const std::uint16_t pid =
        static_cast<std::uint16_t>(((section[offset + 1] & 0x1f) << 8) | section[offset + 2]);
    const std::size_t es_info_length =
        ((static_cast<std::size_t>(section[offset + 3]) & 0x0f) << 8) | section[offset + 4];
    offset += 5 + es_info_length;
    if (stream_type == kPsStreamTypeH264) {
      video_pid_ = pid;
      codec_ = PsVideoCodec::kH264;
      have_codec_ = true;
      have_pmt_ = true;
      return true;
    }
    if (stream_type == kPsStreamTypeH265) {
      video_pid_ = pid;
      codec_ = PsVideoCodec::kH265;
      have_codec_ = true;
      have_pmt_ = true;
      return true;
    }
  }
  return false;
}

void TsDemuxer::HandlePesPayload(const std::uint8_t* data, std::size_t size, bool start) {
  (void)data;
  (void)size;
  (void)start;
}

void TsDemuxer::FlushPes(std::vector<EsFrame>* frames) {
  if (pes_.size() < 9 || !pes_in_progress_) {
    pes_.clear();
    pes_in_progress_ = false;
    pes_expected_ = 0;
    return;
  }
  const std::uint8_t* pes = pes_.data();
  std::size_t payload_offset = 9 + pes[8];
  const std::uint8_t flags = pes[7];
  std::int64_t pts = 0;
  bool has_pts = false;
  if ((flags & 0x80) != 0 && payload_offset >= 14) {
    pts = ReadTimestamp(pes + 9);
    has_pts = true;
    pes_pts_ = pts;
    pes_has_pts_ = true;
  } else if ((flags & 0x40) != 0 && payload_offset >= 14) {
    // DTS-only PES is not produced by the encoders this service supports;
    // keep the previous PTS rather than inventing a timeline.
    has_pts = pes_has_pts_;
    pts = pes_pts_;
  } else {
    has_pts = pes_has_pts_;
    pts = pes_pts_;
  }
  if (payload_offset >= pes_.size()) {
    pes_.clear();
    pes_in_progress_ = false;
    pes_expected_ = 0;
    return;
  }
  const std::uint8_t* es = pes + payload_offset;
  const std::size_t es_size = pes_.size() - payload_offset;
  std::vector<EsAccessUnit> units;
  std::string split_error;
  if (SplitAnnexBAccessUnits(es, es_size, codec_, &units, &split_error)) {
    std::int64_t unit_pts = pts;
    for (size_t i = 0; i < units.size(); ++i) {
      if (frames == nullptr)
        break;
      EsFrame frame;
      frame.data = std::move(units[i].data);
      frame.codec = codec_;
      frame.keyframe = units[i].keyframe;
      frame.pts_90k = unit_pts;
      frame.has_pts = has_pts;
      if (frames != nullptr)
        frames->push_back(std::move(frame));
      unit_pts += 3000; // 25 fps default for multiple pictures in one PES
    }
  }
  pes_.clear();
  pes_in_progress_ = false;
  pes_expected_ = 0;
}

// ---------------------------------------------------------------------------
// PsRtpPipeline
// ---------------------------------------------------------------------------

PsRtpPipeline::PsRtpPipeline(PsFrameSender sender, PsRtpPipelineConfig config)
    : config_(std::move(config)), sender_(std::move(sender)),
      muxer_(std::make_unique<PsMuxer>(config_.muxer)) {}

bool PsRtpPipeline::PushTs(const std::uint8_t* data, std::size_t size, std::string* error) {
  std::vector<EsFrame> frames;
  if (!demuxer_.Push(data, size, &frames, error))
    return false;
  for (const EsFrame& frame : frames) {
    if (!PushFrame(frame, error))
      return false;
  }
  return true;
}

bool PsRtpPipeline::PushFrame(const EsFrame& frame, std::string* error) {
  if (frame.data.empty())
    return true;
  if (!have_codec_ || frame.codec != codec_) {
    codec_ = frame.codec;
    have_codec_ = true;
    config_.muxer.codec = codec_;
    muxer_ = std::make_unique<PsMuxer>(config_.muxer);
    have_base_ = false;
  }
  if (!have_base_) {
    base_pts_ = frame.has_pts ? frame.pts_90k : 0;
    next_synthetic_pts_ = base_pts_;
    have_base_ = true;
  }

  std::int64_t media_pts = frame.has_pts ? frame.pts_90k : next_synthetic_pts_;
  std::int64_t output_pts = media_pts;
  if (config_.rebase_timestamps)
    output_pts = media_pts - base_pts_ + config_.timestamp_offset_90k;
  next_synthetic_pts_ = media_pts + config_.default_frame_interval_90k;

  PsFrame ps;
  ps.data = frame.data.data();
  ps.size = frame.data.size();
  ps.pts_90k = output_pts;
  ps.keyframe = frame.keyframe;
  if (!muxer_->MuxFrame(ps, &pack_, error))
    return false;
  if (!sender_) {
    if (error != nullptr)
      *error = "PS/RTP pipeline has no sender";
    return false;
  }
  const std::uint32_t timestamp = static_cast<std::uint32_t>(output_pts < 0 ? 0 : output_pts);
  if (!sender_(pack_.data(), pack_.size(), timestamp, frame.keyframe, error))
    return false;
  ++frames_sent_;
  if (frame.keyframe)
    ++keyframes_sent_;
  return true;
}

bool PsRtpPipeline::Flush(std::string* error) {
  std::vector<EsFrame> frames;
  if (!demuxer_.Flush(&frames, error))
    return false;
  for (const EsFrame& frame : frames) {
    if (!PushFrame(frame, error))
      return false;
  }
  return true;
}

void PsRtpPipeline::Reset() {
  demuxer_.Reset();
  muxer_->Reset();
  have_codec_ = false;
  have_base_ = false;
  base_pts_ = 0;
  next_synthetic_pts_ = 0;
  frames_sent_ = 0;
  keyframes_sent_ = 0;
}

} // namespace uvnvr::gb28181
