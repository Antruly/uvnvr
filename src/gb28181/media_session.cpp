// GB/T 28181 media dialog manager: SDP negotiation, RTP/RTCP and INVITE/BYE.

#include "gb28181/media_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace uvnvr::gb28181 {
namespace {

std::string ToLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
  return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::int64_t MonotonicMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::uint32_t HashCallId(std::string_view text) {
  // FNV-1a, then force a nonzero SSRC with the low bit set so it is easy to
  // recognise in captures.
  std::uint32_t hash = 2166136261u;
  for (char c : text) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= 16777619u;
  }
  hash |= 1u;
  return hash == 0 ? 1u : hash;
}

RtpTransportMode DeviceModeForOffer(const SdpVideoSession& offer, RtpSetup* setup) {
  if (offer.mode == RtpTransportMode::kUdp) {
    *setup = RtpSetup::kNone;
    return RtpTransportMode::kUdp;
  }
  if (offer.setup == RtpSetup::kPassive) {
    *setup = RtpSetup::kActive;
    return RtpTransportMode::kTcpActive;
  }
  // active or unspecified: the device listens, as RFC 4145's default answer.
  *setup = RtpSetup::kPassive;
  return RtpTransportMode::kTcpPassive;
}

const char* TransportName(RtpTransportMode mode) {
  switch (mode) {
    case RtpTransportMode::kUdp:
      return "udp";
    case RtpTransportMode::kTcpActive:
      return "tcp-active";
    case RtpTransportMode::kTcpPassive:
      return "tcp-passive";
  }
  return "unknown";
}

}  // namespace

const char* MediaSessionStateName(MediaSessionState state) {
  switch (state) {
    case MediaSessionState::kIdle:
      return "idle";
    case MediaSessionState::kWaiting:
      return "waiting";
    case MediaSessionState::kStreaming:
      return "streaming";
    case MediaSessionState::kClosed:
      return "closed";
  }
  return "unknown";
}

struct MediaSessionManager::Impl {
  struct Session {
    MediaSessionState state = MediaSessionState::kIdle;
    std::string call_id;
    std::string cseq;
    std::string offer_body;
    MediaInviteResponse response;
    SdpVideoSession offer;
    SdpVideoSession answer;

    std::unique_ptr<RtpSession> rtp;
    std::uint32_t ssrc = 0;
    std::uint16_t peer_rtp_port = 0;
    std::uint16_t local_rtp_port = 0;
    std::uint16_t local_rtcp_port = 0;

    bool ack_received = false;
    bool keyframe_request_pending = false;
    bool waiting_for_keyframe = false;
    std::uint64_t keyframe_requests = 0;
    std::uint64_t keyframe_requests_honored = 0;

    std::int64_t created_at_ms = 0;
    std::int64_t last_frame_at_ms = 0;
  };

  explicit Impl(MediaSessionConfig config_in) : config(std::move(config_in)) {
    if (config.limits.rtp_port_min % 2 != 0 && config.limits.rtp_port_min != 0)
      ++config.limits.rtp_port_min;
    if (config.limits.rtp_port_max < config.limits.rtp_port_min)
      config.limits.rtp_port_max = config.limits.rtp_port_min;
    next_port = config.limits.rtp_port_min;
  }

  MediaSessionConfig config;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::set<std::uint32_t> used_ssrcs;
  std::uint16_t next_port = 0;
  std::uint32_t ssrc_counter = 0;

  void Log(const std::string& text) const {
    if (config.on_log)
      config.on_log(text);
  }

  void SetState(Session* session, MediaSessionState state, const std::string& detail) {
    session->state = state;
    if (config.on_state)
      config.on_state(session->call_id, state, detail);
  }

  std::string AdvertisedAddress() const {
    if (!config.advertised_address.empty())
      return config.advertised_address;
    if (config.bind_address.empty() || config.bind_address == "0.0.0.0" ||
        config.bind_address == "*")
      return "127.0.0.1";
    return config.bind_address;
  }

