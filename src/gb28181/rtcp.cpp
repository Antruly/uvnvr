// RTCP packet parsing and construction for GB/T 28181 media sessions.

#include "gb28181/rtcp.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace uvnvr::gb28181 {
namespace {

std::uint16_t ReadU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t ReadU24(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 16) |
         (static_cast<std::uint32_t>(data[1]) << 8) | static_cast<std::uint32_t>(data[2]);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value >> 8));
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU24(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>(value >> 24));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void PadToWord(std::vector<std::uint8_t>* out) {
  while (out->size() % 4 != 0)
    out->push_back(0);
}

std::size_t StartPacket(std::vector<std::uint8_t>* out, std::uint8_t count,
                        std::uint8_t payload_type) {
  const std::size_t start = out->size();
  out->push_back(static_cast<std::uint8_t>(0x80 | (count & 0x1F)));
  out->push_back(payload_type);
  out->push_back(0);  // length placeholder
  out->push_back(0);
  return start;
}

void FinishPacket(std::vector<std::uint8_t>* out, std::size_t start) {
  // RTCP length is the packet size in 32-bit words minus one.
  const std::size_t words = (out->size() - start) / 4;
  if (words == 0)
    return;
  const std::size_t encoded = words - 1;
  (*out)[start + 2] = static_cast<std::uint8_t>((encoded >> 8) & 0xFF);
  (*out)[start + 3] = static_cast<std::uint8_t>(encoded & 0xFF);
}

void AppendReportBlock(std::vector<std::uint8_t>* out, const RtcpReportBlock& block) {
  AppendU32(out, block.ssrc);
  out->push_back(block.fraction_lost);
  const std::uint32_t lost = static_cast<std::uint32_t>(block.cumulative_lost) & 0x00FFFFFFu;
  AppendU24(out, lost);
  AppendU32(out, block.extended_highest_sequence);
  AppendU32(out, block.jitter);
  AppendU32(out, block.last_sender_report);
  AppendU32(out, block.delay_since_last_sender_report);
}

bool ParseReportBlocks(const std::uint8_t* data, std::size_t size, std::size_t offset,
                       std::uint8_t count, std::vector<RtcpReportBlock>* blocks,
                       std::string* error) {
  const std::size_t needed = offset + 24u * count;
  if (needed > size) {
    if (error)
      *error = "RTCP report block list runs past the packet";
    return false;
  }
  blocks->clear();
  blocks->reserve(count);
  for (std::uint8_t i = 0; i < count; ++i) {
    const std::uint8_t* block = data + offset + 24u * i;
    RtcpReportBlock parsed;
    parsed.ssrc = ReadU32(block);
    parsed.fraction_lost = block[4];
    std::int32_t lost = static_cast<std::int32_t>(ReadU24(block + 5));
    if (lost & 0x00800000)
      lost |= static_cast<std::int32_t>(0xFF000000u);
    parsed.cumulative_lost = lost;
    parsed.extended_highest_sequence = ReadU32(block + 8);
    parsed.jitter = ReadU32(block + 12);
    parsed.last_sender_report = ReadU32(block + 16);
    parsed.delay_since_last_sender_report = ReadU32(block + 20);
    blocks->push_back(parsed);
  }
  return true;
}

bool ParseSdes(const std::uint8_t* data, std::size_t size, RtcpPacket* packet,
               std::string* error) {
  std::size_t offset = 4;
  for (std::uint8_t chunk = 0; chunk < packet->count; ++chunk) {
    if (offset + 4 > size) {
      if (error)
        *error = "RTCP SDES chunk is truncated";
      return false;
    }
    offset += 4;  // chunk SSRC; the first CNAME is what we need
    while (true) {
      if (offset >= size) {
        if (error)
          *error = "RTCP SDES item list has no END item";
        return false;
      }
      const std::uint8_t item_type = data[offset++];
      if (item_type == 0) {
        // END. The chunk is padded to the next 32-bit boundary.
        offset = (offset + 3u) & ~static_cast<std::size_t>(3u);
        break;
      }
      if (offset >= size) {
        if (error)
          *error = "RTCP SDES item length is truncated";
        return false;
      }
      const std::uint8_t item_length = data[offset++];
      if (offset + item_length > size) {
        if (error)
          *error = "RTCP SDES item value runs past the packet";
        return false;
      }
      if (item_type == 1 && packet->cname.empty())
        packet->cname.assign(reinterpret_cast<const char*>(data + offset), item_length);
      offset += item_length;
    }
  }
  return true;
}

}  // namespace

