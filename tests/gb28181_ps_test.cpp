// Unit tests for the GB/T 28181 PS muxer and RTP packetizer.
//
// The test prints the generated PS header fields and the RTP packet summary so
// the output can be quoted next to a hex dump when reviewing the packet
// layout against GB/T 28181 Appendix C.

#include "gb28181/ps.h"
#include "gb28181/rtp.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
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
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectInt(const std::string& name, std::uint64_t actual, std::uint64_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %llu, expected %llu\n", name.c_str(),
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
  ++failures;
}

void ExpectSize(const std::string& name, std::size_t actual, std::size_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %zu, expected %zu\n", name.c_str(), actual, expected);
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: '%s' does not contain '%s'\n", name.c_str(), haystack.c_str(),
               needle.c_str());
  ++failures;
}

void AppendNal(std::vector<std::uint8_t>* stream, std::initializer_list<std::uint8_t> nal,
               bool four_byte_start_code = true) {
  stream->push_back(0);
  stream->push_back(0);
  if (four_byte_start_code)
    stream->push_back(0);
  stream->push_back(1);
  stream->insert(stream->end(), nal.begin(), nal.end());
}

std::vector<std::uint8_t> MakeH264AccessUnit(bool keyframe) {
  std::vector<std::uint8_t> unit;
  AppendNal(&unit, {0x09, 0xF0});                    // access unit delimiter
  AppendNal(&unit, {0x67, 0x42, 0x00, 0x0A});        // SPS
  AppendNal(&unit, {0x68, 0xCE, 0x3C, 0x80});        // PPS
  if (keyframe)
    AppendNal(&unit, {0x65, 0x80, 0xAA, 0xBB});      // IDR slice, first_mb_in_slice = 0
  else
    AppendNal(&unit, {0x41, 0x80, 0xCC, 0xDD});      // non-IDR slice
  return unit;
}

void TestAnnexBSplitter() {
  // H.264: two access units, with a three-byte start code on the first NAL to
  // exercise the canonicalisation path.
  std::vector<std::uint8_t> h264;
  AppendNal(&h264, {0x09, 0xF0}, false);
  AppendNal(&h264, {0x67, 0x42, 0x00, 0x0A});
  AppendNal(&h264, {0x68, 0xCE, 0x3C, 0x80});
  AppendNal(&h264, {0x65, 0x80, 0xAA});
  AppendNal(&h264, {0x41, 0x80, 0xCC});
  AppendNal(&h264, {0x41, 0x40, 0xDD});  // second slice of the same picture

  std::vector<EsAccessUnit> h264_units;
  std::string error;
  ExpectBool("h264 split", SplitAnnexBAccessUnits(h264.data(), h264.size(),
                                                   PsVideoCodec::kH264, &h264_units, &error),
             true);
  ExpectSize("h264 unit count", h264_units.size(), 2);
  ExpectBool("h264 first keyframe", h264_units.at(0).keyframe, true);
  ExpectBool("h264 second non-key", h264_units.at(1).keyframe, false);
  ExpectBool("h264 canonical start code",
             h264_units.at(0).data.size() >= 4 && h264_units.at(0).data[0] == 0 &&
                 h264_units.at(0).data[1] == 0 && h264_units.at(0).data[2] == 0 &&
                 h264_units.at(0).data[3] == 1,
             true);

  // H.265: VPS/SPS/PPS + IDR_W_RADL, then a TRAIL_R slice.
  std::vector<std::uint8_t> h265;
  AppendNal(&h265, {0x40, 0x01, 0x0C, 0x01});  // VPS
  AppendNal(&h265, {0x42, 0x01, 0x01, 0x02});  // SPS
  AppendNal(&h265, {0x44, 0x01, 0xC0});        // PPS
  AppendNal(&h265, {0x26, 0x01, 0x80, 0xAA});  // IDR_W_RADL, first_slice_segment = 1
  AppendNal(&h265, {0x02, 0x01, 0x80, 0xBB});  // TRAIL_R
  std::vector<EsAccessUnit> h265_units;
  ExpectBool("h265 split", SplitAnnexBAccessUnits(h265.data(), h265.size(),
                                                   PsVideoCodec::kH265, &h265_units, &error),
             true);
  ExpectSize("h265 unit count", h265_units.size(), 2);
  ExpectBool("h265 first keyframe", h265_units.at(0).keyframe, true);
  ExpectBool("h265 second non-key", h265_units.at(1).keyframe, false);
}