  std::uint32_t AllocateSsrc(const std::string& call_id) {
    std::uint32_t candidate =
        config.ssrc_seed != 0 ? config.ssrc_seed + ssrc_counter++ : HashCallId(call_id);
    if (candidate == 0)
      candidate = 1;
    while (used_ssrcs.count(candidate) != 0)
      ++candidate;
    if (candidate == 0)
      candidate = 1;
    used_ssrcs.insert(candidate);
    return candidate;
  }

  void ReleaseSsrc(std::uint32_t ssrc) { used_ssrcs.erase(ssrc); }

  bool AllocatePortAndOpen(Session* session, RtpTransportMode mode,
                           const SdpVideoSession& offer, std::uint32_t ssrc,
                           std::string* error) {
    RtpSessionConfig rtp_config;
    rtp_config.mode = mode;
    rtp_config.local_address = config.bind_address.empty() ? "0.0.0.0"
                                                           : config.bind_address;
    rtp_config.payload_type = offer.payload_type;
    rtp_config.ssrc = ssrc;
    rtp_config.enable_rtcp = config.limits.enable_rtcp;
    rtp_config.sender_report_interval_ms = config.limits.sender_report_interval_ms;
    rtp_config.rtcp_cname =
        (config.device_id.empty() ? std::string("uvnvr") : config.device_id) + "@uvnvr";

    if (mode != RtpTransportMode::kTcpPassive) {
      rtp_config.remote_address = offer.address;
      rtp_config.remote_port = offer.port;
      if (rtp_config.remote_address.empty() || rtp_config.remote_address == "0.0.0.0") {
        if (error)
          *error = "SDP offer has no routable connection address";
        return false;
      }
    }

    const bool ephemeral = config.limits.rtp_port_min == 0;
    std::uint16_t port = ephemeral ? 0 : next_port;
    const std::uint16_t first_port = port;
    const std::uint16_t max_attempts = ephemeral ? 1 : 550;
    std::string last_error = "no free RTP port in the configured range";
    for (std::uint16_t attempt = 0; attempt < max_attempts; ++attempt) {
      rtp_config.local_port = port;
      auto candidate = std::make_unique<RtpSession>(rtp_config);
      if (candidate->Open(&last_error)) {
        session->rtp = std::move(candidate);
        if (!ephemeral) {
          next_port = static_cast<std::uint16_t>(port + 2);
          if (next_port > config.limits.rtp_port_max ||
              next_port < config.limits.rtp_port_min)
            next_port = config.limits.rtp_port_min;
        }
        return true;
      }
      if (mode == RtpTransportMode::kTcpActive) {
        // A connect failure is not a port-collision problem; retrying from a
        // different local port would only hide the platform's refusal.
        break;
      }
      if (ephemeral || config.limits.rtp_port_max <= config.limits.rtp_port_min)
        break;
      port = static_cast<std::uint16_t>(port + 2);
      if (port > config.limits.rtp_port_max || port < first_port)
        port = config.limits.rtp_port_min;
    }
    if (error)
      *error = last_error;
    return false;
  }

