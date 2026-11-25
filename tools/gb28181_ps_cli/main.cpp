// gb28181_ps_cli: package an Annex B H.264/H.265 file as GB/T 28181 PS and
// send it as RTP over UDP, TCP active or TCP passive.
//
// The tool exists so the media path can be exercised without a platform:
//
//   # UDP, send two seconds of video to a receiver
//   gb28181_ps_cli --input es.h264 --codec h264 --transport udp \
//       --remote 127.0.0.1 --port 5004 --ps-out /tmp/out.ps --sdp-out /tmp/out.sdp
//
//   # TCP passive (device listens, platform connects)
//   gb28181_ps_cli --input es.h265 --codec h265 --transport tcp-passive \
//       --local-port 5004 --ps-out /tmp/out.ps
//
// Every frame prints its PTS, PS size and RTP packet count with --verbose,
// and the summary counts frames, packets and markers so the timestamp and
// marker behaviour can be checked on the wire.

#include "gb28181/ps.h"
#include "gb28181/rtp.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace uvnvr::gb28181;

struct Options {
  std::string input;
  PsVideoCodec codec = PsVideoCodec::kH264;
  bool codec_set = false;
  RtpTransportMode transport = RtpTransportMode::kUdp;
  std::string local_address = "0.0.0.0";
  std::uint16_t local_port = 0;
  std::string remote_address;
  std::uint16_t remote_port = 0;
  std::string advertise_address;
  std::string origin_id = "34020000001320000001";
  std::uint32_t ssrc = 1;
  std::uint8_t payload_type = 96;
  std::size_t mtu = 1400;
  std::uint32_t fps = 25;
  std::uint64_t max_frames = 0;
  bool loop = false;
  bool realtime = true;
  bool verbose = false;
  std::string ps_out;
  std::string sdp_out;
  int accept_timeout_ms = 10000;
};

void PrintUsage(const char* program) {
  std::cout
      << "usage: " << program << " --input <annex-b-file> [options]\n"
      << "\n"
      << "Required:\n"
      << "  --input FILE              raw H.264/H.265 Annex B elementary stream\n"
      << "  --codec h264|h265         input codec (inferred from the extension if omitted)\n"
      << "\n"
      << "Transport:\n"
      << "  --transport udp|tcp-passive|tcp-active   (default udp)\n"
      << "  --remote HOST             destination for udp/tcp-active\n"
      << "  --port N                  destination port for udp/tcp-active\n"
      << "  --local HOST              bind address (default 0.0.0.0)\n"
      << "  --local-port N            bind port (default ephemeral)\n"
      << "  --accept-timeout-ms N     TCP passive accept timeout (default 10000)\n"
      << "\n"
      << "RTP/PS:\n"
      << "  --ssrc N                  SSRC, decimal or 0x-prefixed (default 1)\n"
      << "  --payload-type N          RTP payload type (default 96)\n"
      << "  --mtu N                   maximum RTP payload bytes (default 1400)\n"
      << "  --fps N                   timestamp clock for the raw ES (default 25)\n"
      << "  --max-frames N            stop after N frames (0 = all)\n"
      << "  --loop                    repeat the elementary stream\n"
      << "  --no-realtime             send as fast as possible\n"
      << "  --verbose                 print one line per frame and the PS header\n"
      << "\n"
      << "Outputs:\n"
      << "  --ps-out FILE             write the raw PS bytes (for ffprobe/ffmpeg)\n"
      << "  --sdp-out FILE            write the sender SDP\n"
      << "  --advertise-address ADDR  address placed in SDP (default 127.0.0.1)\n"
      << "  --origin-id ID            SDP o= value (default 34020000001320000001)\n"
      << "\n"
      << "Example:\n"
      << "  " << program << " --input es.h264 --codec h264 --transport udp \\\n"
      << "      --remote 127.0.0.1 --port 5004 --ps-out out.ps --verbose\n";
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
  if (text.empty())
    return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != '\0')
    return false;
  *value = static_cast<std::uint64_t>(parsed);
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
    auto parse_u64 = [&](std::uint64_t min, std::uint64_t max, std::uint64_t* value) -> bool {
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
    } else if (arg == "--transport") {
      std::string value;
      if (!next(&value))
        return false;
      if (value == "udp") {
        options->transport = RtpTransportMode::kUdp;
      } else if (value == "tcp-active") {
        options->transport = RtpTransportMode::kTcpActive;
      } else if (value == "tcp-passive") {
        options->transport = RtpTransportMode::kTcpPassive;
      } else {
        *error = "--transport must be udp, tcp-active or tcp-passive";
        return false;
      }
    } else if (arg == "--remote") {
      if (!next(&options->remote_address))
        return false;
    } else if (arg == "--port") {
      std::uint64_t value = 0;
      if (!parse_u64(1, 65535, &value))
        return false;
      options->remote_port = static_cast<std::uint16_t>(value);
    } else if (arg == "--local") {
      if (!next(&options->local_address))
        return false;
    } else if (arg == "--local-port") {
      std::uint64_t value = 0;
      if (!parse_u64(0, 65535, &value))
        return false;
      options->local_port = static_cast<std::uint16_t>(value);
    } else if (arg == "--advertise-address") {
      if (!next(&options->advertise_address))
        return false;
    } else if (arg == "--origin-id") {
      if (!next(&options->origin_id))
        return false;
    } else if (arg == "--ssrc") {
      std::uint64_t value = 0;
      if (!parse_u64(0, 0xFFFFFFFFu, &value))
        return false;
      options->ssrc = static_cast<std::uint32_t>(value);
    } else if (arg == "--payload-type") {
      std::uint64_t value = 0;
      if (!parse_u64(0, 127, &value))
        return false;
      options->payload_type = static_cast<std::uint8_t>(value);
    } else if (arg == "--mtu") {
      std::uint64_t value = 0;
      if (!parse_u64(64, 65523, &value))
        return false;
      options->mtu = static_cast<std::size_t>(value);
    } else if (arg == "--fps") {
      std::uint64_t value = 0;
      if (!parse_u64(1, 1000, &value))
        return false;
      options->fps = static_cast<std::uint32_t>(value);
    } else if (arg == "--max-frames") {
      std::uint64_t value = 0;
      if (!parse_u64(0, 100000000, &value))
        return false;
      options->max_frames = value;
    } else if (arg == "--accept-timeout-ms") {
      std::uint64_t value = 0;
      if (!parse_u64(0, 3600000, &value))
        return false;
      options->accept_timeout_ms = static_cast<int>(value);
    } else if (arg == "--loop") {
      options->loop = true;
    } else if (arg == "--no-realtime") {
      options->realtime = false;
    } else if (arg == "--verbose") {
      options->verbose = true;
    } else if (arg == "--ps-out") {
      if (!next(&options->ps_out))
        return false;
    } else if (arg == "--sdp-out") {
      if (!next(&options->sdp_out))
        return false;
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
  if (!options->codec_set) {
    const std::string lower = options->input;
    if (lower.find(".h265") != std::string::npos || lower.find(".hevc") != std::string::npos ||
        lower.find(".265") != std::string::npos) {
      options->codec = PsVideoCodec::kH265;
    }
  }
  if (options->transport != RtpTransportMode::kTcpPassive) {
    if (options->remote_address.empty() || options->remote_port == 0) {
      *error = "udp and tcp-active transports need --remote and --port";
      return false;
    }
  }
  if (options->advertise_address.empty()) {
    options->advertise_address =
        options->local_address == "0.0.0.0" ? "127.0.0.1" : options->local_address;
  }
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

bool WriteFile(const std::string& path, const void* data, std::size_t size,
               std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    *error = "cannot open " + path;
    return false;
  }
  if (size > 0)
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!output) {
    *error = "cannot write " + path;
    return false;
  }
  return true;
}

