// Unit tests for RTCP parse/build used by the GB/T 28181 media session.
//
// The test prints one decoded compound packet so the field layout can be
// compared with a capture when reviewing RFC 3550 / RFC 4585 behaviour.

#include "gb28181/rtcp.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace uvnvr::gb28181;

int failures = 0;

void Expect(const std::string& name, std::string_view actual, std::string_view expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%.*s', expected '%.*s'\n", name.c_str(),
               static_cast<int>(actual.size()), actual.data(),
               static_cast<int>(expected.size()), expected.data());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(),
               actual ? "true" : "false", expected ? "true" : "false");
  ++failures;
}

void ExpectU64(const std::string& name, std::uint64_t actual, std::uint64_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %llu, expected %llu\n", name.c_str(),
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
  ++failures;
}

void ExpectI64(const std::string& name, std::int64_t actual, std::int64_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %lld, expected %lld\n", name.c_str(),
               static_cast<long long>(actual), static_cast<long long>(expected));
  ++failures;
}

void HexDump(const std::vector<std::uint8_t>& data, std::size_t max_bytes) {
  const std::size_t count = data.size() < max_bytes ? data.size() : max_bytes;
  for (std::size_t i = 0; i < count; i += 16) {
    std::printf("%04zx  ", i);
    for (std::size_t j = 0; j < 16 && i + j < count; ++j)
      std::printf("%02x ", data[i + j]);
    std::printf("\n");
  }
}

void TestSenderReport() {
  RtcpSenderInfo info;
  info.ntp_seconds = 0xE5012345u;  // 2025-ish
  info.ntp_fraction = 0x80000000u;
  info.rtp_timestamp = 123456;
  info.packet_count = 987;
  info.octet_count = 654321;

  RtcpReportBlock block;
  block.ssrc = 0x11223344u;
  block.fraction_lost = 12;
  block.cumulative_lost = -5;
  block.extended_highest_sequence = 0x0012FFFF;
  block.jitter = 360;
  block.last_sender_report = 0xAABBCCDDu;
  block.delay_since_last_sender_report = 0x00010000u;

  const std::vector<std::uint8_t> bytes =
      BuildRtcpSenderReportCompound(0x01020304u, info, "34020000001320000001@uvnvr", {block});
  ExpectBool("SR compound non-empty", !bytes.empty(), true);
  std::printf("RTCP compound SR+SDES (%zu bytes):\n", bytes.size());
  HexDump(bytes, 80);

  std::vector<RtcpPacket> packets;
  std::string error;
  ExpectBool("parse SR compound",
             ParseRtcpCompound(bytes.data(), bytes.size(), &packets, &error), true);
  ExpectU64("SR compound packet count", packets.size(), 2);
  if (packets.size() != 2)
    return;

  const RtcpPacket& sr = packets[0];
  ExpectU64("SR payload type", sr.payload_type, kRtcpPtSenderReport);
  ExpectBool("SR has sender info", sr.has_sender_info, true);
  ExpectU64("SR sender ssrc", sr.sender_ssrc, 0x01020304u);
  ExpectU64("SR ntp seconds", sr.sender_info.ntp_seconds, info.ntp_seconds);
  ExpectU64("SR ntp fraction", sr.sender_info.ntp_fraction, info.ntp_fraction);
  ExpectU64("SR rtp timestamp", sr.sender_info.rtp_timestamp, info.rtp_timestamp);
  ExpectU64("SR packet count", sr.sender_info.packet_count, info.packet_count);
  ExpectU64("SR octet count", sr.sender_info.octet_count, info.octet_count);
  ExpectU64("SR report block count", sr.report_blocks.size(), 1);
  if (!sr.report_blocks.empty()) {
    const RtcpReportBlock& parsed = sr.report_blocks[0];
    ExpectU64("SR block ssrc", parsed.ssrc, block.ssrc);
    ExpectU64("SR block fraction lost", parsed.fraction_lost, block.fraction_lost);
    ExpectI64("SR block cumulative lost (signed)", parsed.cumulative_lost,
              block.cumulative_lost);
    ExpectU64("SR block highest sequence", parsed.extended_highest_sequence,
              block.extended_highest_sequence);
    ExpectU64("SR block jitter", parsed.jitter, block.jitter);
    ExpectU64("SR block LSR", parsed.last_sender_report, block.last_sender_report);
    ExpectU64("SR block DLSR", parsed.delay_since_last_sender_report,
              block.delay_since_last_sender_report);
  }

  const RtcpPacket& sdes = packets[1];
  ExpectU64("SDES payload type", sdes.payload_type, kRtcpPtSdes);
  Expect("SDES CNAME", sdes.cname, "34020000001320000001@uvnvr");
  ExpectU64("RTCP compact NTP", RtcpCompactNtp(info.ntp_seconds, info.ntp_fraction),
            0x23458000u);
  ExpectU64("DLSR to ms", RtcpDlsrToMs(0x00010000u), 1000);
}