const char* RtcpPayloadTypeName(std::uint8_t payload_type) {
  switch (payload_type) {
    case kRtcpPtSenderReport:
      return "SR";
    case kRtcpPtReceiverReport:
      return "RR";
    case kRtcpPtSdes:
      return "SDES";
    case kRtcpPtBye:
      return "BYE";
    case kRtcpPtApp:
      return "APP";
    case kRtcpPtRtpFeedback:
      return "RTPFB";
    case kRtcpPtPayloadFeedback:
      return "PSFB";
    default:
      return "unknown";
  }
}

bool ParseRtcpPacket(const std::uint8_t* data, std::size_t size, RtcpPacket* packet,
                     std::string* error) {
  if (packet == nullptr) {
    if (error)
      *error = "RTCP packet output is null";
    return false;
  }
  *packet = RtcpPacket{};
  if (data == nullptr || size < 4) {
    if (error)
      *error = "RTCP packet is shorter than 4 bytes";
    return false;
  }
  if ((data[0] >> 6) != 2) {
    if (error)
      *error = "unsupported RTCP version";
    return false;
  }
  const std::uint8_t padding = (data[0] & 0x20) != 0;
  packet->count = data[0] & 0x1F;
  packet->format = packet->count;
  packet->payload_type = data[1];
  const std::size_t words = static_cast<std::size_t>(ReadU16(data + 2)) + 1;
  const std::size_t packet_size = words * 4;
  if (packet_size > size) {
    if (error)
      *error = "RTCP length field runs past the datagram";
    return false;
  }
  std::size_t body_size = packet_size;
  if (padding) {
    if (packet_size == 0) {
      if (error)
        *error = "RTCP padding flag on an empty packet";
      return false;
    }
    const std::uint8_t padding_size = data[packet_size - 1];
    if (padding_size == 0 || padding_size > packet_size - 4) {
      if (error)
        *error = "invalid RTCP padding";
      return false;
    }
    body_size = packet_size - padding_size;
  }
  packet->size = packet_size;

  switch (packet->payload_type) {
    case kRtcpPtSenderReport: {
      if (body_size < 28) {
        if (error)
          *error = "RTCP sender report is shorter than its sender info";
        return false;
      }
      packet->sender_ssrc = ReadU32(data + 4);
      packet->has_sender_info = true;
      packet->sender_info.ntp_seconds = ReadU32(data + 8);
      packet->sender_info.ntp_fraction = ReadU32(data + 12);
      packet->sender_info.rtp_timestamp = ReadU32(data + 16);
      packet->sender_info.packet_count = ReadU32(data + 20);
      packet->sender_info.octet_count = ReadU32(data + 24);
      return ParseReportBlocks(data, body_size, 28, packet->count, &packet->report_blocks,
                               error);
    }
    case kRtcpPtReceiverReport: {
      if (body_size < 8) {
        if (error)
          *error = "RTCP receiver report is truncated";
        return false;
      }
      packet->sender_ssrc = ReadU32(data + 4);
      return ParseReportBlocks(data, body_size, 8, packet->count, &packet->report_blocks,
                               error);
    }
    case kRtcpPtSdes:
      return ParseSdes(data, body_size, packet, error);
    case kRtcpPtBye: {
      if (body_size < 4) {
        if (error)
          *error = "RTCP BYE is truncated";
        return false;
      }
      if (4u + 4u * packet->count > body_size) {
        if (error)
          *error = "RTCP BYE SSRC list runs past the packet";
        return false;
      }
      for (std::uint8_t i = 0; i < packet->count; ++i)
        packet->ssrcs.push_back(ReadU32(data + 4 + 4u * i));
      std::size_t offset = 4 + 4u * packet->count;
      if (offset < body_size) {
        const std::uint8_t reason_length = data[offset++];
        if (offset + reason_length > body_size) {
          if (error)
            *error = "RTCP BYE reason runs past the packet";
          return false;
        }
        packet->reason.assign(reinterpret_cast<const char*>(data + offset), reason_length);
      }
      return true;
    }
    case kRtcpPtRtpFeedback:
    case kRtcpPtPayloadFeedback: {
      if (body_size < 12) {
        if (error)
          *error = "RTCP feedback packet is truncated";
        return false;
      }
      packet->sender_ssrc = ReadU32(data + 4);
      packet->media_ssrc = ReadU32(data + 8);
      if (packet->payload_type == kRtcpPtPayloadFeedback) {
        if (packet->format == kRtcpFmtPli) {
          packet->is_keyframe_request = true;
          return true;
        }
        if (packet->format == kRtcpFmtFir) {
          packet->is_keyframe_request = true;
          packet->is_fir = true;
          // FCI: SSRC (4), sequence number (1), reserved (3). A FIR may omit
          // the FCI in some implementations; the header alone is still a
          // valid key-frame request.
          if (body_size >= 20) {
            const std::uint32_t target = ReadU32(data + 12);
            if (target != 0)
              packet->media_ssrc = target;
            packet->fir_sequence = data[16];
          }
          return true;
        }
        return true;
      }
      if (packet->format == kRtcpFmtGenericNack) {
        for (std::size_t offset = 12; offset + 4 <= body_size; offset += 4) {
          const std::uint16_t pid = ReadU16(data + offset);
          const std::uint16_t blp = ReadU16(data + offset + 2);
          packet->nack_sequence_numbers.push_back(pid);
          for (int bit = 0; bit < 16; ++bit) {
            if (blp & (1u << bit))
              packet->nack_sequence_numbers.push_back(
                  static_cast<std::uint16_t>((pid + 1 + bit) & 0xFFFF));
          }
        }
      }
      return true;
    }
    case kRtcpPtApp:
    default:
      return true;
  }
}