std::string TransportName(RtpTransportMode mode) {
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

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, &options, &error)) {
    std::cerr << "gb28181_ps_cli: " << error << "\n\n";
    PrintUsage(argv[0]);
    return 2;
  }

  std::vector<std::uint8_t> elementary_stream;
  if (!ReadFile(options.input, &elementary_stream, &error)) {
    std::cerr << "gb28181_ps_cli: " << error << "\n";
    return 1;
  }
  std::vector<EsAccessUnit> access_units;
  if (!SplitAnnexBAccessUnits(elementary_stream.data(), elementary_stream.size(), options.codec,
                              &access_units, &error)) {
    std::cerr << "gb28181_ps_cli: cannot split elementary stream: " << error << "\n";
    return 1;
  }
  if (access_units.empty()) {
    std::cerr << "gb28181_ps_cli: input has no access units\n";
    return 1;
  }
  std::size_t keyframes = 0;
  for (const EsAccessUnit& unit : access_units) {
    if (unit.keyframe)
      ++keyframes;
  }
  std::cout << "input: " << options.input << " frames=" << access_units.size()
            << " keyframes=" << keyframes << " codec=" << PsVideoCodecName(options.codec)
            << " bytes=" << elementary_stream.size() << "\n";

  RtpSessionConfig session_config;
  session_config.mode = options.transport;
  session_config.local_address = options.local_address;
  session_config.local_port = options.local_port;
  session_config.remote_address = options.remote_address;
  session_config.remote_port = options.remote_port;
  session_config.payload_type = options.payload_type;
  session_config.ssrc = options.ssrc;
  session_config.max_payload_size = options.mtu;

  RtpSession session(session_config);
  if (!session.Open(&error)) {
    std::cerr << "gb28181_ps_cli: open failed: " << error << "\n";
    return 1;
  }
  if (options.transport == RtpTransportMode::kTcpPassive) {
    std::cout << "listening on " << options.local_address << ":" << session.local_port()
              << " (TCP passive)\n";
    // The listening line is the handshake with the test receiver (and with an
    // operator watching a log pipe); flush before blocking in Accept.
    std::cout.flush();
    std::string peer;
    if (!session.Accept(&peer, options.accept_timeout_ms, &error)) {
      std::cerr << "gb28181_ps_cli: accept failed: " << error << "\n";
      return 1;
    }
    std::cout << "accepted peer " << peer << "\n";
    std::cout.flush();
  } else {
    std::cout << "sending " << TransportName(options.transport) << " from "
              << session.local_address() << ":" << session.local_port() << " to "
              << options.remote_address << ":" << options.remote_port
              << " ssrc=0x" << std::hex << options.ssrc << std::dec
              << " payload_type=" << static_cast<int>(options.payload_type)
              << " mtu=" << options.mtu << "\n";
    std::cout.flush();
  }

  if (!options.sdp_out.empty()) {
    const std::string sdp =
        session.BuildSessionSdp(options.advertise_address, options.origin_id);
    if (sdp.empty() || !WriteFile(options.sdp_out, sdp.data(), sdp.size(), &error)) {
      std::cerr << "gb28181_ps_cli: cannot write SDP: " << error << "\n";
      return 1;
    }
    std::cout << "SDP written to " << options.sdp_out << "\n";
  }

  std::ofstream ps_output;
  if (!options.ps_out.empty()) {
    ps_output.open(options.ps_out, std::ios::binary | std::ios::trunc);
    if (!ps_output) {
      std::cerr << "gb28181_ps_cli: cannot open " << options.ps_out << "\n";
      return 1;
    }
  }

  PsMuxerConfig muxer_config;
  muxer_config.codec = options.codec;
  PsMuxer muxer(muxer_config);
  std::vector<std::uint8_t> ps;

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t frame_index = 0;
  std::uint64_t frames_sent = 0;
  bool ok = true;

  while (ok) {
    for (const EsAccessUnit& unit : access_units) {
      if (options.max_frames != 0 && frames_sent >= options.max_frames) {
        ok = false;
        break;
      }
      const std::uint64_t pts64 = frame_index * 90000 / options.fps;
      const std::int64_t pts = static_cast<std::int64_t>(pts64 & 0x1FFFFFFFFull);
      const PsFrame frame{unit.data.data(), unit.data.size(), pts, pts, false, unit.keyframe};
      if (!muxer.MuxFrame(frame, &ps, &error)) {
        std::cerr << "\ngb28181_ps_cli: PS mux failed: " << error << "\n";
        return 1;
      }
      const std::uint64_t packets_before = session.stats().packets;
      if (!session.SendFrame(ps.data(), ps.size(), static_cast<std::uint32_t>(pts),
                             unit.keyframe, &error)) {
        std::cerr << "\ngb28181_ps_cli: RTP send failed: " << error << "\n";
        return 1;
      }
      const std::uint64_t packets = session.stats().packets - packets_before;
      if (ps_output.is_open()) {
        ps_output.write(reinterpret_cast<const char*>(ps.data()),
                        static_cast<std::streamsize>(ps.size()));
        if (!ps_output) {
          std::cerr << "\ngb28181_ps_cli: write to " << options.ps_out << " failed\n";
          return 1;
        }
      }
      if (options.verbose) {
        std::printf("frame=%llu pts=%lld key=%d ps_bytes=%zu rtp_packets=%llu last_seq=%u "
                    "marker=1\n",
                    static_cast<unsigned long long>(frames_sent),
                    static_cast<long long>(pts), unit.keyframe ? 1 : 0, ps.size(),
                    static_cast<unsigned long long>(packets),
                    static_cast<unsigned>(session.stats().last_sequence));
        if (frames_sent == 0 || unit.keyframe) {
          std::cout << HexDump(ps.data(), ps.size(), 64);
        }
        std::cout.flush();
      }
      ++frames_sent;
      ++frame_index;

      if (options.realtime) {
        const auto deadline =
            start + std::chrono::microseconds(frame_index * 1000000 / options.fps);
        std::this_thread::sleep_until(deadline);
      }
    }
    if (!options.loop)
      break;
  }

  if (ps_output.is_open())
    ps_output.close();

  const RtpSendStats& stats = session.stats();
  const std::uint64_t first_pts = 0;
  const std::uint64_t last_pts =
      frames_sent == 0 ? 0 : static_cast<std::uint64_t>((frames_sent - 1) * 90000 / options.fps);
  std::printf("summary: frames=%llu keyframes=%llu ps_bytes=%llu rtp_packets=%llu "
              "marker_packets=%llu first_pts=%llu last_pts=%llu last_seq=%u ssrc=0x%08x\n",
              static_cast<unsigned long long>(stats.frames),
              static_cast<unsigned long long>(stats.keyframes),
              static_cast<unsigned long long>(stats.payload_bytes),
              static_cast<unsigned long long>(stats.packets),
              static_cast<unsigned long long>(stats.marker_packets),
              static_cast<unsigned long long>(first_pts),
              static_cast<unsigned long long>(last_pts),
              static_cast<unsigned>(stats.last_sequence), options.ssrc);
  return 0;
}