void TestReceiverReport() {
  RtcpReportBlock first;
  first.ssrc = 0xDEADBEEF;
  first.fraction_lost = 255;
  first.cumulative_lost = 8388607;  // largest positive 24-bit value
  RtcpReportBlock second;
  second.ssrc = 0xCAFEBABE;
  second.cumulative_lost = -8388608;  // most negative 24-bit value

  const std::vector<std::uint8_t> bytes =
      BuildRtcpReceiverReport(0x55667788u, {first, second});
  ExpectBool("RR non-empty", !bytes.empty(), true);

  RtcpPacket packet;
  std::string error;
  ExpectBool("parse RR", ParseRtcpPacket(bytes.data(), bytes.size(), &packet, &error), true);
  ExpectU64("RR payload type", packet.payload_type, kRtcpPtReceiverReport);
  ExpectU64("RR count", packet.count, 2);
  ExpectU64("RR sender ssrc", packet.sender_ssrc, 0x55667788u);
  ExpectU64("RR blocks", packet.report_blocks.size(), 2);
  if (packet.report_blocks.size() == 2) {
    ExpectI64("RR max positive loss", packet.report_blocks[0].cumulative_lost,
              first.cumulative_lost);
    ExpectI64("RR max negative loss", packet.report_blocks[1].cumulative_lost,
              second.cumulative_lost);
  }

  // The length field is 32-bit words minus one.
  ExpectU64("RR length words", (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3],
            1 + 6 * 2);
}

void TestFeedback() {
  std::string error;
  {
    const std::vector<std::uint8_t> bytes = BuildRtcpPli(0x01020304u, 0x11223344u);
    ExpectBool("PLI non-empty", !bytes.empty(), true);
    RtcpPacket packet;
    ExpectBool("parse PLI", ParseRtcpPacket(bytes.data(), bytes.size(), &packet, &error),
               true);
    ExpectU64("PLI payload type", packet.payload_type, kRtcpPtPayloadFeedback);
    ExpectU64("PLI format", packet.format, kRtcpFmtPli);
    ExpectBool("PLI keyframe request", packet.is_keyframe_request, true);
    ExpectBool("PLI not FIR", packet.is_fir, false);
    ExpectU64("PLI sender ssrc", packet.sender_ssrc, 0x01020304u);
    ExpectU64("PLI media ssrc", packet.media_ssrc, 0x11223344u);
    ExpectU64("PLI header size", packet.size, 12);
  }
  {
    const std::vector<std::uint8_t> bytes = BuildRtcpFir(0x01020304u, 0x11223344u, 17);
    ExpectBool("FIR non-empty", !bytes.empty(), true);
    RtcpPacket packet;
    ExpectBool("parse FIR", ParseRtcpPacket(bytes.data(), bytes.size(), &packet, &error),
               true);
    ExpectU64("FIR format", packet.format, kRtcpFmtFir);
    ExpectBool("FIR keyframe request", packet.is_keyframe_request, true);
    ExpectBool("FIR flag", packet.is_fir, true);
    ExpectU64("FIR media ssrc from FCI", packet.media_ssrc, 0x11223344u);
    ExpectU64("FIR sequence", packet.fir_sequence, 17);
    ExpectU64("FIR header size", packet.size, 20);
  }
  {
    const std::vector<std::uint8_t> bytes =
        BuildRtcpNack(0x01020304u, 0x11223344u, {100, 102, 103, 200});
    RtcpPacket packet;
    ExpectBool("parse NACK", ParseRtcpPacket(bytes.data(), bytes.size(), &packet, &error),
               true);
    ExpectU64("NACK payload type", packet.payload_type, kRtcpPtRtpFeedback);
    ExpectU64("NACK entries", packet.nack_sequence_numbers.size(), 4);
    if (packet.nack_sequence_numbers.size() == 4) {
      ExpectU64("NACK PID", packet.nack_sequence_numbers[0], 100);
      ExpectU64("NACK BLP +2", packet.nack_sequence_numbers[1], 102);
      ExpectU64("NACK BLP +3", packet.nack_sequence_numbers[2], 103);
      ExpectU64("NACK second PID", packet.nack_sequence_numbers[3], 200);
    }
  }
}