  Session* Find(const std::string& call_id) {
    const auto it = sessions.find(call_id);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  const Session* Find(const std::string& call_id) const {
    const auto it = sessions.find(call_id);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  void HandleRtcp(const std::string& call_id, const RtcpPacket& packet) {
    Session* session = Find(call_id);
    if (session == nullptr)
      return;
    if (config.on_rtcp)
      config.on_rtcp(call_id, packet);

    if (packet.payload_type == kRtcpPtBye) {
      const bool targets_us =
          packet.ssrcs.empty() ||
          std::find(packet.ssrcs.begin(), packet.ssrcs.end(), session->ssrc) !=
              packet.ssrcs.end();
      if (targets_us && session->state != MediaSessionState::kClosed) {
        Log("call-id=" + call_id + " rtcp bye received; closing media session");
        std::string ignored;
        CloseSession(session, "rtcp bye", false, &ignored);
      }
      return;
    }

    if (packet.is_keyframe_request) {
      ++session->keyframe_requests;
      session->keyframe_request_pending = true;
      Log(std::string("call-id=") + call_id + " keyframe-request source=" +
          (packet.is_fir ? "fir" : "pli") +
          " media_ssrc=" + std::to_string(packet.media_ssrc));
      if (config.on_keyframe_request)
        config.on_keyframe_request(call_id, packet.is_fir, packet.fir_sequence);
    }
  }

  void CloseSession(Session* session, std::string_view reason, bool send_rtcp_bye,
                    std::string* error) {
    if (session->state == MediaSessionState::kClosed)
      return;
    std::string bye_error;
    if (send_rtcp_bye && session->rtp && session->rtp->is_open() &&
        config.limits.enable_rtcp) {
      if (!session->rtp->SendBye(reason, &bye_error)) {
        Log("call-id=" + session->call_id + " cannot send RTCP BYE: " + bye_error);
      }
    }
    if (session->rtp)
      session->rtp->Close();
    ReleaseSsrc(session->ssrc);
    SetState(session, MediaSessionState::kClosed, std::string(reason));
    (void)error;
  }

  bool TryAccept(Session* session, std::string* error) {
    if (session->rtp == nullptr ||
        session->rtp->config().mode != RtpTransportMode::kTcpPassive)
      return true;
    if (session->rtp->is_open())
      return true;
    std::string peer;
    if (!session->rtp->Accept(&peer, 0, error))
      return false;
    Log("call-id=" + session->call_id + " accepted TCP media peer " + peer);
    return true;
  }
};

MediaSessionManager::MediaSessionManager(MediaSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

MediaSessionManager::~MediaSessionManager() { impl_->sessions.clear(); }

bool MediaSessionManager::OnInvite(const MediaInviteRequest& request,
                                   MediaInviteResponse* response, std::string* error) {
  if (response == nullptr) {
    if (error)
      *error = "INVITE response output is null";
    return false;
  }
  *response = MediaInviteResponse{};
  if (request.call_id.empty()) {
    response->status_code = 400;
    response->reason = "Bad Request";
    if (error)
      *error = "INVITE has no Call-ID";
    return true;
  }
  if (request.body.empty()) {
    response->status_code = 488;
    response->reason = "Not Acceptable Here";
    if (error)
      *error = "INVITE has no SDP body";
    return true;
  }
  if (!request.content_type.empty() && !ContainsIgnoreCase(request.content_type, "sdp")) {
    response->status_code = 415;
    response->reason = "Unsupported Media Type";
    if (error)
      *error = "INVITE content type is not SDP: " + request.content_type;
    return true;
  }

  // A SIP retransmission must not create a second socket or a new SSRC.
  if (Impl::Session* existing = impl_->Find(request.call_id)) {
    if (existing->state != MediaSessionState::kClosed &&
        existing->cseq == request.cseq && existing->offer_body == request.body) {
      *response = existing->response;
      return true;
    }
    if (existing->state != MediaSessionState::kClosed &&
        !existing->cseq.empty() && existing->cseq == request.cseq) {
      response->status_code = 491;
      response->reason = "Request Pending";
      if (error)
        *error = "same CSeq but a different SDP offer";
      return true;
    }
    impl_->Log("call-id=" + request.call_id +
               " re-INVITE replaces the existing media session");
    std::string ignored;
    impl_->CloseSession(existing, "re-INVITE", true, &ignored);
    impl_->sessions.erase(request.call_id);
  }

  // Drop closed diagnostics before refusing a new dialog for lack of slots.
  std::size_t active = 0;
  for (const auto& [id, session] : impl_->sessions) {
    (void)id;
    if (session->state != MediaSessionState::kClosed)
      ++active;
  }
  if (active >= impl_->config.limits.max_sessions) {
    for (auto it = impl_->sessions.begin(); it != impl_->sessions.end();) {
      if (it->second->state == MediaSessionState::kClosed)
        it = impl_->sessions.erase(it);
      else
        ++it;
    }
    active = impl_->sessions.size();
  }
  if (active >= impl_->config.limits.max_sessions) {
    response->status_code = 503;
    response->reason = "Service Unavailable";
    if (error)
      *error = "too many active media sessions";
    return true;
  }

  SdpVideoSession offer;
  std::string parse_error;
  if (!ParseSdp(request.body, &offer, &parse_error)) {
    response->status_code = 488;
    response->reason = "Not Acceptable Here";
    if (error)
      *error = parse_error;
    return true;
  }
  if (offer.direction == "sendonly") {
    response->status_code = 488;
    response->reason = "Not Acceptable Here";
    if (error)
      *error = "offer is sendonly; the device only sends real-time media";
    return true;
  }
  if (offer.address.empty() || offer.address == "0.0.0.0") {
    response->status_code = 488;
    response->reason = "Not Acceptable Here";
    if (error)
      *error = "offer has no routable c= address";
    return true;
  }

  RtpSetup setup = RtpSetup::kNone;
  const RtpTransportMode mode = DeviceModeForOffer(offer, &setup);

  auto session = std::make_unique<Impl::Session>();
  session->call_id = request.call_id;
  session->cseq = request.cseq;
  session->offer_body = request.body;
  session->offer = offer;
  session->ssrc = impl_->AllocateSsrc(request.call_id);
  session->created_at_ms = MonotonicMillis();

  std::string open_error;
  if (!impl_->AllocatePortAndOpen(session.get(), mode, offer, session->ssrc, &open_error)) {
    impl_->ReleaseSsrc(session->ssrc);
    response->status_code = 500;
    response->reason = "Server Internal Error";
    if (error)
      *error = "cannot open the RTP media socket: " + open_error;
    return true;
  }

  const std::uint16_t local_port = session->rtp->local_port();
  const std::string& advertised = impl_->AdvertisedAddress();
  std::string sdp;
  SdpVideoSession answer;
  std::string sdp_error;
  if (!BuildSdpAnswer(offer, advertised, local_port, impl_->config.device_id,
                      session->ssrc, &sdp, &answer, &sdp_error)) {
    session->rtp->Close();
    impl_->ReleaseSsrc(session->ssrc);
    response->status_code = 500;
    response->reason = "Server Internal Error";
    if (error)
      *error = sdp_error;
    return true;
  }
  session->answer = answer;

  const std::string call_id = request.call_id;
  session->rtp->SetRtcpEventCallback(
      [this, call_id](const RtcpPacket& packet) { impl_->HandleRtcp(call_id, packet); });

  response->status_code = 200;
  response->reason = "OK";
  response->sdp = sdp;
  response->mode = answer.mode;
  response->setup = answer.setup;
  response->rtp_port = local_port;
  response->rtcp_port = mode == RtpTransportMode::kUdp ? session->rtp->rtcp_local_port() : 0;
  response->ssrc = session->ssrc;
  session->local_rtp_port = local_port;
  session->local_rtcp_port = response->rtcp_port;
  session->response = *response;
  session->peer_rtp_port = offer.port;
  session->state = MediaSessionState::kWaiting;
  impl_->sessions[request.call_id] = std::move(session);
  impl_->Log("call-id=" + request.call_id + " INVITE accepted mode=" + TransportName(mode) +
             " rtp_port=" + std::to_string(local_port) +
             " rtcp_port=" + std::to_string(response->rtcp_port) +
             " ssrc=0x" + [&] {
               char text[16];
               std::snprintf(text, sizeof(text), "%08x", response->ssrc);
               return std::string(text);
             }());
  if (impl_->config.on_state) {
    impl_->config.on_state(request.call_id, MediaSessionState::kWaiting,
                           std::string("INVITE accepted mode=") + TransportName(mode));
  }
  return true;
}

bool MediaSessionManager::OnAck(const std::string& call_id, std::string* error) {
  Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr) {
    if (error)
      *error = "no media session for this Call-ID";
    return false;
  }
  if (session->state == MediaSessionState::kClosed) {
    if (error)
      *error = "media session is already closed";
    return false;
  }
  if (session->state == MediaSessionState::kStreaming)
    return true;

  session->ack_received = true;
  if (session->rtp->config().mode == RtpTransportMode::kTcpPassive) {
    std::string accept_error;
    if (!impl_->TryAccept(session, &accept_error)) {
      if (impl_->config.limits.tcp_accept_timeout_ms == 0) {
        impl_->Log("call-id=" + call_id +
                   " ACK received; waiting for the platform TCP connection");
        return true;
      }
      if (error)
        *error = accept_error;
      return false;
    }
  }
  impl_->SetState(session, MediaSessionState::kStreaming, "ACK received");
  return true;
}

bool MediaSessionManager::OnBye(const std::string& call_id, std::string* error) {
  Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr) {
    if (error)
      *error = "no media session for this Call-ID";
    return false;
  }
  impl_->CloseSession(session, "sip bye", true, error);
  return true;
}

bool MediaSessionManager::HandleSipRequest(std::string_view method,
                                          const MediaInviteRequest& request,
                                          MediaInviteResponse* response,
                                          std::string* error) {
  const std::string lower = ToLowerAscii(method);
  if (lower == "invite")
    return OnInvite(request, response, error);
  if (response != nullptr) {
    response->status_code = 200;
    response->reason = "OK";
  }
  if (lower == "ack")
    return OnAck(request.call_id, error);
  if (lower == "bye")
    return OnBye(request.call_id, error);
  if (response != nullptr) {
    response->status_code = 405;
    response->reason = "Method Not Allowed";
  }
  if (error)
    *error = "unsupported SIP method for the media dialog: " + std::string(method);
  return false;
}

bool MediaSessionManager::SendFrame(const std::string& call_id, const std::uint8_t* ps,
                                    std::size_t size, std::uint32_t timestamp_90k,
                                    bool keyframe, std::string* error) {
  Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr) {
    if (error)
      *error = "no media session for this Call-ID";
    return false;
  }
  if (session->state != MediaSessionState::kStreaming) {
    if (error)
      *error = session->state == MediaSessionState::kWaiting
                   ? "media session is waiting for ACK or the TCP connection"
                   : "media session is not streaming";
    return false;
  }
  if (!session->rtp->SendFrame(ps, size, timestamp_90k, keyframe, error))
    return false;
  session->last_frame_at_ms = MonotonicMillis();
  if (keyframe && (session->keyframe_request_pending || session->waiting_for_keyframe)) {
    session->keyframe_request_pending = false;
    session->waiting_for_keyframe = false;
    ++session->keyframe_requests_honored;
    impl_->Log("call-id=" + call_id + " keyframe-request honored at pts=" +
               std::to_string(timestamp_90k));
  }
  return true;
}

