#pragma once

// RTCP for the GB/T 28181 media session (RFC 3550, RFC 4585).
//
// The device side is an RTP sender, so it periodically emits a compound
// Sender Report + SDES CNAME and consumes Receiver Reports. A platform asks
// for an immediate I-frame with either a PLI or a FIR payload-specific
// feedback packet; both are parsed here and surfaced as one event so the
// caller only has to implement "produce an IDR".
//
// The module is pure (no sockets) so packet layouts can be unit tested. It is
// compiled only with UVNVR_ENABLE_GB28181=ON (default off).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb28181 {

inline constexpr std::uint8_t kRtcpPtSenderReport = 200;
inline constexpr std::uint8_t kRtcpPtReceiverReport = 201;
inline constexpr std::uint8_t kRtcpPtSdes = 202;
inline constexpr std::uint8_t kRtcpPtBye = 203;
inline constexpr std::uint8_t kRtcpPtApp = 204;
inline constexpr std::uint8_t kRtcpPtRtpFeedback = 205;
inline constexpr std::uint8_t kRtcpPtPayloadFeedback = 206;

// Payload-specific feedback formats (RFC 4585).
inline constexpr std::uint8_t kRtcpFmtPli = 1;
inline constexpr std::uint8_t kRtcpFmtFir = 4;
// Generic NACK (RFC 4585) for the RTP feedback payload type.
inline constexpr std::uint8_t kRtcpFmtGenericNack = 1;

struct RtcpReportBlock {
  std::uint32_t ssrc = 0;
  std::uint8_t fraction_lost = 0;
  // Signed 24-bit field; sign-extended while parsing.
  std::int32_t cumulative_lost = 0;
  std::uint32_t extended_highest_sequence = 0;
  std::uint32_t jitter = 0;
  std::uint32_t last_sender_report = 0;  // LSR, middle 32 bits of the NTP stamp
  std::uint32_t delay_since_last_sender_report = 0;  // DLSR, 1/65536 seconds
};

struct RtcpSenderInfo {
  std::uint32_t ntp_seconds = 0;
  std::uint32_t ntp_fraction = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint32_t packet_count = 0;
  std::uint32_t octet_count = 0;
};

// One RTCP packet. A compound datagram is represented as a vector of these.
struct RtcpPacket {
  std::uint8_t payload_type = 0;
  std::uint8_t count = 0;   // RC for SR/RR/SDES/BYE, FMT for feedback
  std::uint8_t format = 0;  // FMT for feedback packets (same value as count)
  std::uint32_t sender_ssrc = 0;

  bool has_sender_info = false;
  RtcpSenderInfo sender_info;
  std::vector<RtcpReportBlock> report_blocks;

  std::string cname;  // SDES item type 1
  std::vector<std::uint32_t> ssrcs;  // BYE
  std::string reason;                // BYE

  // Feedback packets (PT 205/206).
  std::uint32_t media_ssrc = 0;
  bool is_keyframe_request = false;  // PLI or FIR
  bool is_fir = false;
  std::uint8_t fir_sequence = 0;
  std::vector<std::uint16_t> nack_sequence_numbers;  // RTPFB FMT 1

  std::size_t size = 0;  // encoded size of this packet, including padding
};

const char* RtcpPayloadTypeName(std::uint8_t payload_type);

// Parses one RTCP packet starting at `data`. `packet.size` receives the
// encoded size so a compound parser can advance. Returns false for a truncated
// header, an unsupported version, an inconsistent length or a malformed body.
bool ParseRtcpPacket(const std::uint8_t* data, std::size_t size, RtcpPacket* packet,
                     std::string* error = nullptr);

// Parses every packet in a compound RTCP datagram. Trailing zero padding after
// the last complete packet is accepted; a truncated final packet is an error.
bool ParseRtcpCompound(const std::uint8_t* data, std::size_t size,
                       std::vector<RtcpPacket>* packets, std::string* error = nullptr);

// Builders. Every function returns the complete packet or compound bytes in
// network byte order, and an empty vector for invalid input.
std::vector<std::uint8_t> BuildRtcpSenderReport(
    std::uint32_t sender_ssrc, const RtcpSenderInfo& sender_info,
    const std::vector<RtcpReportBlock>& report_blocks = {});
std::vector<std::uint8_t> BuildRtcpSdesCname(std::uint32_t ssrc, std::string_view cname);
// Compound SR + SDES(CNAME), which is what RFC 3550 requires for a sender.
// An empty cname keeps the CNAME item empty rather than omitting the chunk.
std::vector<std::uint8_t> BuildRtcpSenderReportCompound(
    std::uint32_t sender_ssrc, const RtcpSenderInfo& sender_info, std::string_view cname,
    const std::vector<RtcpReportBlock>& report_blocks = {});
std::vector<std::uint8_t> BuildRtcpReceiverReport(
    std::uint32_t sender_ssrc, const std::vector<RtcpReportBlock>& report_blocks);
std::vector<std::uint8_t> BuildRtcpBye(const std::vector<std::uint32_t>& ssrcs,
                                       std::string_view reason = {});
std::vector<std::uint8_t> BuildRtcpPli(std::uint32_t sender_ssrc,
                                       std::uint32_t media_ssrc);
std::vector<std::uint8_t> BuildRtcpFir(std::uint32_t sender_ssrc,
                                       std::uint32_t media_ssrc,
                                       std::uint8_t fir_sequence = 0);
std::vector<std::uint8_t> BuildRtcpNack(std::uint32_t sender_ssrc,
                                        std::uint32_t media_ssrc,
                                        const std::vector<std::uint16_t>& lost);

// LSR is the middle 32 bits of an NTP timestamp, as carried in report blocks.
std::uint32_t RtcpCompactNtp(std::uint32_t ntp_seconds, std::uint32_t ntp_fraction);
// Converts 1/65536-second units to milliseconds (rounded up).
std::uint32_t RtcpDlsrToMs(std::uint32_t dlsr);

// Counters accumulated by a media session. They are snapshotted into operator
// diagnostics and the integration tests.
struct RtcpStats {
  std::uint64_t bytes_received = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t sender_reports_received = 0;
  std::uint64_t receiver_reports_received = 0;
  std::uint64_t sdes_received = 0;
  std::uint64_t byes_received = 0;
  std::uint64_t keyframe_requests_received = 0;
  std::uint64_t pli_received = 0;
  std::uint64_t fir_received = 0;

  std::uint64_t sender_reports_sent = 0;
  std::uint64_t byes_sent = 0;

  std::uint32_t receiver_ssrc = 0;
  std::uint8_t last_fraction_lost = 0;
  std::int32_t last_cumulative_lost = 0;
  std::uint32_t last_jitter = 0;
  std::uint32_t last_extended_highest_sequence = 0;
  std::uint32_t last_rtt_ms = 0;
  bool has_rtt = false;
};

}  // namespace uvnvr::gb28181
