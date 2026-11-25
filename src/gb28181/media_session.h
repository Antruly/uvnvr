#pragma once

// GB/T 28181 real-time media dialog manager (P6.2c media half).
//
// This is the binding between SIP INVITE/ACK/BYE and the RTP/RTCP session:
//
//   INVITE (offer SDP) -> parse the offer, open UDP / TCP active / TCP passive,
//                         return the 200 OK SDP answer and the SSRC;
//   ACK                -> mark the dialog streaming (and accept a passive TCP
//                         connection when the platform connects after 200 OK);
//   BYE                -> send RTCP BYE, close the socket and stop the stream.
//
// The class does not parse SIP itself. A SIP user agent feeds it the Call-ID,
// CSeq, content type and SDP body; `HandleSipRequest` is the small adapter for
// INVITE/ACK/BYE. All methods run on one thread.
//
// RTCP events (Receiver Reports, PLI/FIR key-frame requests, BYE) are delivered
// through callbacks. A key-frame request is advisory: the caller must make its
// encoder/source produce an IDR. `ConsumeKeyframeRequest` lets a pumping loop
// notice the request and skip ahead to the next key frame when it can.
//
// Compiled only with UVNVR_ENABLE_GB28181=ON (default off).

#include "gb28181/ps.h"
#include "gb28181/rtcp.h"
#include "gb28181/rtp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb28181 {

enum class MediaSessionState {
  kIdle,
  kWaiting,    // 200 OK sent, waiting for ACK / the TCP connection
  kStreaming,  // ACK received, frames may be sent
  kClosed,
};

const char* MediaSessionStateName(MediaSessionState state);

struct MediaSessionLimits {
  std::uint16_t rtp_port_min = 30000;
  std::uint16_t rtp_port_max = 30998;
  bool enable_rtcp = true;
  std::uint32_t sender_report_interval_ms = 5000;
  // A dialog that never receives ACK is closed after this long.
  std::uint32_t ack_timeout_ms = 32000;
  // How long OnAck waits inline for a passive TCP connection. 0 means a
  // non-blocking accept; Tick() retries while the dialog is waiting.
  std::uint32_t tcp_accept_timeout_ms = 0;
  std::size_t max_sessions = 8;
};

struct MediaSessionConfig {
  // Local 20-digit GB/T 28181 device ID, used as SDP o= and as the default
  // CNAME suffix.
  std::string device_id;
  // Address placed in the answer SDP. Empty means bind_address, or 127.0.0.1
  // when the bind address is a wildcard.
  std::string advertised_address;
  std::string bind_address = "0.0.0.0";
  // Base SSRC; 0 derives a stable nonzero value from the Call-ID.
  std::uint32_t ssrc_seed = 0;
  MediaSessionLimits limits;

  std::function<void(const std::string& call_id, MediaSessionState state,
                     const std::string& detail)>
      on_state;
  std::function<void(const std::string& call_id, const RtcpPacket& packet)> on_rtcp;
  // `fir_sequence` is meaningful only when `is_fir` is true.
  std::function<void(const std::string& call_id, bool is_fir,
                     std::uint8_t fir_sequence)>
      on_keyframe_request;
  std::function<void(const std::string&)> on_log;
};

struct MediaInviteRequest {
  std::string call_id;
  std::string cseq;  // e.g. "1 INVITE"; retransmissions reuse it
  std::string from;
  std::string to;
  std::string content_type;  // application/sdp; empty is treated as SDP
  std::string body;          // SDP offer
};

struct MediaInviteResponse {
  int status_code = 200;
  std::string reason = "OK";
  std::string sdp;
  RtpTransportMode mode = RtpTransportMode::kUdp;
  RtpSetup setup = RtpSetup::kNone;
  std::uint16_t rtp_port = 0;
  std::uint16_t rtcp_port = 0;  // 0 for TCP (RTCP shares the media connection)
  std::uint32_t ssrc = 0;
};