bool MediaSessionManager::PollControl(const std::string& call_id, int timeout_ms,
                                      std::string* error) {
  if (!call_id.empty()) {
    Impl::Session* session = impl_->Find(call_id);
    if (session == nullptr) {
      if (error)
        *error = "no media session for this Call-ID";
      return false;
    }
    if (session->state != MediaSessionState::kStreaming)
      return false;
    return session->rtp->PollRtcp(timeout_ms, error);
  }

  bool any = false;
  bool first = true;
  for (auto& [id, session] : impl_->sessions) {
    (void)id;
    if (session->state != MediaSessionState::kStreaming)
      continue;
    std::string poll_error;
    const bool result = session->rtp->PollRtcp(first ? timeout_ms : 0, &poll_error);
    first = false;
    if (result)
      any = true;
    else if (!poll_error.empty() && error != nullptr && error->empty())
      *error = poll_error;
  }
  return any;
}

bool MediaSessionManager::Tick(std::int64_t now_ms, std::string* error) {
  bool ok = true;
  for (auto& [id, session] : impl_->sessions) {
    (void)id;
    if (session->state == MediaSessionState::kClosed)
      continue;

    if (session->state == MediaSessionState::kWaiting) {
      if (!session->ack_received && now_ms - session->created_at_ms >=
                                        static_cast<std::int64_t>(
                                            impl_->config.limits.ack_timeout_ms)) {
        impl_->Log("call-id=" + session->call_id + " ACK timeout; closing media session");
        std::string ignored;
        impl_->CloseSession(session.get(), "ack timeout", true, &ignored);
        continue;
      }
      if (session->rtp->config().mode == RtpTransportMode::kTcpPassive &&
          !session->rtp->is_open()) {
        std::string accept_error;
        if (!impl_->TryAccept(session.get(), &accept_error))
          continue;
      }
      // Media starts only after ACK, even if the platform connected first.
      if (session->ack_received) {
        impl_->SetState(session.get(), MediaSessionState::kStreaming, "ACK received");
      }
    }

    if (session->state == MediaSessionState::kStreaming) {
      if (session->rtp->config().mode == RtpTransportMode::kTcpPassive &&
          !session->rtp->is_open()) {
        std::string accept_error;
        if (!impl_->TryAccept(session.get(), &accept_error)) {
          if (error != nullptr && error->empty())
            *error = accept_error;
          continue;
        }
        impl_->SetState(session.get(), MediaSessionState::kStreaming,
                        "TCP media connected");
      }
      std::string tick_error;
      if (!session->rtp->Tick(now_ms, &tick_error)) {
        ok = false;
        if (error != nullptr && error->empty())
          *error = tick_error;
      }
    }
  }
  return ok;
}

