// gb28181_session_cli: drive one GB/T 28181 media dialog from an SDP offer.
//
// The platform side of the test harness writes an SDP offer to a file and
// launches this tool. The tool answers the INVITE, simulates the ACK, streams
// an Annex B file as PS/RTP, consumes RTCP Receiver Reports and key-frame
// requests, and sends RTCP BYE on exit (or on a platform RTCP BYE).
//
//   gb28181_session_cli --input es.h264 --offer offer.sdp \
//       --call-id test-1 --advertise 127.0.0.1 --duration 5
//
// It is a device-side demo, not the SIP stack: a real deployment passes the
// same offer/ACK/BYE to MediaSessionManager from the SIP user agent.

#include "gb28181/media_session.h"
#include "gb28181/ps.h"
#include "gb28181/rtcp.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace uvnvr::gb28181;

struct Options {
  std::string input;
  std::string offer_path;
  PsVideoCodec codec = PsVideoCodec::kH264;
  bool codec_set = false;
  std::string device_id = "34020000001320000001";
  std::string call_id = "gb28181-session";
  std::string bind_address = "0.0.0.0";
  std::string advertise_address = "127.0.0.1";
  std::uint16_t port_min = 30000;
  std::uint16_t port_max = 30998;
  std::uint32_t ssrc_seed = 0;
  std::uint32_t fps = 25;
  std::uint32_t sender_report_ms = 1000;
  std::uint32_t max_frames = 0;
  int duration_seconds = 8;
  int connect_timeout_ms = 10000;
  bool loop = true;
  bool realtime = true;
  bool verbose = false;
};

void PrintUsage(const char* program) {
  std::cout
      << "usage: " << program << " --input <annex-b> --offer <sdp> [options]\n\n"
      << "  --input FILE             Annex B H.264/H.265 elementary stream\n"
      << "  --offer FILE             SDP offer from the platform INVITE\n"
      << "  --codec h264|h265        input codec (default: infer from extension)\n"
      << "  --device-id ID           20-digit local ID for SDP o= (default\n"
      << "                           34020000001320000001)\n"
      << "  --call-id ID             dialog identifier (default gb28181-session)\n"
      << "  --bind ADDR              local bind address (default 0.0.0.0)\n"
      << "  --advertise ADDR         address in the answer SDP (default 127.0.0.1)\n"
      << "  --port-min/--port-max N  RTP port range (default 30000..30998)\n"
      << "  --ssrc N                 SSRC seed; 0 derives from the Call-ID\n"
      << "  --fps N                  timestamp clock for the raw ES (default 25)\n"
      << "  --sender-report-ms N     RTCP SR interval (default 1000)\n"
      << "  --duration N             seconds to stream (default 8)\n"
      << "  --max-frames N           stop after N sent frames (0 = no limit)\n"
      << "  --connect-timeout-ms N   wait for ACK/TCP connection (default 10000)\n"
      << "  --no-loop                stop at the end of the elementary stream\n"
      << "  --no-realtime            send as fast as possible\n"
      << "  --verbose                print every frame\n";
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
  if (text.empty())
    return false;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0')
    return false;
  *value = parsed;
  return true;
}

