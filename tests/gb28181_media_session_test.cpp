// Socket-level tests for the GB/T 28181 media dialog manager.
//
// Unlike gb28181_ps_test, this test plays the platform: it sends an SDP offer,
// connects (or accepts) the RTP socket, receives real packets, sends RTCP
// Receiver Reports / PLI / FIR / BYE, and checks the INVITE/ACK/BYE state
// machine. It therefore proves the control-plane binding and the on-wire
// packet counts, sequence numbers and timestamp deltas together.

#include "gb28181/media_session.h"
#include "gb28181/ps.h"
#include "gb28181/rtcp.h"
#include "gb28181/rtp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

std::int64_t MonotonicMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Small socket helpers (IPv4 loopback only, which is all a lab test needs).
// ---------------------------------------------------------------------------

#ifdef _WIN32
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;

void EnsureSockets() {
  static bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}

int LastSocketError() { return WSAGetLastError(); }
void CloseSocket(Socket socket) {
  if (socket != kInvalidSocket)
    closesocket(socket);
}
#else
using Socket = int;
const Socket kInvalidSocket = -1;

void EnsureSockets() {}
int LastSocketError() { return errno; }
void CloseSocket(Socket socket) {
  if (socket != kInvalidSocket)
    ::close(socket);
}
#endif

sockaddr_in Loopback(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

Socket BoundUdp(std::uint16_t port) {
  const Socket socket = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket == kInvalidSocket)
    return kInvalidSocket;
  int one = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
             sizeof(one));
  const sockaddr_in address = Loopback(port);
  if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    CloseSocket(socket);
    return kInvalidSocket;
  }
  return socket;
}

std::uint16_t BoundPort(Socket socket) {
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    return 0;
  return ntohs(address.sin_port);
}

// Binds an even RTP port and the following RTCP port, as RFC 3550 expects.
bool BindUdpPair(Socket* rtp, Socket* rtcp, std::uint16_t* rtp_port) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const Socket candidate = BoundUdp(0);
    if (candidate == kInvalidSocket)
      return false;
    const std::uint16_t port = BoundPort(candidate);
    if (port == 0 || (port & 1u) || port == 65535) {
      CloseSocket(candidate);
      continue;
    }
    const Socket control = BoundUdp(static_cast<std::uint16_t>(port + 1));
    if (control == kInvalidSocket) {
      CloseSocket(candidate);
      continue;
    }
    *rtp = candidate;
    *rtcp = control;
    *rtp_port = port;
    return true;
  }
  return false;
}

Socket TcpListener(std::uint16_t* port) {
  const Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener == kInvalidSocket)
    return kInvalidSocket;
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
             sizeof(one));
  const sockaddr_in address = Loopback(0);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, 1) != 0) {
    CloseSocket(listener);
    return kInvalidSocket;
  }
  *port = BoundPort(listener);
  return listener;
}

Socket ConnectTcp(std::uint16_t port) {
  const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket == kInvalidSocket)
    return kInvalidSocket;
  const sockaddr_in address = Loopback(port);
  if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    CloseSocket(socket);
    return kInvalidSocket;
  }
  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
             sizeof(one));
  return socket;
}

bool SendBytes(Socket socket, const std::uint8_t* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
#ifdef _WIN32
    const int result = send(socket, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(size - sent), 0);
#else
    const ssize_t result = send(socket, data + sent, size - sent, 0);
#endif
    if (result <= 0)
      return false;
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

bool SendFramed(Socket socket, const std::vector<std::uint8_t>& bytes) {
  std::vector<std::uint8_t> framed(2 + bytes.size());
  framed[0] = static_cast<std::uint8_t>(bytes.size() >> 8);
  framed[1] = static_cast<std::uint8_t>(bytes.size() & 0xFF);
  std::memcpy(framed.data() + 2, bytes.data(), bytes.size());
  return SendBytes(socket, framed.data(), framed.size());
}

bool WaitReadable(Socket socket, int timeout_ms) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  return select(static_cast<int>(socket) + 1, &read_set, nullptr, nullptr, &timeout) > 0;
}

// Returns the datagram size, or -1 on timeout/error.
int ReceiveUdp(Socket socket, int timeout_ms, std::vector<std::uint8_t>* out,
               sockaddr_in* from = nullptr) {
  if (!WaitReadable(socket, timeout_ms))
    return -1;
  std::uint8_t buffer[65535];
  sockaddr_in peer{};
#ifdef _WIN32
  int length = sizeof(peer);
#else
  socklen_t length = sizeof(peer);
#endif
  const int received = recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                reinterpret_cast<sockaddr*>(&peer), &length);
  if (received <= 0)
    return -1;
  out->assign(buffer, buffer + received);
  if (from != nullptr)
    *from = peer;
  return received;
}