void TestPsMuxer() {
  const std::vector<std::uint8_t> keyframe = MakeH264AccessUnit(true);
  const std::vector<std::uint8_t> delta = MakeH264AccessUnit(false);

  PsMuxer muxer;
  std::vector<std::uint8_t> ps;
  std::string error;

  PsFrame first{keyframe.data(), keyframe.size(), 0, 0, false, true};
  ExpectBool("mux first frame", muxer.MuxFrame(first, &ps, &error), true);
  ExpectBool("pack start", ps.size() > 4 && ps[0] == 0 && ps[1] == 0 && ps[2] == 1 &&
                               ps[3] == 0xBA,
             true);

  PsPackInfo info;
  ExpectBool("parse first pack", ParsePsPack(ps.data(), ps.size(), &info, &error), true);
  ExpectSize("pack header length", info.pack_header_size, 14);
  ExpectInt("program mux rate", info.program_mux_rate, 6106);
  ExpectBool("system header present", info.has_system_header, true);
  ExpectInt("system header stream id", info.system_header_stream_id, 0xE0);
  ExpectBool("PSM present", info.has_psm, true);
  ExpectInt("PSM stream type", info.psm_stream_type, 0x1B);
  ExpectInt("PSM ES id", info.psm_elementary_stream_id, 0xE0);
  ExpectBool("PSM CRC valid", info.psm_crc_valid, true);
  ExpectInt("PSM CRC known value", info.psm_crc_stored, 0x32141109u);
  ExpectInt("PES stream id", info.pes_stream_id, 0xE0);
  ExpectBool("PES data alignment", info.pes_data_alignment, true);
  ExpectBool("PES has PTS", info.has_pts, true);
  ExpectBool("PES has no DTS", info.has_dts, false);
  ExpectInt("PES PTS", static_cast<std::uint64_t>(info.pts), 0);
  ExpectSize("PES payload size", info.pes_payload_size, keyframe.size());
  ExpectBool("PES payload bytes",
             info.pes_payload_size == keyframe.size() &&
                 std::memcmp(ps.data() + info.pes_payload_offset, keyframe.data(),
                             keyframe.size()) == 0,
             true);

  std::printf("PS header (first 64 bytes):\n%s", HexDump(ps.data(), ps.size(), 64).c_str());
  std::printf("pack: offset=0 size=%zu scr=%llu mux_rate=%u\n", info.pack_header_size,
              static_cast<unsigned long long>(info.scr_base), info.program_mux_rate);
  std::printf("system header: offset=%zu length=%u stream_id=0x%02X\n",
              info.system_header_offset, info.system_header_length,
              info.system_header_stream_id);
  std::printf("PSM: offset=%zu length=%u stream_type=0x%02X es_id=0x%02X crc=0x%08X valid=%d\n",
              info.psm_offset, info.psm_length, info.psm_stream_type,
              info.psm_elementary_stream_id, info.psm_crc_stored,
              info.psm_crc_valid ? 1 : 0);
  std::printf("PES: offset=%zu stream_id=0x%02X pts=%lld alignment=%d payload=%zu\n",
              info.pes_offset, info.pes_stream_id, static_cast<long long>(info.pts),
              info.pes_data_alignment ? 1 : 0, info.pes_payload_size);

  PsFrame second{delta.data(), delta.size(), 3600, 0, false, false};
  ExpectBool("mux second frame", muxer.MuxFrame(second, &ps, &error), true);
  ExpectBool("parse second pack", ParsePsPack(ps.data(), ps.size(), &info, &error), true);
  ExpectBool("no system header on delta", info.has_system_header, false);
  ExpectBool("no PSM on delta", info.has_psm, false);
  ExpectInt("second PTS", static_cast<std::uint64_t>(info.pts), 3600);
  ExpectInt("second timestamp delta",
            static_cast<std::uint64_t>(info.pts), static_cast<std::uint64_t>(3600));

  // A frame larger than max_pes_payload becomes several PES packets inside one
  // pack; the first carries PTS, continuations do not.
  std::vector<std::uint8_t> large(140000, 0x5A);
  PsFrame large_frame{large.data(), large.size(), 7200, 0, false, false};
  ExpectBool("mux large frame", muxer.MuxFrame(large_frame, &ps, &error), true);
  ExpectBool("parse large pack", ParsePsPack(ps.data(), ps.size(), &info, &error), true);
  ExpectSize("large first PES payload", info.pes_payload_size, 65000);
  std::size_t pes_count = 0;
  for (std::size_t i = 0; i + 3 < ps.size(); ++i) {
    if (ps[i] == 0 && ps[i + 1] == 0 && ps[i + 2] == 1 && ps[i + 3] == 0xE0)
      ++pes_count;
  }
  ExpectInt("large frame PES count", pes_count, 3);
  const std::size_t next_pes = info.pes_offset + 6 + info.pes_packet_length;
  ExpectBool("continuation PES start", next_pes + 3 < ps.size() && ps[next_pes] == 0 &&
                                           ps[next_pes + 1] == 0 && ps[next_pes + 2] == 1 &&
                                           ps[next_pes + 3] == 0xE0,
             true);
  ExpectBool("continuation has no PTS", (ps[next_pes + 7] & 0xC0) == 0, true);

  // DTS form.
  PsFrame dts_frame{delta.data(), delta.size(), 7200, 3600, true, true};
  PsMuxer dts_muxer;
  ExpectBool("mux DTS frame", dts_muxer.MuxFrame(dts_frame, &ps, &error), true);
  ExpectBool("parse DTS pack", ParsePsPack(ps.data(), ps.size(), &info, &error), true);
  ExpectBool("has PTS", info.has_pts, true);
  ExpectBool("has DTS", info.has_dts, true);
  ExpectInt("PTS with DTS", static_cast<std::uint64_t>(info.pts), 7200);
  ExpectInt("DTS", static_cast<std::uint64_t>(info.dts), 3600);
  ExpectInt("SCR follows DTS", static_cast<std::uint64_t>(info.scr_base), 3600);

  // H.265 uses stream_type 0x24 in the PSM.
  PsMuxerConfig h265_config;
  h265_config.codec = PsVideoCodec::kH265;
  PsMuxer h265_muxer(h265_config);
  const std::vector<std::uint8_t> h265_frame = {0, 0, 0, 1, 0x26, 0x01, 0x80};
  PsFrame h265_ps_frame{h265_frame.data(), h265_frame.size(), 0, 0, false, true};
  ExpectBool("mux h265", h265_muxer.MuxFrame(h265_ps_frame, &ps, &error), true);
  ExpectBool("parse h265 pack", ParsePsPack(ps.data(), ps.size(), &info, &error), true);
  ExpectInt("h265 PSM stream type", info.psm_stream_type, 0x24);
  ExpectBool("h265 PSM CRC valid", info.psm_crc_valid, true);
  ExpectInt("h265 PSM CRC known value", info.psm_crc_stored, 0xCA811C3Au);
}