bool ParseRtcpCompound(const std::uint8_t* data, std::size_t size,
                       std::vector<RtcpPacket>* packets, std::string* error) {
  if (packets == nullptr) {
    if (error)
      *error = "RTCP compound output is null";
    return false;
  }
  packets->clear();
  if (data == nullptr || size < 4) {
    if (error)
      *error = "RTCP datagram is shorter than one header";
    return false;
  }
  std::size_t offset = 0;
  while (offset + 4 <= size) {
    // Trailing zero padding after the last packet is legal.
    if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
        data[offset + 3] == 0) {
      break;
    }
    RtcpPacket packet;
    if (!ParseRtcpPacket(data + offset, size - offset, &packet, error))
      return false;
    if (packet.size == 0) {
      if (error)
        *error = "RTCP parser returned a zero-length packet";
      return false;
    }
    offset += packet.size;
    packets->push_back(std::move(packet));
  }
  if (offset != size) {
    if (error)
      *error = "trailing bytes after the last RTCP packet";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> BuildRtcpSenderReport(
    std::uint32_t sender_ssrc, const RtcpSenderInfo& sender_info,
    const std::vector<RtcpReportBlock>& report_blocks) {
  if (report_blocks.size() > 31)
    return {};
  std::vector<std::uint8_t> out;
  out.reserve(28 + 24 * report_blocks.size());
  const std::size_t start =
      StartPacket(&out, static_cast<std::uint8_t>(report_blocks.size()), kRtcpPtSenderReport);
  AppendU32(&out, sender_ssrc);
  AppendU32(&out, sender_info.ntp_seconds);
  AppendU32(&out, sender_info.ntp_fraction);
  AppendU32(&out, sender_info.rtp_timestamp);
  AppendU32(&out, sender_info.packet_count);
  AppendU32(&out, sender_info.octet_count);
  for (const RtcpReportBlock& block : report_blocks)
    AppendReportBlock(&out, block);
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpSdesCname(std::uint32_t ssrc, std::string_view cname) {
  if (cname.size() > 255)
    return {};
  std::vector<std::uint8_t> out;
  const std::size_t start = StartPacket(&out, 1, kRtcpPtSdes);
  AppendU32(&out, ssrc);
  out.push_back(1);  // CNAME
  out.push_back(static_cast<std::uint8_t>(cname.size()));
  out.insert(out.end(), cname.begin(), cname.end());
  out.push_back(0);  // END
  PadToWord(&out);
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpSenderReportCompound(
    std::uint32_t sender_ssrc, const RtcpSenderInfo& sender_info, std::string_view cname,
    const std::vector<RtcpReportBlock>& report_blocks) {
  std::vector<std::uint8_t> out =
      BuildRtcpSenderReport(sender_ssrc, sender_info, report_blocks);
  if (out.empty())
    return {};
  std::vector<std::uint8_t> sdes = BuildRtcpSdesCname(sender_ssrc, cname);
  if (sdes.empty())
    return {};
  out.reserve(out.size() + sdes.size());
  for (std::uint8_t byte : sdes)
    out.push_back(byte);
  return out;
}

std::vector<std::uint8_t> BuildRtcpReceiverReport(
    std::uint32_t sender_ssrc, const std::vector<RtcpReportBlock>& report_blocks) {
  if (report_blocks.size() > 31)
    return {};
  std::vector<std::uint8_t> out;
  const std::size_t start = StartPacket(
      &out, static_cast<std::uint8_t>(report_blocks.size()), kRtcpPtReceiverReport);
  AppendU32(&out, sender_ssrc);
  for (const RtcpReportBlock& block : report_blocks)
    AppendReportBlock(&out, block);
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpBye(const std::vector<std::uint32_t>& ssrcs,
                                       std::string_view reason) {
  if (ssrcs.size() > 31 || reason.size() > 255)
    return {};
  std::vector<std::uint8_t> out;
  const std::size_t start =
      StartPacket(&out, static_cast<std::uint8_t>(ssrcs.size()), kRtcpPtBye);
  for (std::uint32_t ssrc : ssrcs)
    AppendU32(&out, ssrc);
  if (!reason.empty()) {
    out.push_back(static_cast<std::uint8_t>(reason.size()));
    out.insert(out.end(), reason.begin(), reason.end());
    PadToWord(&out);
  }
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpPli(std::uint32_t sender_ssrc,
                                       std::uint32_t media_ssrc) {
  std::vector<std::uint8_t> out;
  const std::size_t start = StartPacket(&out, kRtcpFmtPli, kRtcpPtPayloadFeedback);
  AppendU32(&out, sender_ssrc);
  AppendU32(&out, media_ssrc);
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpFir(std::uint32_t sender_ssrc,
                                       std::uint32_t media_ssrc,
                                       std::uint8_t fir_sequence) {
  std::vector<std::uint8_t> out;
  const std::size_t start = StartPacket(&out, kRtcpFmtFir, kRtcpPtPayloadFeedback);
  AppendU32(&out, sender_ssrc);
  AppendU32(&out, 0);  // media source is carried in the FCI for FIR
  AppendU32(&out, media_ssrc);
  out.push_back(fir_sequence);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  FinishPacket(&out, start);
  return out;
}

std::vector<std::uint8_t> BuildRtcpNack(std::uint32_t sender_ssrc,
                                        std::uint32_t media_ssrc,
                                        const std::vector<std::uint16_t>& lost) {
  std::vector<std::uint8_t> out;
  const std::size_t start = StartPacket(&out, kRtcpFmtGenericNack, kRtcpPtRtpFeedback);
  AppendU32(&out, sender_ssrc);
  AppendU32(&out, media_ssrc);
  // Each FCI names one packet ID and a bitmask of the next 16 packets.
  // Non-contiguous loss starts a new FCI instead of being silently dropped.
  std::size_t index = 0;
  while (index < lost.size()) {
    const std::uint16_t pid = lost[index];
    std::uint16_t blp = 0;
    std::size_t next = index + 1;
    while (next < lost.size()) {
      const std::uint16_t offset =
          static_cast<std::uint16_t>((lost[next] - pid) & 0xFFFF);
      if (offset == 0 || offset > 16)
        break;
      blp = static_cast<std::uint16_t>(blp | (1u << (offset - 1)));
      ++next;
    }
    AppendU16(&out, pid);
    AppendU16(&out, blp);
    index = next;
  }
  FinishPacket(&out, start);
  return out;
}

std::uint32_t RtcpCompactNtp(std::uint32_t ntp_seconds, std::uint32_t ntp_fraction) {
  return (ntp_seconds << 16) | (ntp_fraction >> 16);
}

std::uint32_t RtcpDlsrToMs(std::uint32_t dlsr) {
  // DLSR is in 1/65536-second units. Add 65535 before dividing to round up.
  const std::uint64_t scaled = static_cast<std::uint64_t>(dlsr) * 1000u + 65535u;
  return static_cast<std::uint32_t>(scaled / 65536u);
}

}  // namespace uvnvr::gb28181
