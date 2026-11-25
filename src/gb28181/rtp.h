#pragma once

// RTP carrying GB/T 28181 program streams (P6.2c).
//
// The RTP payload is the PS byte stream from ps.h. Every access-unit PS pack
// is split into RTP packets of at most `max_payload_size` bytes; the last
// packet of the frame carries the RTP marker bit. The timestamp is the frame
// PTS in the 90 kHz MPEG clock, and every frame uses the same SSRC.
//
// The module has two layers:
//   * RtpPacketizer: pure, no sockets; used by the unit tests;
//   * RtpSession:    UDP, TCP active and TCP passive (the GB/T 28181 device
//                    side is usually passive) plus SDP build/parse for INVITE.
//
// TCP packets are length-prefixed with a two-byte big-endian size (RFC 4571),
// which is what GB/T 28181 platforms expect for TCP media.
//
// This file is compiled only with UVNVR_ENABLE_GB28181=ON (default off).

#include "gb28181/rtcp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb28181 {

struct RtpPacketHeader {
  bool marker = false;
  std::uint8_t payload_type = 0;
  std::uint16_t sequence = 0;
  std::uint32_t timestamp = 0;
  std::uint32_t ssrc = 0;
  std::size_t header_size = 0;
  std::size_t payload_size = 0;
};

bool ParseRtpPacket(const std::uint8_t* data, std::size_t size, RtpPacketHeader* header,
                    std::string* error = nullptr);

struct RtpPacketizerConfig {
  std::uint8_t payload_type = 96;
  std::uint32_t ssrc = 0;
  std::uint16_t initial_sequence = 0;
  std::size_t max_payload_size = 1400;
};

// Packetizes one PS frame. Each output vector is a complete RTP packet
// (12-byte header + payload). Marker is set on the final packet only.
class RtpPacketizer {
 public:
  explicit RtpPacketizer(RtpPacketizerConfig config = {});

  void Reset();
  const RtpPacketizerConfig& config() const { return config_; }

  bool PacketizeFrame(const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp_90k,
                      std::vector<std::vector<std::uint8_t>>* packets,
                      std::string* error = nullptr);

  std::uint16_t next_sequence() const { return next_sequence_; }
  std::uint64_t packets_sent() const { return packets_sent_; }

 private:
  RtpPacketizerConfig config_;
  std::uint16_t next_sequence_ = 0;
  std::uint64_t packets_sent_ = 0;
};

enum class RtpTransportMode {
  kUdp,
  kTcpActive,   // the device connects to the platform
  kTcpPassive,  // the device listens and the platform connects
};

enum class RtpSetup {
  kNone,  // UDP: no a=setup line
  kActive,
  kPassive,
};

const char* RtpSetupName(RtpSetup setup);
bool ParseRtpSetup(std::string_view text, RtpSetup* setup);

// SDP that a GB/T 28181 INVITE or 200 OK carries for one video stream.
struct SdpVideoSession {
  std::string address;       // c=IN IP4 <address>
  std::uint16_t port = 0;    // m=video <port>
  std::string origin_id;     // o= first token (the 20-digit GB ID)
  std::string session_name = "Play";
  RtpTransportMode mode = RtpTransportMode::kUdp;
  RtpSetup setup = RtpSetup::kNone;
  bool connection_new = false;
  std::uint8_t payload_type = 96;
  std::string encoding = "PS";
  std::uint32_t clock_rate = 90000;
  bool has_ssrc = false;
  std::uint32_t ssrc = 0;
  std::string direction = "sendonly";  // sendonly / recvonly / sendrecv
};

// Builds CRLF-terminated SDP:
//
//   v=0
//   o=<origin_id> 0 0 IN IP4 <address>
//   s=<session_name>
//   c=IN IP4 <address>
//   t=0 0
//   m=video <port> RTP/AVP 96
//   a=sendonly
//   a=rtpmap:96 PS/90000
//   y=<ssrc as 10 decimal digits>
//
// TCP sessions use "TCP/RTP/AVP" and add a=setup:passive|active and
// a=connection:new. If `setup` is kNone for a TCP mode, the mode's natural
// setup is used (passive for kTcpPassive, active for kTcpActive).
std::string BuildSdp(const SdpVideoSession& session);