bool ParseOptions(int argc, char** argv, Options* options, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](std::string* value) -> bool {
      if (i + 1 >= argc) {
        *error = arg + " needs a value";
        return false;
      }
      *value = argv[++i];
      return true;
    };
    auto number = [&](std::uint64_t min, std::uint64_t max, std::uint64_t* value) -> bool {
      std::string text;
      if (!next(&text))
        return false;
      std::uint64_t parsed = 0;
      if (!ParseU64(text, &parsed) || parsed < min || parsed > max) {
        *error = arg + " has an invalid value: " + text;
        return false;
      }
      *value = parsed;
      return true;
    };

    if (arg == "--input") {
      if (!next(&options->input))
        return false;
    } else if (arg == "--offer") {
      if (!next(&options->offer_path))
        return false;
    } else if (arg == "--codec") {
      std::string value;
      if (!next(&value))
        return false;
      if (value == "h264" || value == "H264") {
        options->codec = PsVideoCodec::kH264;
      } else if (value == "h265" || value == "H265" || value == "hevc") {
        options->codec = PsVideoCodec::kH265;
      } else {
        *error = "--codec must be h264 or h265";
        return false;
      }
      options->codec_set = true;
    } else if (arg == "--device-id") {
      if (!next(&options->device_id))
        return false;
    } else if (arg == "--call-id") {
      if (!next(&options->call_id))
        return false;
    } else if (arg == "--bind") {
      if (!next(&options->bind_address))
        return false;
    } else if (arg == "--advertise") {
      if (!next(&options->advertise_address))
        return false;
    } else if (arg == "--port-min") {
      std::uint64_t value = 0;
      if (!number(0, 65534, &value))
        return false;
      options->port_min = static_cast<std::uint16_t>(value);
    } else if (arg == "--port-max") {
      std::uint64_t value = 0;
      if (!number(0, 65534, &value))
        return false;
      options->port_max = static_cast<std::uint16_t>(value);
    } else if (arg == "--ssrc") {
      std::uint64_t value = 0;
      if (!number(0, 0xFFFFFFFFu, &value))
        return false;
      options->ssrc_seed = static_cast<std::uint32_t>(value);
    } else if (arg == "--fps") {
      std::uint64_t value = 0;
      if (!number(1, 1000, &value))
        return false;
      options->fps = static_cast<std::uint32_t>(value);
    } else if (arg == "--sender-report-ms") {
      std::uint64_t value = 0;
      if (!number(0, 3600000, &value))
        return false;
      options->sender_report_ms = static_cast<std::uint32_t>(value);
    } else if (arg == "--duration") {
      std::uint64_t value = 0;
      if (!number(1, 3600, &value))
        return false;
      options->duration_seconds = static_cast<int>(value);
    } else if (arg == "--max-frames") {
      std::uint64_t value = 0;
      if (!number(0, 100000000, &value))
        return false;
      options->max_frames = static_cast<std::uint32_t>(value);
    } else if (arg == "--connect-timeout-ms") {
      std::uint64_t value = 0;
      if (!number(1, 3600000, &value))
        return false;
      options->connect_timeout_ms = static_cast<int>(value);
    } else if (arg == "--no-loop") {
      options->loop = false;
    } else if (arg == "--no-realtime") {
      options->realtime = false;
    } else if (arg == "--verbose") {
      options->verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else {
      *error = "unknown option: " + arg;
      return false;
    }
  }
  if (options->input.empty()) {
    *error = "--input is required";
    return false;
  }
  if (options->offer_path.empty()) {
    *error = "--offer is required";
    return false;
  }
  if (!options->codec_set) {
    const std::string& lower = options->input;
    if (lower.find(".h265") != std::string::npos || lower.find(".hevc") != std::string::npos ||
        lower.find(".265") != std::string::npos)
      options->codec = PsVideoCodec::kH265;
  }
  if (options->port_max < options->port_min)
    options->port_max = options->port_min;
  return true;
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* data, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open " + path;
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    *error = "cannot size " + path;
    return false;
  }
  input.seekg(0, std::ios::beg);
  data->resize(static_cast<std::size_t>(size));
  if (size > 0 &&
      !input.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(size))) {
    *error = "cannot read " + path;
    return false;
  }
  return true;
}

bool ReadText(const std::string& path, std::string* text, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open " + path;
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  *text = std::move(content);
  return true;
}

std::int64_t MonotonicMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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