void TestBye() {
  const std::vector<std::uint8_t> bytes = BuildRtcpBye({0x11223344u, 0x55667788u}, "bye");
  ExpectBool("BYE non-empty", !bytes.empty(), true);
  RtcpPacket packet;
  std::string error;
  ExpectBool("parse BYE", ParseRtcpPacket(bytes.data(), bytes.size(), &packet, &error),
             true);
  ExpectU64("BYE payload type", packet.payload_type, kRtcpPtBye);
  ExpectU64("BYE SSRC count", packet.ssrcs.size(), 2);
  if (packet.ssrcs.size() == 2)
    ExpectU64("BYE SSRC", packet.ssrcs[1], 0x55667788u);
  Expect("BYE reason", packet.reason, "bye");

  const std::vector<std::uint8_t> no_reason = BuildRtcpBye({0x11223344u}, {});
  RtcpPacket packet2;
  ExpectBool("parse BYE without reason",
             ParseRtcpPacket(no_reason.data(), no_reason.size(), &packet2, &error), true);
  Expect("BYE empty reason", packet2.reason, "");
}

void TestMalformed() {
  std::string error;
  RtcpPacket packet;
  const std::vector<std::uint8_t> pli = BuildRtcpPli(1, 2);
  ExpectBool("reject short RTCP",
             ParseRtcpPacket(pli.data(), 3, &packet, &error), false);
  std::vector<std::uint8_t> bad_length = pli;
  bad_length[3] = 0xFF;  // claims a much larger packet
  ExpectBool("reject length past datagram",
             ParseRtcpPacket(bad_length.data(), bad_length.size(), &packet, &error), false);
  std::vector<std::uint8_t> bad_version = pli;
  bad_version[0] = 0x40;  // version 1
  ExpectBool("reject RTCP version 1",
             ParseRtcpPacket(bad_version.data(), bad_version.size(), &packet, &error), false);
  std::vector<std::uint8_t> bad_ssrc = BuildRtcpSenderReport(1, {});
  bad_ssrc.resize(20);  // truncated sender info
  bad_ssrc[3] = 4;      // length says 20 bytes
  ExpectBool("reject truncated SR",
             ParseRtcpPacket(bad_ssrc.data(), bad_ssrc.size(), &packet, &error), false);

  const std::vector<std::uint8_t> compound =
      BuildRtcpSenderReportCompound(1, {}, "c");
  std::vector<RtcpPacket> truncated;
  ExpectBool("reject truncated compound",
             ParseRtcpCompound(compound.data(), compound.size() - 2, &truncated, &error),
             false);
}

}  // namespace

int main() {
  TestSenderReport();
  TestReceiverReport();
  TestFeedback();
  TestBye();
  TestMalformed();

  if (failures == 0) {
    std::printf("gb28181_rtcp_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb28181_rtcp_test: %d checks failed\n", failures);
  return 1;
}