bool ParseSdp(std::string_view sdp, SdpVideoSession* session, std::string* error = nullptr);

// Builds the device's answer to a platform INVITE. The direction flips to
// sendonly and the setup role is mirrored (a platform that offers active is
// answered passive, and vice versa), which is the negotiation used by GB/T
// 28181 TCP media.
bool BuildSdpAnswer(const SdpVideoSession& offer, const std::string& local_address,
                    std::uint16_t local_port, const std::string& local_id, std::uint32_t ssrc,
                    std::string* sdp, SdpVideoSession* answer = nullptr,
                    std::string* error = nullptr);

struct RtpSessionConfig {
  RtpTransportMode mode = RtpTransportMode::kUdp;
  // Local bind address. "0.0.0.0" listens on all IPv4 interfaces.
  std::string local_address = "0.0.0.0";
  std::uint16_t local_port = 0;  // 0 = ephemeral / OS-assigned
  // Destination for UDP and kTcpActive. kTcpPassive learns it from accept().
  std::string remote_address;
  std::uint16_t remote_port = 0;
  std::uint8_t payload_type = 96;
  std::uint32_t ssrc = 0;
  std::uint16_t initial_sequence = 0;
  std::size_t max_payload_size = 1400;

  // RTCP is opt-in so the PS/RTP-only path keeps its exact socket behaviour.
  // For UDP the sender opens a second socket; the platform receives SR/SDES
  // there and sends RR/PLI/FIR to it. For TCP, RTCP is framed on the same
  // media connection (RFC 4571), so no extra port is used.
  bool enable_rtcp = false;
  // 0 = RTP local port + 1 (RFC 3550 convention).
  std::uint16_t rtcp_local_port = 0;
  // 0 = remote RTP port + 1.
  std::uint16_t rtcp_remote_port = 0;
  std::string rtcp_cname;  // defaults to "uvnvr@<local address>"
  std::uint32_t sender_report_interval_ms = 5000;
};

struct RtpSendStats {
  std::uint64_t frames = 0;
  std::uint64_t packets = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t marker_packets = 0;
  std::uint16_t last_sequence = 0;
  std::uint32_t last_timestamp = 0;
};

// Owns one media socket. Open() is non-blocking for kTcpPassive: it only
// creates the listener, and Accept() waits for the platform connection. The
// UDP and TCP active modes complete their socket setup in Open().
class RtpSession {
 public:
  explicit RtpSession(RtpSessionConfig config);
  ~RtpSession();

  RtpSession(const RtpSession&) = delete;
  RtpSession& operator=(const RtpSession&) = delete;

  bool Open(std::string* error = nullptr);
  bool Accept(std::string* peer, int timeout_ms, std::string* error = nullptr);
  void Close();

  bool SendFrame(const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp_90k,
                 bool keyframe, std::string* error = nullptr);

  // RTCP. SetRtcpEventCallback receives every parsed inbound packet; the
  // callback runs on the calling thread inside PollRtcp(). Tick() emits a
  // sender report when the interval elapsed and is cheap to call often.
  // `now_ms` is a monotonic millisecond clock supplied by the caller; the SR
  // NTP field itself uses the system clock.
  void SetRtcpEventCallback(std::function<void(const RtcpPacket&)> callback);
  bool SendSenderReport(std::string* error = nullptr);
  bool SendBye(std::string_view reason = {}, std::string* error = nullptr);
  bool PollRtcp(int timeout_ms, std::string* error = nullptr);
  bool Tick(int64_t now_ms, std::string* error = nullptr);

  const RtcpStats& rtcp_stats() const;
  std::uint16_t rtcp_local_port() const;
  std::string rtcp_peer_address() const;

  bool is_open() const;
  std::uint16_t local_port() const;
  std::string local_address() const;
  std::string peer_address() const;

  const RtpSessionConfig& config() const { return config_; }
  const RtpSendStats& stats() const { return stats_; }

  // Convenience SDP for this session. When `local_port()` is 0 (before Open)
  // the configured port is used. `advertise_address` is the address the
  // platform should route to; it is separate from the bind address.
  std::string BuildSessionSdp(const std::string& advertise_address,
                              const std::string& origin_id,
                              const std::string& session_name = "Play") const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  RtpSessionConfig config_;
  RtpSendStats stats_;
};

}  // namespace uvnvr::gb28181