void PrintAnswer(const MediaInviteResponse& response) {
  std::cout << "INVITE call-id answer: status=" << response.status_code
            << " reason=" << response.reason << "\n";
  std::cout << "mode=" << TransportName(response.mode)
            << " setup=" << RtpSetupName(response.setup)
            << " rtp_port=" << response.rtp_port
            << " rtcp_port=" << response.rtcp_port
            << " ssrc=0x" << std::hex << response.ssrc << std::dec << "\n";
  std::cout << "answer-sdp-begin\n";
  std::cout << response.sdp;
  if (!response.sdp.empty() && response.sdp.size() >= 2 &&
      response.sdp[response.sdp.size() - 2] != '\n')
    std::cout << "\n";
  std::cout << "answer-sdp-end\n";
  std::cout.flush();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, &options, &error)) {
    std::cerr << "gb28181_session_cli: " << error << "\n\n";
    PrintUsage(argv[0]);
    return 2;
  }

  std::vector<std::uint8_t> elementary;
  if (!ReadFile(options.input, &elementary, &error)) {
    std::cerr << "gb28181_session_cli: " << error << "\n";
    return 1;
  }
  std::vector<EsAccessUnit> access_units;
  if (!SplitAnnexBAccessUnits(elementary.data(), elementary.size(), options.codec,
                              &access_units, &error)) {
    std::cerr << "gb28181_session_cli: cannot split elementary stream: " << error << "\n";
    return 1;
  }
  if (access_units.empty()) {
    std::cerr << "gb28181_session_cli: input has no access units\n";
    return 1;
  }
  std::size_t input_keyframes = 0;
  for (const EsAccessUnit& unit : access_units)
    input_keyframes += unit.keyframe ? 1 : 0;
  std::cout << "input: " << options.input << " codec=" << PsVideoCodecName(options.codec)
            << " frames=" << access_units.size() << " keyframes=" << input_keyframes
            << " bytes=" << elementary.size() << "\n";

  std::string offer;
  if (!ReadText(options.offer_path, &offer, &error)) {
    std::cerr << "gb28181_session_cli: " << error << "\n";
    return 1;
  }

  MediaSessionConfig config;
  config.device_id = options.device_id;
  config.advertised_address = options.advertise_address;
  config.bind_address = options.bind_address;
  config.ssrc_seed = options.ssrc_seed;
  config.limits.rtp_port_min = options.port_min;
  config.limits.rtp_port_max = options.port_max;
  config.limits.enable_rtcp = true;
  config.limits.sender_report_interval_ms = options.sender_report_ms;
  config.on_state = [](const std::string& call_id, MediaSessionState state,
                       const std::string& detail) {
    std::cout << "event: state call-id=" << call_id << " state="
              << MediaSessionStateName(state) << " detail=" << detail << "\n";
    std::cout.flush();
  };
  config.on_keyframe_request = [](const std::string& call_id, bool is_fir,
                                  std::uint8_t sequence) {
    std::cout << "event: keyframe-request call-id=" << call_id
              << " source=" << (is_fir ? "fir" : "pli")
              << " fir_sequence=" << static_cast<int>(sequence) << "\n";
    std::cout.flush();
  };
  config.on_rtcp = [](const std::string& call_id, const RtcpPacket& packet) {
    if (packet.payload_type == kRtcpPtReceiverReport) {
      std::cout << "event: rtcp-rr call-id=" << call_id
                << " blocks=" << packet.report_blocks.size();
      if (!packet.report_blocks.empty()) {
        const RtcpReportBlock& block = packet.report_blocks.front();
        std::cout << " fraction_lost=" << static_cast<int>(block.fraction_lost)
                  << " cumulative_lost=" << block.cumulative_lost
                  << " jitter=" << block.jitter
                  << " highest_seq=" << block.extended_highest_sequence;
      }
      std::cout << "\n";
      std::cout.flush();
    } else if (packet.payload_type == kRtcpPtBye) {
      std::cout << "event: rtcp-bye call-id=" << call_id
                << " reason=" << packet.reason << "\n";
      std::cout.flush();
    }
  };
  config.on_log = [](const std::string& text) {
    std::cout << "event: log " << text << "\n";
    std::cout.flush();
  };

  MediaSessionManager manager(std::move(config));
  MediaInviteRequest invite;
  invite.call_id = options.call_id;
  invite.cseq = "1 INVITE";
  invite.from = "<sip:34020000002000000001@3402000000>";
  invite.to = "<sip:" + options.device_id + "@3402000000>";
  invite.content_type = "application/sdp";
  invite.body = offer;
  MediaInviteResponse response;
  if (!manager.OnInvite(invite, &response, &error) || response.status_code != 200) {
    PrintAnswer(response);
    std::cerr << "gb28181_session_cli: INVITE rejected: " << error << "\n";
    return 1;
  }
  PrintAnswer(response);

  if (!manager.OnAck(options.call_id, &error)) {
    std::cerr << "gb28181_session_cli: ACK failed: " << error << "\n";
    return 1;
  }
  std::cout << "ACK call-id=" << options.call_id << "\n";
  std::cout.flush();

  // For TCP passive the platform connects after reading the answer. Wait until
  // the dialog is streaming before sending the first access unit.
  const std::int64_t connect_deadline = MonotonicMillis() + options.connect_timeout_ms;
  while (MonotonicMillis() < connect_deadline) {
    manager.Tick(MonotonicMillis(), &error);
    manager.PollControl(options.call_id, 0, &error);
    MediaSessionSnapshot snapshot;
    if (!manager.Snapshot(options.call_id, &snapshot))
      break;
    if (snapshot.state == MediaSessionState::kStreaming)
      break;
    if (snapshot.state == MediaSessionState::kClosed) {
      std::cerr << "gb28181_session_cli: session closed before streaming\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  {
    MediaSessionSnapshot snapshot;
    manager.Snapshot(options.call_id, &snapshot);
    if (snapshot.state != MediaSessionState::kStreaming) {
      std::cerr << "gb28181_session_cli: timed out waiting for ACK/TCP connection\n";
      return 1;
    }
  }

  PsMuxerConfig muxer_config;
  muxer_config.codec = options.codec;
  PsMuxer muxer(muxer_config);
  std::vector<std::uint8_t> ps;
  std::uint64_t frame_index = 0;
  std::uint64_t frames_sent = 0;
  std::uint64_t frames_skipped = 0;
  std::uint64_t sent_keyframes = 0;
  std::uint32_t first_timestamp = 0;
  std::uint32_t last_timestamp = 0;
  bool waiting_for_keyframe = false;
  bool ok = true;
  const auto start = std::chrono::steady_clock::now();
  std::size_t unit_index = 0;

  while (ok) {
    if (unit_index >= access_units.size()) {
      if (!options.loop)
        break;
      unit_index = 0;
    }
    const EsAccessUnit& unit = access_units[unit_index++];
    const std::uint32_t timestamp =
        static_cast<std::uint32_t>((frame_index * 90000u) / options.fps);

    // A platform PLI/FIR asks for an immediate I-frame. The file source cannot
    // force an encoder, so it drops delta frames until the next natural IDR;
    // a camera integration would call the encoder's force-IDR instead.
    if (manager.ConsumeKeyframeRequest(options.call_id)) {
      waiting_for_keyframe = true;
      std::cout << "event: keyframe-request accepted frame_index=" << frame_index << "\n";
      std::cout.flush();
    }
    if (waiting_for_keyframe && !unit.keyframe) {
      ++frames_skipped;
      ++frame_index;
      if (options.realtime) {
        const auto deadline =
            start + std::chrono::microseconds(frame_index * 1000000ull / options.fps);
        std::this_thread::sleep_until(deadline);
      }
      manager.Tick(MonotonicMillis(), &error);
      manager.PollControl(options.call_id, 0, &error);
      MediaSessionSnapshot snapshot;
      if (manager.Snapshot(options.call_id, &snapshot) &&
          snapshot.state == MediaSessionState::kClosed) {
        ok = false;
      }
      continue;
    }
    if (unit.keyframe)
      waiting_for_keyframe = false;

    const std::int64_t pts = static_cast<std::int64_t>(timestamp);
    const PsFrame frame{unit.data.data(), unit.data.size(), pts, pts, false, unit.keyframe};
    if (!muxer.MuxFrame(frame, &ps, &error)) {
      std::cerr << "\ngb28181_session_cli: PS mux failed: " << error << "\n";
      return 1;
    }
    if (!manager.SendFrame(options.call_id, ps.data(), ps.size(), timestamp, unit.keyframe,
                           &error)) {
      std::cerr << "\ngb28181_session_cli: RTP send failed: " << error << "\n";
      return 1;
    }
    if (frames_sent == 0)
      first_timestamp = timestamp;
    ++frames_sent;
    if (unit.keyframe)
      ++sent_keyframes;
    last_timestamp = timestamp;
    if (options.verbose) {
      std::printf("frame=%llu pts=%u key=%d ps_bytes=%zu\n",
                  static_cast<unsigned long long>(frame_index), timestamp,
                  unit.keyframe ? 1 : 0, ps.size());
      std::cout.flush();
    }
    ++frame_index;

    manager.Tick(MonotonicMillis(), &error);
    manager.PollControl(options.call_id, 0, &error);
    {
      MediaSessionSnapshot snapshot;
      if (manager.Snapshot(options.call_id, &snapshot) &&
          snapshot.state == MediaSessionState::kClosed)
        ok = false;
    }

    const bool frame_limit = options.max_frames != 0 && frames_sent >= options.max_frames;
    const bool time_limit =
        frame_index >= static_cast<std::uint64_t>(options.duration_seconds) *
                           static_cast<std::uint64_t>(options.fps);
    if (frame_limit || time_limit)
      break;

    if (options.realtime) {
      const auto deadline =
          start + std::chrono::microseconds(frame_index * 1000000ull / options.fps);
      std::this_thread::sleep_until(deadline);
    }
  }

  MediaSessionSnapshot final_snapshot;
  manager.Snapshot(options.call_id, &final_snapshot);
  const bool still_open = final_snapshot.state != MediaSessionState::kClosed;
  if (still_open)
    manager.OnBye(options.call_id, &error);

  manager.Snapshot(options.call_id, &final_snapshot);
  std::printf(
      "summary: call_id=%s mode=%s rtp_port=%u rtcp_port=%u ssrc=0x%08x "
      "frames_sent=%llu frames_skipped=%llu keyframes=%llu rtp_frames=%llu "
      "rtp_packets=%llu payload_bytes=%llu last_sequence=%u last_timestamp=%u "
      "rtcp_sr_sent=%llu rtcp_rr_recv=%llu rtcp_pli=%llu rtcp_fir=%llu "
      "rtcp_rtt_ms=%u rtcp_fraction_lost=%u rtcp_cumulative_lost=%d "
      "keyframe_requests=%llu honored=%llu state=%s\n",
      options.call_id.c_str(), TransportName(final_snapshot.mode),
      final_snapshot.local_rtp_port, final_snapshot.local_rtcp_port,
      final_snapshot.ssrc, static_cast<unsigned long long>(frames_sent),
      static_cast<unsigned long long>(frames_skipped),
      static_cast<unsigned long long>(sent_keyframes),
      static_cast<unsigned long long>(final_snapshot.frames),
      static_cast<unsigned long long>(final_snapshot.packets),
      static_cast<unsigned long long>(final_snapshot.payload_bytes),
      static_cast<unsigned>(final_snapshot.last_sequence),
      static_cast<unsigned>(final_snapshot.last_timestamp),
      static_cast<unsigned long long>(final_snapshot.rtcp.sender_reports_sent),
      static_cast<unsigned long long>(final_snapshot.rtcp.receiver_reports_received),
      static_cast<unsigned long long>(final_snapshot.rtcp.pli_received),
      static_cast<unsigned long long>(final_snapshot.rtcp.fir_received),
      static_cast<unsigned>(final_snapshot.rtcp.last_rtt_ms),
      static_cast<unsigned>(final_snapshot.rtcp.last_fraction_lost),
      static_cast<int>(final_snapshot.rtcp.last_cumulative_lost),
      static_cast<unsigned long long>(final_snapshot.keyframe_requests),
      static_cast<unsigned long long>(final_snapshot.keyframe_requests_honored),
      MediaSessionStateName(final_snapshot.state));
  std::printf("timestamp range: first=%u last=%u frames=%llu\n", first_timestamp,
              last_timestamp, static_cast<unsigned long long>(frames_sent));
  return 0;
}