bool MediaSessionManager::ConsumeKeyframeRequest(const std::string& call_id) {
  Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr || session->state != MediaSessionState::kStreaming)
    return false;
  if (!session->keyframe_request_pending)
    return false;
  session->keyframe_request_pending = false;
  session->waiting_for_keyframe = true;
  return true;
}

bool MediaSessionManager::Close(const std::string& call_id, std::string_view reason,
                                bool send_rtcp_bye, std::string* error) {
  Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr) {
    if (error)
      *error = "no media session for this Call-ID";
    return false;
  }
  impl_->CloseSession(session, reason, send_rtcp_bye, error);
  return true;
}

std::size_t MediaSessionManager::session_count() const { return impl_->sessions.size(); }

std::size_t MediaSessionManager::active_session_count() const {
  std::size_t count = 0;
  for (const auto& [id, session] : impl_->sessions) {
    (void)id;
    if (session->state != MediaSessionState::kClosed)
      ++count;
  }
  return count;
}

std::vector<std::string> MediaSessionManager::call_ids() const {
  std::vector<std::string> ids;
  ids.reserve(impl_->sessions.size());
  for (const auto& [id, session] : impl_->sessions) {
    (void)session;
    ids.push_back(id);
  }
  return ids;
}

bool MediaSessionManager::Snapshot(const std::string& call_id,
                                   MediaSessionSnapshot* snapshot) const {
  if (snapshot == nullptr)
    return false;
  const Impl::Session* session = impl_->Find(call_id);
  if (session == nullptr)
    return false;
  *snapshot = MediaSessionSnapshot{};
  snapshot->state = session->state;
  snapshot->call_id = session->call_id;
  snapshot->cseq = session->cseq;
  snapshot->mode = session->rtp ? session->rtp->config().mode : RtpTransportMode::kUdp;
  snapshot->setup = session->answer.setup;
  snapshot->peer_address = session->rtp && !session->rtp->peer_address().empty()
                               ? session->rtp->peer_address()
                               : session->offer.address;
  snapshot->peer_rtp_port = session->peer_rtp_port;
  snapshot->local_rtp_port = session->local_rtp_port;
  snapshot->local_rtcp_port = session->local_rtcp_port;
  snapshot->ssrc = session->ssrc;
  if (session->rtp) {
    const RtpSendStats& rtp = session->rtp->stats();
    snapshot->frames = rtp.frames;
    snapshot->packets = rtp.packets;
    snapshot->payload_bytes = rtp.payload_bytes;
    snapshot->keyframes = rtp.keyframes;
    snapshot->last_sequence = rtp.last_sequence;
    snapshot->last_timestamp = rtp.last_timestamp;
    snapshot->rtcp = session->rtp->rtcp_stats();
  }
  snapshot->keyframe_requests = session->keyframe_requests;
  snapshot->keyframe_requests_honored = session->keyframe_requests_honored;
  snapshot->keyframe_request_pending =
      session->keyframe_request_pending || session->waiting_for_keyframe;
  snapshot->created_at_ms = session->created_at_ms;
  snapshot->last_frame_at_ms = session->last_frame_at_ms;
  return true;
}

const MediaSessionConfig& MediaSessionManager::config() const { return impl_->config; }

}  // namespace uvnvr::gb28181