void TestRtpPacketizer() {
  RtpPacketizerConfig config;
  config.payload_type = 96;
  config.ssrc = 0x11223344;
  config.initial_sequence = 100;
  config.max_payload_size = 1400;
  RtpPacketizer packetizer(config);

  std::vector<std::uint8_t> frame(3000);
  for (std::size_t i = 0; i < frame.size(); ++i)
    frame[i] = static_cast<std::uint8_t>(i & 0xFF);

  std::vector<std::vector<std::uint8_t>> packets;
  std::string error;
  ExpectBool("packetize key frame",
             packetizer.PacketizeFrame(frame.data(), frame.size(), 3600, &packets, &error),
             true);
  ExpectSize("key frame packet count", packets.size(), 3);

  std::vector<std::uint8_t> reassembled;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    RtpPacketHeader header;
    ExpectBool("parse RTP packet",
               ParseRtpPacket(packets[i].data(), packets[i].size(), &header, &error), true);
    ExpectInt("RTP payload type", header.payload_type, 96);
    ExpectInt("RTP SSRC", header.ssrc, 0x11223344);
    ExpectInt("RTP sequence", header.sequence, 100 + i);
    ExpectInt("RTP timestamp", header.timestamp, 3600);
    ExpectBool("marker only on last", header.marker, i + 1 == packets.size());
    ExpectSize("RTP header size", header.header_size, 12);
    reassembled.insert(reassembled.end(), packets[i].begin() + 12, packets[i].end());
  }
  ExpectBool("RTP payload reassembles", reassembled == frame, true);

  std::vector<std::uint8_t> second(100, 0xA5);
  ExpectBool("packetize delta frame",
             packetizer.PacketizeFrame(second.data(), second.size(), 7200, &packets, &error),
             true);
  ExpectSize("delta frame packet count", packets.size(), 1);
  RtpPacketHeader header;
  ExpectBool("parse delta RTP packet",
             ParseRtpPacket(packets[0].data(), packets[0].size(), &header, &error), true);
  ExpectInt("delta sequence", header.sequence, 103);
  ExpectInt("delta timestamp", header.timestamp, 7200);
  ExpectBool("delta marker", header.marker, true);
  ExpectInt("next sequence", packetizer.next_sequence(), 104);
  ExpectInt("timestamp continuity", header.timestamp - 3600, 3600);
  std::printf("RTP: key frame 3000 bytes -> 3 packets seq=100..102 ts=3600 marker=001; "
              "delta 100 bytes -> 1 packet seq=103 ts=7200 marker=1\n");

  ExpectBool("reject short RTP",
             ParseRtpPacket(packets[0].data(), 11, &header, &error), false);
  ExpectBool("reject empty PS",
             packetizer.PacketizeFrame(nullptr, 0, 0, &packets, &error), false);
}