struct MediaSessionSnapshot {
  MediaSessionState state = MediaSessionState::kIdle;
  std::string call_id;
  std::string cseq;
  RtpTransportMode mode = RtpTransportMode::kUdp;
  RtpSetup setup = RtpSetup::kNone;
  std::string peer_address;
  std::uint16_t peer_rtp_port = 0;
  std::uint16_t local_rtp_port = 0;
  std::uint16_t local_rtcp_port = 0;
  std::uint32_t ssrc = 0;

  std::uint64_t frames = 0;
  std::uint64_t packets = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t keyframes = 0;
  std::uint64_t keyframe_requests = 0;
  std::uint64_t keyframe_requests_honored = 0;
  std::uint16_t last_sequence = 0;
  std::uint32_t last_timestamp = 0;

  RtcpStats rtcp;
  bool keyframe_request_pending = false;
  std::int64_t created_at_ms = 0;
  std::int64_t last_frame_at_ms = 0;
};

class MediaSessionManager {
 public:
  explicit MediaSessionManager(MediaSessionConfig config);
  ~MediaSessionManager();

  MediaSessionManager(const MediaSessionManager&) = delete;
  MediaSessionManager& operator=(const MediaSessionManager&) = delete;

  // Handles an INVITE offer. A retransmission with the same Call-ID and CSeq
  // returns the cached 200 OK without opening a second socket. A re-INVITE
  // (same Call-ID, different CSeq) replaces the old media session.
  bool OnInvite(const MediaInviteRequest& request, MediaInviteResponse* response,
                std::string* error = nullptr);

  // ACK moves the dialog to streaming. For TCP passive media this also tries
  // to accept the platform connection; Tick() keeps trying if it is not there
  // yet.
  bool OnAck(const std::string& call_id, std::string* error = nullptr);

  // BYE sends RTCP BYE, closes the media socket and keeps the closed snapshot
  // available for diagnostics until the Call-ID is reused.
  bool OnBye(const std::string& call_id, std::string* error = nullptr);

  // SIP UA adapter: method is "INVITE", "ACK" or "BYE" (case-insensitive).
  // For INVITE, `request.body` carries the SDP offer. Other methods return
  // false with an explicit error, so the SIP layer can answer 405.
  bool HandleSipRequest(std::string_view method, const MediaInviteRequest& request,
                        MediaInviteResponse* response, std::string* error = nullptr);

  // Sends one PS access unit. The dialog must be streaming; for TCP passive
  // the platform connection must have been accepted.
  bool SendFrame(const std::string& call_id, const std::uint8_t* ps, std::size_t size,
                 std::uint32_t timestamp_90k, bool keyframe,
                 std::string* error = nullptr);

  // Drains RTCP for one dialog (or all dialogs when call_id is empty) and
  // dispatches the callbacks. A timeout is not an error.
  bool PollControl(const std::string& call_id, int timeout_ms,
                   std::string* error = nullptr);

  // Periodic work: sender reports and ACK/TCP-accept timeouts. `now_ms` is a
  // monotonic clock (std::chrono::steady_clock is fine).
  bool Tick(std::int64_t now_ms, std::string* error = nullptr);

  // True once if a platform PLI/FIR arrived and the caller has not consumed
  // it yet. The frame pump uses this to jump to the next key frame.
  bool ConsumeKeyframeRequest(const std::string& call_id);

  // Closes a dialog. `send_rtcp_bye` should be true when the device leaves an
  // active session; false for a failed INVITE.
  bool Close(const std::string& call_id, std::string_view reason,
             bool send_rtcp_bye, std::string* error = nullptr);

  std::size_t session_count() const;
  std::size_t active_session_count() const;
  std::vector<std::string> call_ids() const;
  bool Snapshot(const std::string& call_id, MediaSessionSnapshot* snapshot) const;
  const MediaSessionConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvnvr::gb28181