bool ReadExact(Socket socket, int timeout_ms, std::uint8_t* data, std::size_t size) {
  std::size_t done = 0;
  while (done < size) {
    if (!WaitReadable(socket, timeout_ms))
      return false;
    const int received = recv(socket, reinterpret_cast<char*>(data + done),
                              static_cast<int>(size - done), 0);
    if (received <= 0)
      return false;
    done += static_cast<std::size_t>(received);
  }
  return true;
}

bool ReceiveTcpFrame(Socket socket, int timeout_ms, std::vector<std::uint8_t>* frame) {
  std::uint8_t header[2];
  if (!ReadExact(socket, timeout_ms, header, sizeof(header)))
    return false;
  const std::size_t length =
      (static_cast<std::size_t>(header[0]) << 8) | static_cast<std::size_t>(header[1]);
  frame->resize(length);
  return ReadExact(socket, timeout_ms, frame->data(), length);
}

bool ReceiveTcpRtp(Socket socket, int timeout_ms, RtpPacketHeader* header,
                   std::vector<std::uint8_t>* packet) {
  while (true) {
    if (!ReceiveTcpFrame(socket, timeout_ms, packet))
      return false;
    if (packet->size() >= 2 && packet->at(1) >= 200 && packet->at(1) <= 207)
      continue;  // RTCP interleaved on the same connection
    std::string error;
    return ParseRtpPacket(packet->data(), packet->size(), header, &error);
  }
}

std::string MakeOffer(const SdpVideoSession& session,
                      std::string_view direction = "recvonly") {
  SdpVideoSession offer = session;
  offer.origin_id = "34020000002000000001";
  offer.session_name = "Play";
  offer.payload_type = 96;
  offer.encoding = "PS";
  offer.clock_rate = 90000;
  offer.direction = std::string(direction);
  offer.has_ssrc = true;
  offer.ssrc = 0x0BADF00D;
  return BuildSdp(offer);
}

struct PlatformUdp {
  Socket rtp = kInvalidSocket;
  Socket rtcp = kInvalidSocket;
  std::uint16_t rtp_port = 0;

  ~PlatformUdp() {
    CloseSocket(rtp);
    CloseSocket(rtcp);
  }
};

struct TestCounters {
  int keyframe_requests = 0;
  int pli = 0;
  int fir = 0;
  std::uint8_t last_fir_sequence = 0;
};

MediaSessionConfig TestConfig(TestCounters* counters) {
  MediaSessionConfig config;
  config.device_id = "34020000001320000001";
  config.advertised_address = "127.0.0.1";
  config.bind_address = "127.0.0.1";
  config.ssrc_seed = 0x12340000;
  config.limits.rtp_port_min = 32100;
  config.limits.rtp_port_max = 32498;
  config.limits.enable_rtcp = true;
  config.limits.sender_report_interval_ms = 40;
  config.limits.ack_timeout_ms = 3000;
  config.limits.max_sessions = 4;
  config.on_keyframe_request = [counters](const std::string&, bool is_fir,
                                          std::uint8_t sequence) {
    ++counters->keyframe_requests;
    if (is_fir) {
      ++counters->fir;
      counters->last_fir_sequence = sequence;
    } else {
      ++counters->pli;
    }
  };
  return config;
}

std::vector<std::uint8_t> MuxFrame(PsVideoCodec codec, bool keyframe, std::size_t size,
                                   std::int64_t pts) {
  std::vector<std::uint8_t> elementary(size);
  for (std::size_t i = 0; i < size; ++i)
    elementary[i] = static_cast<std::uint8_t>(i & 0xFF);
  PsMuxerConfig muxer_config;
  muxer_config.codec = codec;
  PsMuxer muxer(muxer_config);
  std::vector<std::uint8_t> ps;
  std::string error;
  const PsFrame frame{elementary.data(), elementary.size(), pts, pts, false, keyframe};
  if (!muxer.MuxFrame(frame, &ps, &error)) {
    std::fprintf(stderr, "FAIL mux frame: %s\n", error.c_str());
    ++failures;
  }
  return ps;
}