void TestSdp() {
  SdpVideoSession udp;
  udp.address = "192.168.1.10";
  udp.port = 5004;
  udp.origin_id = "34020000001320000001";
  udp.session_name = "Play";
  udp.mode = RtpTransportMode::kUdp;
  udp.payload_type = 96;
  udp.has_ssrc = true;
  udp.ssrc = 1;
  udp.direction = "sendonly";
  const std::string udp_text = BuildSdp(udp);
  ExpectContains("SDP version", udp_text, "v=0\r\n");
  ExpectContains("SDP media", udp_text, "m=video 5004 RTP/AVP 96\r\n");
  ExpectContains("SDP rtpmap", udp_text, "a=rtpmap:96 PS/90000\r\n");
  ExpectContains("SDP direction", udp_text, "a=sendonly\r\n");
  ExpectContains("SDP SSRC", udp_text, "y=0000000001\r\n");
  ExpectContains("SDP connection", udp_text, "c=IN IP4 192.168.1.10\r\n");

  SdpVideoSession parsed;
  std::string error;
  ExpectBool("parse UDP SDP", ParseSdp(udp_text, &parsed, &error), true);
  ExpectBool("parsed UDP mode", parsed.mode == RtpTransportMode::kUdp, true);
  ExpectInt("parsed UDP port", parsed.port, 5004);
  ExpectInt("parsed UDP payload type", parsed.payload_type, 96);
  ExpectInt("parsed UDP clock", parsed.clock_rate, 90000);
  ExpectInt("parsed UDP SSRC", parsed.ssrc, 1);
  Expect("parsed UDP address", parsed.address, "192.168.1.10");
  Expect("parsed UDP direction", parsed.direction, "sendonly");

  SdpVideoSession tcp;
  tcp.address = "192.168.1.10";
  tcp.port = 6000;
  tcp.origin_id = "34020000001320000001";
  tcp.mode = RtpTransportMode::kTcpPassive;
  tcp.setup = RtpSetup::kPassive;
  tcp.connection_new = true;
  tcp.has_ssrc = true;
  tcp.ssrc = 2;
  tcp.direction = "sendonly";
  const std::string tcp_text = BuildSdp(tcp);
  ExpectContains("TCP media", tcp_text, "m=video 6000 TCP/RTP/AVP 96\r\n");
  ExpectContains("TCP setup", tcp_text, "a=setup:passive\r\n");
  ExpectContains("TCP connection new", tcp_text, "a=connection:new\r\n");
  ExpectContains("TCP SSRC", tcp_text, "y=0000000002\r\n");
  ExpectBool("parse TCP SDP", ParseSdp(tcp_text, &parsed, &error), true);
  ExpectBool("parsed TCP passive", parsed.mode == RtpTransportMode::kTcpPassive, true);
  ExpectBool("parsed setup passive", parsed.setup == RtpSetup::kPassive, true);
  ExpectBool("parsed connection new", parsed.connection_new, true);

  SdpVideoSession offer = tcp;
  offer.mode = RtpTransportMode::kTcpActive;
  offer.setup = RtpSetup::kActive;
  std::string answer_text;
  SdpVideoSession answer;
  ExpectBool("build active answer",
             BuildSdpAnswer(offer, "192.168.1.20", 7000, "34020000001320000002", 3,
                            &answer_text, &answer, &error),
             true);
  ExpectBool("answer passive", answer.mode == RtpTransportMode::kTcpPassive, true);
  ExpectBool("answer setup passive", answer.setup == RtpSetup::kPassive, true);
  ExpectInt("answer port", answer.port, 7000);
  Expect("answer address", answer.address, "192.168.1.20");
  Expect("answer direction", answer.direction, "sendonly");
  ExpectInt("answer SSRC", answer.ssrc, 3);
  ExpectContains("answer SDP setup", answer_text, "a=setup:passive\r\n");
  ExpectContains("answer SDP media", answer_text, "m=video 7000 TCP/RTP/AVP 96\r\n");

  SdpVideoSession no_direction;
  ExpectBool("parse SDP without direction",
             ParseSdp("v=0\r\no=- 0 0 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 1.2.3.4\r\n"
                      "t=0 0\r\nm=video 5004 RTP/AVP 96\r\n"
                      "a=rtpmap:96 PS/90000\r\n",
                      &no_direction, &error),
             true);
  Expect("default parsed direction", no_direction.direction, "sendrecv");

  SdpVideoSession no_video;
  ExpectBool("reject SDP without video",
             ParseSdp("v=0\r\no=- 0 0 IN IP4 1.2.3.4\r\ns=x\r\nt=0 0\r\n", &no_video, &error),
             false);
  ExpectBool("reject SDP without PS",
             ParseSdp("v=0\r\no=- 0 0 IN IP4 1.2.3.4\r\ns=x\r\nc=IN IP4 1.2.3.4\r\n"
                      "t=0 0\r\nm=video 5004 RTP/AVP 97\r\na=rtpmap:97 H264/90000\r\n",
                      &no_video, &error),
             false);
  std::printf("SDP: UDP and TCP passive/active round-trip; active offer answered passive\n");
}

}  // namespace

int main() {
  TestAnnexBSplitter();
  TestPsMuxer();
  TestRtpPacketizer();
  TestSdp();

  if (failures == 0) {
    std::printf("gb28181_ps_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb28181_ps_test: %d checks failed\n", failures);
  return 1;
}