void TestUdpInviteRtcpAndBye() {
  EnsureSockets();
  PlatformUdp platform;
  if (!BindUdpPair(&platform.rtp, &platform.rtcp, &platform.rtp_port)) {
    std::fprintf(stderr, "FAIL cannot bind platform UDP pair\n");
    ++failures;
    return;
  }

  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = platform.rtp_port;
  offer.mode = RtpTransportMode::kUdp;
  SdpVideoSession parsed_offer;
  ExpectBool("parse built UDP offer", ParseSdp(MakeOffer(offer), &parsed_offer, &error), true);

  MediaInviteRequest invite;
  invite.call_id = "udp-call-1";
  invite.cseq = "1 INVITE";
  invite.from = "<sip:34020000002000000001@3402000000>";
  invite.to = "<sip:34020000001320000001@3402000000>";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);

  MediaInviteResponse response;
  ExpectBool("UDP INVITE handled", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("UDP INVITE status", response.status_code, 200);
  ExpectU64("UDP answer mode", static_cast<int>(response.mode),
            static_cast<int>(RtpTransportMode::kUdp));
  ExpectU64("UDP answer RTCP port", response.rtcp_port,
            static_cast<std::uint64_t>(response.rtp_port + 1));

  SdpVideoSession answer;
  ExpectBool("parse answer SDP", ParseSdp(response.sdp, &answer, &error), true);
  ExpectU64("answer port", answer.port, response.rtp_port);
  ExpectU64("answer PS payload", answer.payload_type, 96);
  ExpectU64("answer clock", answer.clock_rate, 90000);
  ExpectU64("answer SSRC", answer.ssrc, response.ssrc);
  Expect("answer direction", answer.direction, "sendonly");
  Expect("answer address", answer.address, "127.0.0.1");
  std::printf("UDP answer: rtp=%u rtcp=%u ssrc=0x%08x\n", response.rtp_port,
              response.rtcp_port, response.ssrc);

  MediaSessionSnapshot snapshot;
  ExpectBool("snapshot after INVITE", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("state after INVITE", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kWaiting));

  // A retransmitted INVITE must not allocate a second socket or SSRC.
  MediaInviteResponse retransmit;
  ExpectBool("retransmitted INVITE", manager.OnInvite(invite, &retransmit, &error), true);
  ExpectU64("retransmit same SSRC", retransmit.ssrc, response.ssrc);
  ExpectU64("retransmit same port", retransmit.rtp_port, response.rtp_port);
  ExpectU64("retransmit single session", manager.session_count(), 1);

  ExpectBool("UDP ACK", manager.OnAck(invite.call_id, &error), true);
  ExpectBool("snapshot after ACK", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("state after ACK", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kStreaming));

  // Five access units: the key frame spans three RTP packets, the deltas one.
  const std::vector<std::uint8_t> key = MuxFrame(PsVideoCodec::kH264, true, 3000, 0);
  const std::vector<std::uint8_t> delta1 = MuxFrame(PsVideoCodec::kH264, false, 100, 3600);
  const std::vector<std::uint8_t> delta2 = MuxFrame(PsVideoCodec::kH264, false, 100, 7200);
  ExpectBool("send key frame",
             manager.SendFrame(invite.call_id, key.data(), key.size(), 0, true, &error),
             true);
  ExpectBool("send delta 1",
             manager.SendFrame(invite.call_id, delta1.data(), delta1.size(), 3600, false,
                               &error),
             true);
  ExpectBool("send delta 2",
             manager.SendFrame(invite.call_id, delta2.data(), delta2.size(), 7200, false,
                               &error),
             true);

  std::vector<RtpPacketHeader> headers;
  for (int i = 0; i < 5; ++i) {
    std::vector<std::uint8_t> packet;
    if (ReceiveUdp(platform.rtp, 1000, &packet) < 0) {
      std::fprintf(stderr, "FAIL missing RTP packet %d\n", i);
      ++failures;
      break;
    }
    RtpPacketHeader header;
    ExpectBool("parse received RTP",
               ParseRtpPacket(packet.data(), packet.size(), &header, &error), true);
    headers.push_back(header);
    ExpectU64("RTP payload type", header.payload_type, 96);
    ExpectU64("RTP SSRC", header.ssrc, response.ssrc);
    ExpectU64("RTP sequence", header.sequence, i);
  }
  if (headers.size() == 5) {
    const std::uint32_t timestamps[5] = {0, 0, 0, 3600, 7200};
    const bool markers[5] = {false, false, true, true, true};
    for (std::size_t i = 0; i < headers.size(); ++i) {
      ExpectU64("RTP timestamp " + std::to_string(i), headers[i].timestamp, timestamps[i]);
      ExpectBool("RTP marker " + std::to_string(i), headers[i].marker, markers[i]);
    }
    std::printf("UDP RTP: packets=%zu frames=3 seq=%u..%u ts=%u..%u marker=00111\n",
                headers.size(), headers.front().sequence, headers.back().sequence,
                headers.front().timestamp, headers.back().timestamp);
  }

  // Sender report: the first Tick sends it and the platform sees SR + SDES.
  ExpectBool("first sender report tick",
             manager.Tick(MonotonicMillis(), &error), true);
  std::vector<std::uint8_t> sr_datagram;
  const int sr_size = ReceiveUdp(platform.rtcp, 1000, &sr_datagram);
  ExpectBool("platform received SR", sr_size > 0, true);
  std::vector<RtcpPacket> sr_packets;
  if (sr_size > 0) {
    ExpectBool("parse SR compound",
               ParseRtcpCompound(sr_datagram.data(), sr_datagram.size(), &sr_packets, &error),
               true);
  }
  ExpectU64("SR compound packets", sr_packets.size(), 2);
  std::uint32_t compact_ntp = 0;
  if (!sr_packets.empty()) {
    const RtcpPacket& sr = sr_packets.front();
    ExpectU64("SR payload type", sr.payload_type, kRtcpPtSenderReport);
    ExpectU64("SR sender ssrc", sr.sender_ssrc, response.ssrc);
    ExpectU64("SR packet count", sr.sender_info.packet_count, 5);
    ExpectU64("SR octet count", sr.sender_info.octet_count,
              key.size() + delta1.size() + delta2.size());
    ExpectU64("SR rtp timestamp", sr.sender_info.rtp_timestamp, 7200);
    ExpectU64("SR CNAME present", sr_packets.size() > 1 ? sr_packets[1].cname.empty() : true,
              false);
    compact_ntp = RtcpCompactNtp(sr.sender_info.ntp_seconds, sr.sender_info.ntp_fraction);
    std::printf("UDP RTCP SR: ssrc=0x%08x packets=%u octets=%u rtp_ts=%u ntp=%08x\n",
                sr.sender_ssrc, sr.sender_info.packet_count, sr.sender_info.octet_count,
                sr.sender_info.rtp_timestamp, compact_ntp);
  }

  // Receiver report about our SSRC: loss counters and RTT from LSR/DLSR.
  RtcpReportBlock block;
  block.ssrc = response.ssrc;
  block.fraction_lost = 3;
  block.cumulative_lost = -2;
  block.extended_highest_sequence = 4;
  block.jitter = 17;
  block.last_sender_report = compact_ntp;
  block.delay_since_last_sender_report = 0;
  const std::vector<std::uint8_t> rr =
      BuildRtcpReceiverReport(0x0BADF00D, {block});
  const sockaddr_in manager_rtcp = Loopback(response.rtcp_port);
  ExpectBool("send RR to device",
             sendto(platform.rtcp, reinterpret_cast<const char*>(rr.data()),
                    static_cast<int>(rr.size()), 0,
                    reinterpret_cast<const sockaddr*>(&manager_rtcp),
                    sizeof(manager_rtcp)) > 0,
             true);
  ExpectBool("poll RR", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectBool("snapshot after RR", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("RR received count", snapshot.rtcp.receiver_reports_received, 1);
  ExpectU64("RR fraction lost", snapshot.rtcp.last_fraction_lost, 3);
  ExpectI64("RR cumulative lost", snapshot.rtcp.last_cumulative_lost, -2);
  ExpectU64("RR jitter", snapshot.rtcp.last_jitter, 17);
  std::printf("UDP RTCP RR: fraction_lost=%u cumulative_lost=%d jitter=%u rtt=%u ms "
              "has_rtt=%d\n",
              snapshot.rtcp.last_fraction_lost, snapshot.rtcp.last_cumulative_lost,
              snapshot.rtcp.last_jitter, snapshot.rtcp.last_rtt_ms,
              snapshot.rtcp.has_rtt ? 1 : 0);

  // PLI: the manager raises the callback; the pump consumes the request and
  // the next key frame is counted as honoured.
  const std::vector<std::uint8_t> pli = BuildRtcpPli(0x0BADF00D, response.ssrc);
  ExpectBool("send PLI",
             sendto(platform.rtcp, reinterpret_cast<const char*>(pli.data()),
                    static_cast<int>(pli.size()), 0,
                    reinterpret_cast<const sockaddr*>(&manager_rtcp),
                    sizeof(manager_rtcp)) > 0,
             true);
  ExpectBool("poll PLI", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectU64("PLI callback", counters.keyframe_requests, 1);
  ExpectU64("PLI count", counters.pli, 1);
  ExpectBool("consume keyframe request", manager.ConsumeKeyframeRequest(invite.call_id),
             true);
  const std::vector<std::uint8_t> key2 = MuxFrame(PsVideoCodec::kH264, true, 100, 10800);
  ExpectBool("send key frame after PLI",
             manager.SendFrame(invite.call_id, key2.data(), key2.size(), 10800, true,
                               &error),
             true);
  ExpectBool("snapshot after PLI", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("PLI honoured", snapshot.keyframe_requests_honored, 1);

  // FIR carries a sequence number in its FCI.
  const std::vector<std::uint8_t> fir = BuildRtcpFir(0x0BADF00D, response.ssrc, 9);
  ExpectBool("send FIR",
             sendto(platform.rtcp, reinterpret_cast<const char*>(fir.data()),
                    static_cast<int>(fir.size()), 0,
                    reinterpret_cast<const sockaddr*>(&manager_rtcp),
                    sizeof(manager_rtcp)) > 0,
             true);
  ExpectBool("poll FIR", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectU64("FIR callback", counters.keyframe_requests, 2);
  ExpectU64("FIR count", counters.fir, 1);
  ExpectU64("FIR sequence", counters.last_fir_sequence, 9);
  ExpectBool("consume FIR request", manager.ConsumeKeyframeRequest(invite.call_id), true);
  const std::vector<std::uint8_t> key3 = MuxFrame(PsVideoCodec::kH264, true, 100, 14400);
  ExpectBool("send key frame after FIR",
             manager.SendFrame(invite.call_id, key3.data(), key3.size(), 14400, true,
                               &error),
             true);

  // RTCP BYE from the platform closes the media session.
  const std::vector<std::uint8_t> bye = BuildRtcpBye({response.ssrc}, "platform bye");
  ExpectBool("send RTCP BYE",
             sendto(platform.rtcp, reinterpret_cast<const char*>(bye.data()),
                    static_cast<int>(bye.size()), 0,
                    reinterpret_cast<const sockaddr*>(&manager_rtcp),
                    sizeof(manager_rtcp)) > 0,
             true);
  ExpectBool("poll BYE", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectBool("snapshot after BYE", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("state after RTCP BYE", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kClosed));
  ExpectBool("send after BYE must fail",
             manager.SendFrame(invite.call_id, key.data(), key.size(), 18000, true, &error),
             false);
}

void TestSipByeSendsRtcp() {
  EnsureSockets();
  PlatformUdp platform;
  if (!BindUdpPair(&platform.rtp, &platform.rtcp, &platform.rtp_port)) {
    std::fprintf(stderr, "FAIL cannot bind platform UDP pair\n");
    ++failures;
    return;
  }
  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = platform.rtp_port;
  offer.mode = RtpTransportMode::kUdp;

  MediaInviteRequest invite;
  invite.call_id = "udp-bye-1";
  invite.cseq = "7 INVITE";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);
  MediaInviteResponse response;
  ExpectBool("SIP BYE INVITE", manager.OnInvite(invite, &response, &error), true);
  MediaInviteResponse sip_response;
  ExpectBool("SIP BYE ACK via adapter",
             manager.HandleSipRequest("ACK", invite, &sip_response, &error), true);
  const std::vector<std::uint8_t> ps = MuxFrame(PsVideoCodec::kH264, true, 100, 0);
  ExpectBool("SIP BYE frame",
             manager.SendFrame(invite.call_id, ps.data(), ps.size(), 0, true, &error), true);
  std::vector<std::uint8_t> packet;
  ExpectBool("SIP BYE platform got RTP", ReceiveUdp(platform.rtp, 500, &packet) > 0, true);

  ExpectBool("SIP BYE handled via adapter",
             manager.HandleSipRequest("BYE", invite, &sip_response, &error), true);
  MediaSessionSnapshot snapshot;
  ExpectBool("snapshot after SIP BYE", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("state after SIP BYE", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kClosed));

  bool saw_bye = false;
  for (int attempt = 0; attempt < 6 && !saw_bye; ++attempt) {
    std::vector<std::uint8_t> datagram;
    if (ReceiveUdp(platform.rtcp, 300, &datagram) <= 0)
      break;
    std::vector<RtcpPacket> packets;
    if (ParseRtcpCompound(datagram.data(), datagram.size(), &packets, &error)) {
      for (const RtcpPacket& packet : packets) {
        if (packet.payload_type == kRtcpPtBye) {
          saw_bye = !packet.ssrcs.empty() && packet.ssrcs[0] == response.ssrc;
          std::printf("SIP BYE -> RTCP BYE: ssrc=0x%08x reason='%s'\n", packet.ssrcs.empty() ? 0 : packet.ssrcs[0],
                      packet.reason.c_str());
        }
      }
    }
  }
  ExpectBool("SIP BYE sends RTCP BYE", saw_bye, true);
}

void TestTcpPassive() {
  EnsureSockets();
  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = 6000;  // the platform's port; the device ignores it for passive
  offer.mode = RtpTransportMode::kTcpActive;  // offerer will connect
  offer.setup = RtpSetup::kActive;
  offer.connection_new = true;

  MediaInviteRequest invite;
  invite.call_id = "tcp-passive-1";
  invite.cseq = "1 INVITE";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);
  MediaInviteResponse response;
  ExpectBool("TCP passive INVITE", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("TCP passive answer mode", static_cast<int>(response.mode),
            static_cast<int>(RtpTransportMode::kTcpPassive));
  ExpectU64("TCP passive setup", static_cast<int>(response.setup),
            static_cast<int>(RtpSetup::kPassive));
  ExpectU64("TCP passive no separate RTCP port", response.rtcp_port, 0);

  // ACK can arrive before the platform's TCP connection; Tick completes it.
  ExpectBool("TCP passive ACK", manager.OnAck(invite.call_id, &error), true);
  const Socket media = ConnectTcp(response.rtp_port);
  ExpectBool("platform connects to TCP passive port", media != kInvalidSocket, true);
  if (media == kInvalidSocket)
    return;
  ExpectBool("TCP passive tick accepts", manager.Tick(MonotonicMillis(), &error), true);
  MediaSessionSnapshot snapshot;
  manager.Snapshot(invite.call_id, &snapshot);
  ExpectU64("TCP passive state", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kStreaming));

  const std::vector<std::uint8_t> ps = MuxFrame(PsVideoCodec::kH264, true, 1500, 0);
  ExpectBool("TCP passive send",
             manager.SendFrame(invite.call_id, ps.data(), ps.size(), 0, true, &error), true);
  RtpPacketHeader header;
  std::vector<std::uint8_t> packet;
  ExpectBool("TCP passive receive", ReceiveTcpRtp(media, 1000, &header, &packet), true);
  ExpectU64("TCP passive PT", header.payload_type, 96);
  ExpectU64("TCP passive SSRC", header.ssrc, response.ssrc);

  const std::vector<std::uint8_t> pli = BuildRtcpPli(0x0BADF00D, response.ssrc);
  ExpectBool("TCP passive send framed PLI", SendFramed(media, pli), true);
  ExpectBool("TCP passive poll", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectU64("TCP passive PLI callback", counters.keyframe_requests, 1);
  std::printf("TCP passive: rtp_port=%u round-trip PLI ok\n", response.rtp_port);

  ExpectBool("TCP passive close",
             manager.Close(invite.call_id, "test done", true, &error), true);
  bool saw_bye = false;
  for (int attempt = 0; attempt < 4 && !saw_bye; ++attempt) {
    std::vector<std::uint8_t> frame;
    if (!ReceiveTcpFrame(media, 300, &frame))
      break;
    std::vector<RtcpPacket> packets;
    if (ParseRtcpCompound(frame.data(), frame.size(), &packets, &error)) {
      for (const RtcpPacket& packet : packets) {
        if (packet.payload_type == kRtcpPtBye)
          saw_bye = true;
      }
    }
  }
  ExpectBool("TCP passive RTCP BYE framed", saw_bye, true);
  CloseSocket(media);
}

void TestTcpActive() {
  EnsureSockets();
  std::uint16_t listener_port = 0;
  const Socket listener = TcpListener(&listener_port);
  ExpectBool("TCP active listener", listener != kInvalidSocket, true);
  if (listener == kInvalidSocket)
    return;

  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = listener_port;
  offer.mode = RtpTransportMode::kTcpPassive;  // offerer will listen
  offer.setup = RtpSetup::kPassive;
  offer.connection_new = true;

  MediaInviteRequest invite;
  invite.call_id = "tcp-active-1";
  invite.cseq = "1 INVITE";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);
  MediaInviteResponse response;
  ExpectBool("TCP active INVITE", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("TCP active answer mode", static_cast<int>(response.mode),
            static_cast<int>(RtpTransportMode::kTcpActive));
  ExpectU64("TCP active setup", static_cast<int>(response.setup),
            static_cast<int>(RtpSetup::kActive));

  const bool device_connected = WaitReadable(listener, 2000);
  ExpectBool("platform accepts device TCP connection", device_connected, true);
  if (!device_connected) {
    CloseSocket(listener);
    return;
  }
  sockaddr_in peer{};
#ifdef _WIN32
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const Socket media =
      accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
  ExpectBool("platform accepts device TCP connection", media != kInvalidSocket, true);
  if (media == kInvalidSocket)
    return;

  ExpectBool("TCP active ACK", manager.OnAck(invite.call_id, &error), true);
  const std::vector<std::uint8_t> ps = MuxFrame(PsVideoCodec::kH264, false, 700, 0);
  ExpectBool("TCP active send",
             manager.SendFrame(invite.call_id, ps.data(), ps.size(), 0, false, &error), true);
  RtpPacketHeader header;
  std::vector<std::uint8_t> packet;
  ExpectBool("TCP active receive", ReceiveTcpRtp(media, 1000, &header, &packet), true);
  ExpectU64("TCP active PT", header.payload_type, 96);
  ExpectU64("TCP active SSRC", header.ssrc, response.ssrc);
  ExpectBool("TCP active marker", header.marker, true);

  const std::vector<std::uint8_t> fir = BuildRtcpFir(0x0BADF00D, response.ssrc, 3);
  ExpectBool("TCP active send framed FIR", SendFramed(media, fir), true);
  ExpectBool("TCP active poll", manager.PollControl(invite.call_id, 500, &error), true);
  ExpectU64("TCP active FIR callback", counters.keyframe_requests, 1);
  ExpectU64("TCP active FIR flag", counters.fir, 1);
  ExpectU64("TCP active FIR sequence", counters.last_fir_sequence, 3);
  std::printf("TCP active: connected to 127.0.0.1:%u, FIR sequence=%u\n", listener_port,
              counters.last_fir_sequence);
  CloseSocket(media);
  CloseSocket(listener);
}

void TestTimestampContinuity() {
  EnsureSockets();
  PlatformUdp platform;
  if (!BindUdpPair(&platform.rtp, &platform.rtcp, &platform.rtp_port)) {
    std::fprintf(stderr, "FAIL cannot bind platform UDP pair\n");
    ++failures;
    return;
  }
  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = platform.rtp_port;
  offer.mode = RtpTransportMode::kUdp;
  MediaInviteRequest invite;
  invite.call_id = "continuity-1";
  invite.cseq = "1 INVITE";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);
  MediaInviteResponse response;
  ExpectBool("continuity INVITE", manager.OnInvite(invite, &response, &error), true);
  ExpectBool("continuity ACK", manager.OnAck(invite.call_id, &error), true);

  const int frame_count = 25;
  const std::uint32_t delta = 3600;  // 25 fps at 90 kHz
  for (int i = 0; i < frame_count; ++i) {
    const bool keyframe = i == 0 || i == 12;
    const std::vector<std::uint8_t> ps =
        MuxFrame(PsVideoCodec::kH264, keyframe, 900, static_cast<std::int64_t>(i) * delta);
    ExpectBool("continuity send " + std::to_string(i),
               manager.SendFrame(invite.call_id, ps.data(), ps.size(),
                                 static_cast<std::uint32_t>(i) * delta, keyframe, &error),
               true);
  }

  std::vector<RtpPacketHeader> headers;
  for (int i = 0; i < frame_count; ++i) {
    std::vector<std::uint8_t> packet;
    if (ReceiveUdp(platform.rtp, 1000, &packet) < 0) {
      std::fprintf(stderr, "FAIL continuity missing packet %d\n", i);
      ++failures;
      break;
    }
    RtpPacketHeader header;
    if (!ParseRtpPacket(packet.data(), packet.size(), &header, &error)) {
      std::fprintf(stderr, "FAIL continuity parse %d\n", i);
      ++failures;
      break;
    }
    headers.push_back(header);
  }
  ExpectU64("continuity packets", headers.size(), frame_count);
  for (std::size_t i = 0; i < headers.size(); ++i) {
    ExpectU64("continuity seq " + std::to_string(i), headers[i].sequence, i);
    ExpectU64("continuity ts " + std::to_string(i), headers[i].timestamp, i * delta);
    ExpectBool("continuity marker " + std::to_string(i), headers[i].marker, true);
    if (i > 0)
      ExpectU64("continuity delta " + std::to_string(i),
                headers[i].timestamp - headers[i - 1].timestamp, delta);
  }
  if (!headers.empty()) {
    std::printf("continuity: frames=%d packets=%zu seq=%u..%u ts=%u..%u delta=%u "
                "ssrc=0x%08x\n",
                frame_count, headers.size(), headers.front().sequence,
                headers.back().sequence, headers.front().timestamp,
                headers.back().timestamp, delta, response.ssrc);
  }
}


void TestAckTimeout() {
  EnsureSockets();
  PlatformUdp platform;
  if (!BindUdpPair(&platform.rtp, &platform.rtcp, &platform.rtp_port)) {
    std::fprintf(stderr, "FAIL cannot bind platform UDP pair\n");
    ++failures;
    return;
  }
  TestCounters counters;
  MediaSessionConfig config = TestConfig(&counters);
  config.limits.ack_timeout_ms = 100;
  MediaSessionManager manager(config);
  std::string error;

  SdpVideoSession offer;
  offer.address = "127.0.0.1";
  offer.port = platform.rtp_port;
  offer.mode = RtpTransportMode::kUdp;
  MediaInviteRequest invite;
  invite.call_id = "ack-timeout-1";
  invite.cseq = "1 INVITE";
  invite.content_type = "application/sdp";
  invite.body = MakeOffer(offer);
  MediaInviteResponse response;
  ExpectBool("ACK timeout INVITE", manager.OnInvite(invite, &response, &error), true);
  ExpectBool("ACK timeout: no ACK, Tick closes",
             manager.Tick(MonotonicMillis() + 1000, &error), true);
  MediaSessionSnapshot snapshot;
  ExpectBool("ACK timeout snapshot", manager.Snapshot(invite.call_id, &snapshot), true);
  ExpectU64("ACK timeout state", static_cast<int>(snapshot.state),
            static_cast<int>(MediaSessionState::kClosed));
  const std::vector<std::uint8_t> ps = MuxFrame(PsVideoCodec::kH264, true, 100, 0);
  ExpectBool("ACK timeout blocks frames",
             manager.SendFrame(invite.call_id, ps.data(), ps.size(), 0, true, &error),
             false);
  std::printf("ACK timeout: dialog closed after %u ms without ACK\n",
              config.limits.ack_timeout_ms);
}

void TestRejections() {
  EnsureSockets();
  TestCounters counters;
  MediaSessionManager manager(TestConfig(&counters));
  std::string error;

  MediaInviteRequest invite;
  MediaInviteResponse response;
  invite.content_type = "application/sdp";

  ExpectBool("missing Call-ID is handled", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("missing Call-ID status", response.status_code, 400);

  invite.call_id = "bad-1";
  ExpectBool("missing SDP is handled", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("missing SDP status", response.status_code, 488);

  invite.body = MakeOffer([] {
    SdpVideoSession offer;
    offer.address = "127.0.0.1";
    offer.port = 5004;
    offer.mode = RtpTransportMode::kUdp;
    return offer;
  }());
  invite.content_type = "application/json";
  ExpectBool("wrong content type is handled", manager.OnInvite(invite, &response, &error),
             true);
  ExpectU64("wrong content type status", response.status_code, 415);

  invite.content_type = "application/sdp";
  invite.body =
      "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=x\r\nc=IN IP4 127.0.0.1\r\n"
      "t=0 0\r\nm=video 5004 RTP/AVP 97\r\na=rtpmap:97 H264/90000\r\n";
  ExpectBool("non-PS SDP is handled", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("non-PS SDP status", response.status_code, 488);

  SdpVideoSession sendonly;
  sendonly.address = "127.0.0.1";
  sendonly.port = 5004;
  sendonly.mode = RtpTransportMode::kUdp;
  sendonly.direction = "sendonly";
  invite.body = MakeOffer(sendonly, "sendonly");
  ExpectBool("sendonly offer is handled", manager.OnInvite(invite, &response, &error), true);
  ExpectU64("sendonly offer status", response.status_code, 488);

  ExpectBool("unknown SIP method",
             manager.HandleSipRequest("OPTIONS", invite, &response, &error), false);
  ExpectU64("unknown SIP method status", response.status_code, 405);
}

}  // namespace

int main() {
  TestUdpInviteRtcpAndBye();
  TestSipByeSendsRtcp();
  TestTcpPassive();
  TestTcpActive();
  TestTimestampContinuity();
  TestAckTimeout();
  TestRejections();

  if (failures == 0) {
    std::printf("gb28181_media_session_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb28181_media_session_test: %d checks failed\n", failures);
  return 1;
}
