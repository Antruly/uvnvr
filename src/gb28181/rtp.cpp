// RTP packetization, UDP/TCP transport and SDP for GB/T 28181 media.
//
// The packetizer is deliberately separated from the socket code: the tests
// inspect the complete RTP headers (version, payload type, sequence, timestamp,
// SSRC, marker) without opening a port. RtpSession then puts those packets on
// the wire. TCP uses the RFC 4571 two-byte length prefix that GB/T 28181
// platforms expect for "TCP passive" and "TCP active" media.

#include "gb28181/rtp.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <mutex>  // std::once_flag for WSAStartup
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/socket.h>
#endif
#endif

namespace uvnvr::gb28181 {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
const SocketHandle kInvalidSocket = INVALID_SOCKET;

int LastSocketError() { return WSAGetLastError(); }
void CloseSocketHandle(SocketHandle socket) {
  if (socket != kInvalidSocket)
    closesocket(socket);
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
const SocketHandle kInvalidSocket = -1;

int LastSocketError() { return errno; }
void CloseSocketHandle(SocketHandle socket) {
  if (socket != kInvalidSocket)
    ::close(socket);
}
#endif

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

bool EnsureSockets(std::string* error) {
#ifdef _WIN32
  static std::once_flag once;
  static int startup_result = 0;
  std::call_once(once, []() {
    WSADATA data{};
    startup_result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (startup_result != 0) {
    if (error)
      *error = "WSAStartup failed with error " + std::to_string(startup_result);
    return false;
  }
#else
  (void)error;
#endif
  return true;
}

std::string SocketErrorText(int code) {
#ifdef _WIN32
  return "socket error " + std::to_string(code);
#else
  return std::strerror(code);
#endif
}

std::string ToLowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

std::string_view Trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size())
    return false;
  return ToLowerAscii(text.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

std::vector<std::string> SplitWhitespace(std::string_view text) {
  std::vector<std::string> tokens;
  std::size_t position = 0;
  while (position < text.size()) {
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position])))
      ++position;
    if (position >= text.size())
      break;
    const std::size_t begin = position;
    while (position < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[position])))
      ++position;
    tokens.emplace_back(text.substr(begin, position - begin));
  }
  return tokens;
}

bool ParseUnsigned(std::string_view text, std::uint32_t max_value, std::uint32_t* value) {
  if (text.empty())
    return false;
  std::uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9')
      return false;
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
    if (result > max_value)
      return false;
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

bool ResolveIpv4(const std::string& host, std::uint16_t port, int sock_type, bool passive,
                 sockaddr_in* address, std::string* error) {
  std::memset(address, 0, sizeof(*address));
  address->sin_family = AF_INET;
  address->sin_port = htons(port);
  if (host.empty() || host == "0.0.0.0" || host == "*") {
    if (!passive) {
      if (error)
        *error = "a destination address is required";
      return false;
    }
    address->sin_addr.s_addr = htonl(INADDR_ANY);
    return true;
  }
  if (inet_pton(AF_INET, host.c_str(), &address->sin_addr) == 1)
    return true;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = sock_type;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  addrinfo* result = nullptr;
  const int status = getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (status != 0 || result == nullptr) {
#ifdef _WIN32
    const std::string reason = "getaddrinfo error " + std::to_string(status);
#else
    const std::string reason = gai_strerror(status);
#endif
    if (error)
      *error = "cannot resolve " + host + ": " + reason;
    if (result)
      freeaddrinfo(result);
    return false;
  }
  address->sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
  freeaddrinfo(result);
  return true;
}

std::string AddressToString(const sockaddr_in& address) {
  char text[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr)
    return {};
  return text;
}

std::uint16_t BoundPort(SocketHandle socket) {
  sockaddr_in address{};
  SocketLength length = static_cast<SocketLength>(sizeof(address));
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    return 0;
  return ntohs(address.sin_port);
}

bool BindSocket(SocketHandle socket, const std::string& address, std::uint16_t port,
                int sock_type, std::string* error) {
  sockaddr_in bind_address{};
  if (!ResolveIpv4(address, port, sock_type, true, &bind_address, error))
    return false;
  if (bind(socket, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
    if (error)
      *error = "bind failed: " + SocketErrorText(LastSocketError());
    return false;
  }
  return true;
}

void SetReuseAddress(SocketHandle socket) {
  int one = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
             sizeof(one));
}

void SetNoDelay(SocketHandle socket) {
  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
             sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one),
             sizeof(one));
#endif
}

bool SendAll(SocketHandle socket, const std::uint8_t* data, std::size_t size,
             std::string* error) {
  std::size_t sent = 0;
  while (sent < size) {
#ifdef _WIN32
    const int result = send(socket, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(size - sent), 0);
#else
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    const ssize_t result = send(socket, data + sent, size - sent, send_flags);
#endif
    if (result <= 0) {
      if (error)
        *error = "send failed: " + SocketErrorText(LastSocketError());
      return false;
    }
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

std::vector<std::string> SplitSdpLines(std::string_view sdp) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= sdp.size()) {
    const std::size_t end = sdp.find('\n', begin);
    std::string line(sdp.substr(begin, end == std::string_view::npos ? std::string_view::npos
                                                                     : end - begin));
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return lines;
}

std::int64_t MonotonicMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SystemNtpNow(std::uint32_t* seconds, std::uint32_t* fraction) {
  using namespace std::chrono;
  const std::int64_t unix_ms =
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  const std::int64_t unix_seconds = unix_ms / 1000;
  const std::int64_t millis = unix_ms % 1000;
  // The NTP epoch starts 2208988800 seconds before the Unix epoch.
  *seconds = static_cast<std::uint32_t>(unix_seconds + 2208988800LL);
  *fraction = static_cast<std::uint32_t>((millis * 4294967296LL) / 1000);
}

}  // namespace

const char* RtpSetupName(RtpSetup setup) {
  switch (setup) {
    case RtpSetup::kActive:
      return "active";
    case RtpSetup::kPassive:
      return "passive";
    case RtpSetup::kNone:
      break;
  }
  return "";
}

bool ParseRtpSetup(std::string_view text, RtpSetup* setup) {
  if (setup == nullptr)
    return false;
  const std::string lower = ToLowerAscii(Trim(text));
  if (lower == "active") {
    *setup = RtpSetup::kActive;
    return true;
  }
  if (lower == "passive") {
    *setup = RtpSetup::kPassive;
    return true;
  }
  return false;
}

bool ParseRtpPacket(const std::uint8_t* data, std::size_t size, RtpPacketHeader* header,
                    std::string* error) {
  if (header == nullptr) {
    if (error)
      *error = "RTP header output is null";
    return false;
  }
  *header = RtpPacketHeader{};
  if (data == nullptr || size < 12) {
    if (error)
      *error = "RTP packet is shorter than 12 bytes";
    return false;
  }
  const std::uint8_t version = data[0] >> 6;
  if (version != 2) {
    if (error)
      *error = "unsupported RTP version";
    return false;
  }
  const bool padding = (data[0] & 0x20) != 0;
  const bool extension = (data[0] & 0x10) != 0;
  const std::size_t csrc_count = data[0] & 0x0F;
  std::size_t header_size = 12 + 4 * csrc_count;
  if (header_size > size) {
    if (error)
      *error = "RTP CSRC list runs past the packet";
    return false;
  }
  if (extension) {
    if (header_size + 4 > size) {
      if (error)
        *error = "RTP extension header is truncated";
      return false;
    }
    const std::uint16_t extension_words =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[header_size + 2]) << 8) |
                                   data[header_size + 3]);
    header_size += 4 + 4 * static_cast<std::size_t>(extension_words);
    if (header_size > size) {
      if (error)
        *error = "RTP extension runs past the packet";
      return false;
    }
  }
  std::size_t padding_size = 0;
  if (padding) {
    padding_size = data[size - 1];
    if (padding_size > size - header_size) {
      if (error)
        *error = "RTP padding is larger than the payload";
      return false;
    }
  }
  header->marker = (data[1] & 0x80) != 0;
  header->payload_type = data[1] & 0x7F;
  header->sequence = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[2]) << 8) |
                                                data[3]);
  header->timestamp = (static_cast<std::uint32_t>(data[4]) << 24) |
                      (static_cast<std::uint32_t>(data[5]) << 16) |
                      (static_cast<std::uint32_t>(data[6]) << 8) |
                      static_cast<std::uint32_t>(data[7]);
  header->ssrc = (static_cast<std::uint32_t>(data[8]) << 24) |
                 (static_cast<std::uint32_t>(data[9]) << 16) |
                 (static_cast<std::uint32_t>(data[10]) << 8) |
                 static_cast<std::uint32_t>(data[11]);
  header->header_size = header_size;
  header->payload_size = size - header_size - padding_size;
  return true;
}

RtpPacketizer::RtpPacketizer(RtpPacketizerConfig config) : config_(config) {
  if (config_.payload_type > 127)
    config_.payload_type = 96;
  if (config_.max_payload_size == 0 || config_.max_payload_size > 65535 - 12)
    config_.max_payload_size = 1400;
  next_sequence_ = config_.initial_sequence;
}

void RtpPacketizer::Reset() {
  next_sequence_ = config_.initial_sequence;
  packets_sent_ = 0;
}

bool RtpPacketizer::PacketizeFrame(
    const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp_90k,
    std::vector<std::vector<std::uint8_t>>* packets, std::string* error) {
  if (packets == nullptr) {
    if (error)
      *error = "RTP packet output is null";
    return false;
  }
  packets->clear();
  if (ps == nullptr || size == 0) {
    if (error)
      *error = "empty PS frame";
    return false;
  }

  const std::size_t packet_count =
      (size + config_.max_payload_size - 1) / config_.max_payload_size;
  packets->reserve(packet_count);
  std::size_t offset = 0;
  for (std::size_t index = 0; index < packet_count; ++index) {
    const std::size_t payload_size = std::min(config_.max_payload_size, size - offset);
    const bool marker = index + 1 == packet_count;
    std::vector<std::uint8_t> packet(12 + payload_size);
    packet[0] = 0x80;  // version 2, no padding/extension/CSRC
    packet[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) |
                                          (config_.payload_type & 0x7F));
    packet[2] = static_cast<std::uint8_t>(next_sequence_ >> 8);
    packet[3] = static_cast<std::uint8_t>(next_sequence_ & 0xFF);
    packet[4] = static_cast<std::uint8_t>(timestamp_90k >> 24);
    packet[5] = static_cast<std::uint8_t>((timestamp_90k >> 16) & 0xFF);
    packet[6] = static_cast<std::uint8_t>((timestamp_90k >> 8) & 0xFF);
    packet[7] = static_cast<std::uint8_t>(timestamp_90k & 0xFF);
    packet[8] = static_cast<std::uint8_t>(config_.ssrc >> 24);
    packet[9] = static_cast<std::uint8_t>((config_.ssrc >> 16) & 0xFF);
    packet[10] = static_cast<std::uint8_t>((config_.ssrc >> 8) & 0xFF);
    packet[11] = static_cast<std::uint8_t>(config_.ssrc & 0xFF);
    std::memcpy(packet.data() + 12, ps + offset, payload_size);
    packets->push_back(std::move(packet));
    offset += payload_size;
    ++next_sequence_;
    ++packets_sent_;
  }
  return true;
}

std::string BuildSdp(const SdpVideoSession& session) {
  if (session.address.empty() || session.port == 0 || session.payload_type > 127 ||
      session.clock_rate == 0)
    return {};

  RtpSetup setup = session.setup;
  if (session.mode != RtpTransportMode::kUdp && setup == RtpSetup::kNone) {
    setup = session.mode == RtpTransportMode::kTcpPassive ? RtpSetup::kPassive
                                                          : RtpSetup::kActive;
  }
  const bool tcp = session.mode != RtpTransportMode::kUdp;
  const std::string origin = session.origin_id.empty() ? "-" : session.origin_id;
  const std::string name = session.session_name.empty() ? "Play" : session.session_name;
  const std::string encoding = session.encoding.empty() ? "PS" : session.encoding;
  const std::string direction = session.direction;

  std::string sdp;
  sdp += "v=0\r\n";
  sdp += "o=" + origin + " 0 0 IN IP4 " + session.address + "\r\n";
  sdp += "s=" + name + "\r\n";
  sdp += "c=IN IP4 " + session.address + "\r\n";
  sdp += "t=0 0\r\n";
  sdp += "m=video " + std::to_string(session.port) + (tcp ? " TCP/RTP/AVP " : " RTP/AVP ") +
         std::to_string(session.payload_type) + "\r\n";
  if (!direction.empty())
    sdp += "a=" + direction + "\r\n";
  sdp += "a=rtpmap:" + std::to_string(session.payload_type) + " " + encoding + "/" +
         std::to_string(session.clock_rate) + "\r\n";
  if (tcp) {
    sdp += "a=setup:" + std::string(RtpSetupName(setup)) + "\r\n";
    if (session.connection_new)
      sdp += "a=connection:new\r\n";
  }
  if (session.has_ssrc) {
    char line[32];
    std::snprintf(line, sizeof(line), "y=%010u\r\n", session.ssrc);
    sdp += line;
  }
  return sdp;
}

bool ParseSdp(std::string_view sdp, SdpVideoSession* session, std::string* error) {
  if (session == nullptr) {
    if (error)
      *error = "SDP output is null";
    return false;
  }
  *session = SdpVideoSession{};
  // The struct default ("sendonly") describes what BuildSdp emits, but a
  // parsed session without an a= direction attribute is sendrecv per RFC 4566.
  session->direction.clear();
  bool has_version = false;
  bool found_video = false;
  bool found_ps = false;
  bool in_video = false;
  bool seen_media = false;
  bool is_tcp = false;
  RtpSetup setup = RtpSetup::kNone;

  for (const std::string& raw_line : SplitSdpLines(sdp)) {
    const std::string_view line = Trim(raw_line);
    if (line.size() < 2 || line[1] != '=')
      continue;
    const char type = line[0];
    const std::string value(line.substr(2));

    if (type == 'v') {
      has_version = true;
      if (value != "0") {
        if (error)
          *error = "only SDP version 0 is supported";
        return false;
      }
      continue;
    }
    if (type == 'o') {
      const std::vector<std::string> tokens = SplitWhitespace(value);
      if (!tokens.empty() && session->origin_id.empty())
        session->origin_id = tokens[0];
      continue;
    }
    if (type == 's') {
      session->session_name = value;
      continue;
    }
    if (type == 'c') {
      // A global c= line applies to the first media section; a media-level
      // c= line only applies while that section is the video section.
      if (seen_media && !in_video)
        continue;
      const std::vector<std::string> tokens = SplitWhitespace(value);
      if (tokens.size() >= 3) {
        std::string address = tokens[2];
        const std::size_t slash = address.find('/');
        if (slash != std::string::npos)
          address.resize(slash);
        session->address = std::move(address);
      }
      continue;
    }
    if (type == 'y') {
      std::uint32_t ssrc = 0;
      const std::string_view text = Trim(value);
      if (!ParseUnsigned(text, std::numeric_limits<std::uint32_t>::max(), &ssrc)) {
        if (error)
          *error = "invalid SDP y= SSRC";
        return false;
      }
      session->has_ssrc = true;
      session->ssrc = ssrc;
      continue;
    }
    if (type == 'm') {
      seen_media = true;
      const std::vector<std::string> tokens = SplitWhitespace(value);
      if (tokens.size() < 4) {
        if (error)
          *error = "malformed SDP media line";
        return false;
      }
      in_video = ToLowerAscii(tokens[0]) == "video";
      if (!in_video)
        continue;
      std::uint32_t port = 0;
      const std::string port_text = tokens[1].substr(0, tokens[1].find('/'));
      if (!ParseUnsigned(port_text, 65535, &port) || port == 0) {
        if (error)
          *error = "invalid SDP media port";
        return false;
      }
      session->port = static_cast<std::uint16_t>(port);
      is_tcp = ToLowerAscii(tokens[2]).find("tcp") != std::string::npos;
      found_video = true;
      continue;
    }
    if (type != 'a' || !in_video)
      continue;

    if (StartsWithIgnoreCase(value, "rtpmap:")) {
      const std::string_view rest = Trim(std::string_view(value).substr(7));
      const std::size_t space = rest.find_first_of(" \t");
      if (space == std::string_view::npos)
        continue;
      std::uint32_t payload_type = 0;
      if (!ParseUnsigned(rest.substr(0, space), 127, &payload_type))
        continue;
      const std::string_view encoding_text = Trim(rest.substr(space + 1));
      const std::size_t slash = encoding_text.find('/');
      if (slash == std::string_view::npos)
        continue;
      const std::string encoding(Trim(encoding_text.substr(0, slash)));
      std::uint32_t clock_rate = 0;
      if (!ParseUnsigned(Trim(encoding_text.substr(slash + 1)), 1000000, &clock_rate))
        continue;
      if (ToLowerAscii(encoding) == "ps") {
        session->payload_type = static_cast<std::uint8_t>(payload_type);
        session->encoding = "PS";
        session->clock_rate = clock_rate;
        found_ps = true;
      }
      continue;
    }
    if (StartsWithIgnoreCase(value, "setup:")) {
      if (!ParseRtpSetup(std::string_view(value).substr(6), &setup)) {
        if (error)
          *error = "unsupported SDP a=setup value";
        return false;
      }
      continue;
    }
    if (ToLowerAscii(value) == "connection:new") {
      session->connection_new = true;
      continue;
    }
    const std::string lower = ToLowerAscii(value);
    if (lower == "sendonly" || lower == "recvonly" || lower == "sendrecv") {
      session->direction = lower;
      continue;
    }
  }

  if (has_version == false) {
    if (error)
      *error = "SDP is missing v=0";
    return false;
  }
  if (!found_video || session->port == 0) {
    if (error)
      *error = "SDP has no video media line";
    return false;
  }
  if (!found_ps) {
    if (error)
      *error = "SDP has no PS/90000 rtpmap";
    return false;
  }
  if (session->address.empty()) {
    if (error)
      *error = "SDP has no connection address";
    return false;
  }
  if (session->direction.empty())
    session->direction = "sendrecv";
  if (is_tcp) {
    if (setup == RtpSetup::kPassive)
      session->mode = RtpTransportMode::kTcpPassive;
    else if (setup == RtpSetup::kActive)
      session->mode = RtpTransportMode::kTcpActive;
    else
      session->mode = RtpTransportMode::kTcpActive;
    session->setup = setup;
  } else {
    session->mode = RtpTransportMode::kUdp;
    session->setup = RtpSetup::kNone;
    session->connection_new = false;
  }
  return true;
}

bool BuildSdpAnswer(const SdpVideoSession& offer, const std::string& local_address,
                    std::uint16_t local_port, const std::string& local_id, std::uint32_t ssrc,
                    std::string* sdp, SdpVideoSession* answer, std::string* error) {
  if (sdp == nullptr) {
    if (error)
      *error = "SDP output is null";
    return false;
  }
  if (ToLowerAscii(offer.encoding) != "ps" || offer.clock_rate != 90000) {
    if (error)
      *error = "offer is not PS/90000";
    return false;
  }
  if (local_address.empty() || local_port == 0) {
    if (error)
      *error = "answer address and port are required";
    return false;
  }

  SdpVideoSession result = offer;
  result.address = local_address;
  result.port = local_port;
  result.origin_id = local_id.empty() ? offer.origin_id : local_id;
  if (result.session_name.empty())
    result.session_name = "Play";
  result.direction = "sendonly";
  result.has_ssrc = true;
  result.ssrc = ssrc;
  if (offer.mode == RtpTransportMode::kUdp) {
    result.mode = RtpTransportMode::kUdp;
    result.setup = RtpSetup::kNone;
    result.connection_new = false;
  } else if (offer.setup == RtpSetup::kPassive) {
    result.mode = RtpTransportMode::kTcpActive;
    result.setup = RtpSetup::kActive;
    result.connection_new = true;
  } else {
    // active or unspecified: the device listens.
    result.mode = RtpTransportMode::kTcpPassive;
    result.setup = RtpSetup::kPassive;
    result.connection_new = true;
  }
  const std::string text = BuildSdp(result);
  if (text.empty()) {
    if (error)
      *error = "cannot build the SDP answer";
    return false;
  }
  *sdp = text;
  if (answer)
    *answer = result;
  return true;
}

struct RtpSession::Impl {
  explicit Impl(const RtpSessionConfig& config)
      : packetizer(RtpPacketizerConfig{config.payload_type, config.ssrc,
                                       config.initial_sequence, config.max_payload_size}),
        mode(config.mode),
        enable_rtcp(config.enable_rtcp),
        media_ssrc(config.ssrc),
        sender_report_interval_ms(config.sender_report_interval_ms) {}

  bool SendRtcpPayload(const std::vector<std::uint8_t>& bytes, std::string* error);
  void HandleRtcpPacket(const RtcpPacket& packet, std::int64_t now_ms);

  SocketHandle fd = kInvalidSocket;
  SocketHandle accepted = kInvalidSocket;
  sockaddr_in remote{};
  bool has_remote = false;
  bool listening = false;
  std::uint16_t bound_port = 0;
  std::string peer;
  RtpPacketizer packetizer;

  RtpTransportMode mode = RtpTransportMode::kUdp;
  bool enable_rtcp = false;
  std::uint32_t media_ssrc = 0;
  std::uint32_t sender_report_interval_ms = 5000;

  SocketHandle rtcp_fd = kInvalidSocket;
  sockaddr_in rtcp_remote{};
  bool has_rtcp_remote = false;
  bool rtcp_via_tcp = false;
  std::uint16_t rtcp_bound_port = 0;
  std::string rtcp_peer;
  std::vector<std::uint8_t> tcp_rtcp_buffer;

  RtcpStats rtcp_stats;
  std::function<void(const RtcpPacket&)> rtcp_callback;
  std::int64_t last_sr_at_ms = -1;
  std::int64_t last_sr_wall_ms = -1;
  std::uint32_t last_sr_compact_ntp = 0;
};

bool RtpSession::Impl::SendRtcpPayload(const std::vector<std::uint8_t>& bytes,
                                       std::string* error) {
  if (bytes.empty()) {
    if (error)
      *error = "empty RTCP packet";
    return false;
  }
  if (rtcp_via_tcp) {
    const SocketHandle target = mode == RtpTransportMode::kTcpPassive ? accepted : fd;
    if (target == kInvalidSocket) {
      if (error)
        *error = "TCP media connection is not open";
      return false;
    }
    std::vector<std::uint8_t> framed(2 + bytes.size());
    framed[0] = static_cast<std::uint8_t>(bytes.size() >> 8);
    framed[1] = static_cast<std::uint8_t>(bytes.size() & 0xFF);
    std::memcpy(framed.data() + 2, bytes.data(), bytes.size());
    return SendAll(target, framed.data(), framed.size(), error);
  }
  if (rtcp_fd == kInvalidSocket || !has_rtcp_remote) {
    if (error)
      *error = "RTCP UDP socket is not open";
    return false;
  }
#ifdef _WIN32
  const int sent = sendto(rtcp_fd, reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()), 0,
                          reinterpret_cast<const sockaddr*>(&rtcp_remote),
                          sizeof(rtcp_remote));
#else
  const ssize_t sent = sendto(rtcp_fd, bytes.data(), bytes.size(), 0,
                              reinterpret_cast<const sockaddr*>(&rtcp_remote),
                              sizeof(rtcp_remote));
#endif
  if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
    if (error)
      *error = "RTCP UDP send failed: " + SocketErrorText(LastSocketError());
    return false;
  }
  return true;
}

void RtpSession::Impl::HandleRtcpPacket(const RtcpPacket& packet, std::int64_t now_ms) {
  ++rtcp_stats.packets_received;
  switch (packet.payload_type) {
    case kRtcpPtSenderReport:
      ++rtcp_stats.sender_reports_received;
      break;
    case kRtcpPtReceiverReport:
      ++rtcp_stats.receiver_reports_received;
      rtcp_stats.receiver_ssrc = packet.sender_ssrc;
      break;
    case kRtcpPtSdes:
      ++rtcp_stats.sdes_received;
      break;
    case kRtcpPtBye:
      ++rtcp_stats.byes_received;
      break;
    default:
      break;
  }

  if (packet.payload_type == kRtcpPtSenderReport ||
      packet.payload_type == kRtcpPtReceiverReport) {
    for (const RtcpReportBlock& block : packet.report_blocks) {
      if (block.ssrc != 0 && block.ssrc != media_ssrc)
        continue;
      rtcp_stats.last_fraction_lost = block.fraction_lost;
      rtcp_stats.last_cumulative_lost = block.cumulative_lost;
      rtcp_stats.last_jitter = block.jitter;
      rtcp_stats.last_extended_highest_sequence = block.extended_highest_sequence;
      if (last_sr_wall_ms >= 0 && block.last_sender_report != 0 &&
          block.last_sender_report == last_sr_compact_ntp) {
        const std::int64_t elapsed = now_ms - last_sr_wall_ms;
        const std::int64_t rtt =
            elapsed - static_cast<std::int64_t>(RtcpDlsrToMs(block.delay_since_last_sender_report));
        if (rtt >= 0 && rtt <= 600000) {
          rtcp_stats.last_rtt_ms = static_cast<std::uint32_t>(rtt);
          rtcp_stats.has_rtt = true;
        }
      }
    }
  }

  if (packet.is_keyframe_request) {
    ++rtcp_stats.keyframe_requests_received;
    if (packet.is_fir)
      ++rtcp_stats.fir_received;
    else
      ++rtcp_stats.pli_received;
  }

  if (rtcp_callback)
    rtcp_callback(packet);
}

RtpSession::RtpSession(RtpSessionConfig config)
    : impl_(std::make_unique<Impl>(config)), config_(std::move(config)) {}

RtpSession::~RtpSession() { Close(); }

void RtpSession::Close() {
  if (impl_->accepted != kInvalidSocket) {
    CloseSocketHandle(impl_->accepted);
    impl_->accepted = kInvalidSocket;
  }
  if (impl_->fd != kInvalidSocket) {
    CloseSocketHandle(impl_->fd);
    impl_->fd = kInvalidSocket;
  }
  if (impl_->rtcp_fd != kInvalidSocket) {
    CloseSocketHandle(impl_->rtcp_fd);
    impl_->rtcp_fd = kInvalidSocket;
  }
  impl_->has_remote = false;
  impl_->has_rtcp_remote = false;
  impl_->listening = false;
  impl_->rtcp_via_tcp = false;
  impl_->bound_port = 0;
  impl_->rtcp_bound_port = 0;
  impl_->peer.clear();
  impl_->rtcp_peer.clear();
  impl_->tcp_rtcp_buffer.clear();
  impl_->packetizer.Reset();
}

bool RtpSession::Open(std::string* error) {
  Close();
  if (!EnsureSockets(error))
    return false;

  if (config_.mode == RtpTransportMode::kUdp) {
    if (!config_.enable_rtcp) {
      impl_->fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (impl_->fd == kInvalidSocket) {
        if (error)
          *error = "cannot create UDP socket: " + SocketErrorText(LastSocketError());
        return false;
      }
      SetReuseAddress(impl_->fd);
      if (!BindSocket(impl_->fd, config_.local_address, config_.local_port, SOCK_DGRAM,
                      error)) {
        Close();
        return false;
      }
      if (!ResolveIpv4(config_.remote_address, config_.remote_port, SOCK_DGRAM, false,
                       &impl_->remote, error)) {
        Close();
        return false;
      }
      impl_->has_remote = true;
      impl_->bound_port = BoundPort(impl_->fd);
      impl_->peer = AddressToString(impl_->remote);
      return true;
    }

    // RTCP needs a second socket. RFC 3550 expects an even RTP port followed
    // by RTCP on the next (odd) port; when the caller asks for an ephemeral
    // port, retry until that pair can be bound.
    const int attempts = config_.local_port == 0 ? 32 : 1;
    std::string last_error = "cannot bind a UDP RTP/RTCP port pair";
    for (int attempt = 0; attempt < attempts; ++attempt) {
      impl_->fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (impl_->fd == kInvalidSocket) {
        last_error = "cannot create UDP socket: " + SocketErrorText(LastSocketError());
        break;
      }
      SetReuseAddress(impl_->fd);
      if (!BindSocket(impl_->fd, config_.local_address, config_.local_port, SOCK_DGRAM,
                      &last_error)) {
        CloseSocketHandle(impl_->fd);
        impl_->fd = kInvalidSocket;
        if (config_.local_port != 0)
          break;
        continue;
      }
      const std::uint16_t rtp_port = BoundPort(impl_->fd);
      if (config_.local_port == 0 && (rtp_port & 1u)) {
        CloseSocketHandle(impl_->fd);
        impl_->fd = kInvalidSocket;
        continue;
      }
      if (rtp_port == 0 || rtp_port == 65535) {
        CloseSocketHandle(impl_->fd);
        impl_->fd = kInvalidSocket;
        continue;
      }

      const std::uint16_t rtcp_port =
          config_.rtcp_local_port != 0 ? config_.rtcp_local_port
                                       : static_cast<std::uint16_t>(rtp_port + 1);
      impl_->rtcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (impl_->rtcp_fd == kInvalidSocket) {
        last_error = "cannot create the RTCP UDP socket: " +
                     SocketErrorText(LastSocketError());
        CloseSocketHandle(impl_->fd);
        impl_->fd = kInvalidSocket;
        break;
      }
      SetReuseAddress(impl_->rtcp_fd);
      if (!BindSocket(impl_->rtcp_fd, config_.local_address, rtcp_port, SOCK_DGRAM,
                      &last_error)) {
        CloseSocketHandle(impl_->rtcp_fd);
        impl_->rtcp_fd = kInvalidSocket;
        CloseSocketHandle(impl_->fd);
        impl_->fd = kInvalidSocket;
        if (config_.local_port != 0)
          break;
        continue;
      }

      if (!ResolveIpv4(config_.remote_address, config_.remote_port, SOCK_DGRAM, false,
                       &impl_->remote, &last_error)) {
        Close();
        if (error)
          *error = last_error;
        return false;
      }
      std::uint16_t remote_rtcp_port =
          config_.rtcp_remote_port != 0
              ? config_.rtcp_remote_port
              : static_cast<std::uint16_t>(config_.remote_port == 65535
                                               ? 65534
                                               : config_.remote_port + 1);
      if (!ResolveIpv4(config_.remote_address, remote_rtcp_port, SOCK_DGRAM, false,
                       &impl_->rtcp_remote, &last_error)) {
        Close();
        if (error)
          *error = last_error;
        return false;
      }

      impl_->has_remote = true;
      impl_->has_rtcp_remote = true;
      impl_->rtcp_via_tcp = false;
      impl_->bound_port = rtp_port;
      impl_->rtcp_bound_port = BoundPort(impl_->rtcp_fd);
      impl_->peer = AddressToString(impl_->remote);
      impl_->rtcp_peer = AddressToString(impl_->rtcp_remote);
      return true;
    }

    Close();
    if (error)
      *error = last_error;
    return false;
  }

  impl_->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (impl_->fd == kInvalidSocket) {
    if (error)
      *error = "cannot create TCP socket: " + SocketErrorText(LastSocketError());
    return false;
  }
  SetNoDelay(impl_->fd);

  if (config_.mode == RtpTransportMode::kTcpPassive) {
    SetReuseAddress(impl_->fd);
    if (!BindSocket(impl_->fd, config_.local_address, config_.local_port, SOCK_STREAM,
                    error)) {
      Close();
      return false;
    }
    if (listen(impl_->fd, 1) != 0) {
      if (error)
        *error = "listen failed: " + SocketErrorText(LastSocketError());
      Close();
      return false;
    }
    impl_->bound_port = BoundPort(impl_->fd);
    impl_->listening = true;
    if (config_.enable_rtcp) {
      impl_->rtcp_via_tcp = true;
      impl_->rtcp_bound_port = impl_->bound_port;
    }
    return true;
  }

  // kTcpActive: bind the requested local endpoint (possibly all-zero) so the
  // selected source port can be reported in SDP, then connect. SO_REUSEADDR
  // lets a dialog reuse its port after the previous TCP connection closed.
  SetReuseAddress(impl_->fd);
  if (!BindSocket(impl_->fd, config_.local_address, config_.local_port, SOCK_STREAM, error)) {
    Close();
    return false;
  }
  sockaddr_in remote{};
  if (!ResolveIpv4(config_.remote_address, config_.remote_port, SOCK_STREAM, false, &remote,
                   error)) {
    Close();
    return false;
  }
  if (connect(impl_->fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
    if (error)
      *error = "connect failed: " + SocketErrorText(LastSocketError());
    Close();
    return false;
  }
  impl_->remote = remote;
  impl_->has_remote = true;
  impl_->bound_port = BoundPort(impl_->fd);
  impl_->peer = AddressToString(remote);
  if (config_.enable_rtcp) {
    impl_->rtcp_via_tcp = true;
    impl_->rtcp_bound_port = impl_->bound_port;
    impl_->rtcp_peer = impl_->peer;
  }
  return true;
}

bool RtpSession::Accept(std::string* peer, int timeout_ms, std::string* error) {
  if (config_.mode != RtpTransportMode::kTcpPassive) {
    if (error)
      *error = "Accept is only valid for TCP passive media";
    return false;
  }
  if (impl_->accepted != kInvalidSocket) {
    if (peer)
      *peer = impl_->peer;
    return true;
  }
  if (!impl_->listening || impl_->fd == kInvalidSocket) {
    if (error)
      *error = "TCP passive listener is not open";
    return false;
  }

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(impl_->fd, &read_set);
  timeval timeout{};
  timeval* timeout_ptr = nullptr;
  if (timeout_ms >= 0) {
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    timeout_ptr = &timeout;
  }
  const int ready =
      select(static_cast<int>(impl_->fd) + 1, &read_set, nullptr, nullptr, timeout_ptr);
  if (ready == 0) {
    if (error)
      *error = "timed out waiting for the TCP media connection";
    return false;
  }
  if (ready < 0) {
    if (error)
      *error = "select failed: " + SocketErrorText(LastSocketError());
    return false;
  }

  sockaddr_in peer_address{};
  SocketLength peer_length = static_cast<SocketLength>(sizeof(peer_address));
  impl_->accepted =
      accept(impl_->fd, reinterpret_cast<sockaddr*>(&peer_address), &peer_length);
  if (impl_->accepted == kInvalidSocket) {
    if (error)
      *error = "accept failed: " + SocketErrorText(LastSocketError());
    return false;
  }
  SetNoDelay(impl_->accepted);
  impl_->peer = AddressToString(peer_address);
  impl_->rtcp_peer = impl_->peer;
  if (peer)
    *peer = impl_->peer;
  return true;
}

bool RtpSession::SendFrame(const std::uint8_t* ps, std::size_t size,
                           std::uint32_t timestamp_90k, bool keyframe,
                           std::string* error) {
  if (!is_open()) {
    if (error)
      *error = "RTP session is not connected";
    return false;
  }
  std::vector<std::vector<std::uint8_t>> packets;
  if (!impl_->packetizer.PacketizeFrame(ps, size, timestamp_90k, &packets, error))
    return false;

  for (const std::vector<std::uint8_t>& packet : packets) {
    if (config_.mode == RtpTransportMode::kUdp) {
#ifdef _WIN32
      const int sent = sendto(impl_->fd, reinterpret_cast<const char*>(packet.data()),
                              static_cast<int>(packet.size()), 0,
                              reinterpret_cast<const sockaddr*>(&impl_->remote),
                              sizeof(impl_->remote));
#else
      const ssize_t sent = sendto(impl_->fd, packet.data(), packet.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&impl_->remote),
                                  sizeof(impl_->remote));
#endif
      if (sent < 0 || static_cast<std::size_t>(sent) != packet.size()) {
        if (error)
          *error = "UDP send failed: " + SocketErrorText(LastSocketError());
        return false;
      }
    } else {
      std::vector<std::uint8_t> framed(2 + packet.size());
      framed[0] = static_cast<std::uint8_t>(packet.size() >> 8);
      framed[1] = static_cast<std::uint8_t>(packet.size() & 0xFF);
      std::memcpy(framed.data() + 2, packet.data(), packet.size());
      const SocketHandle target = config_.mode == RtpTransportMode::kTcpPassive
                                      ? impl_->accepted
                                      : impl_->fd;
      if (!SendAll(target, framed.data(), framed.size(), error))
        return false;
    }
  }

  ++stats_.frames;
  stats_.packets += packets.size();
  stats_.payload_bytes += size;
  if (keyframe)
    ++stats_.keyframes;
  if (!packets.empty())
    ++stats_.marker_packets;
  stats_.last_sequence = impl_->packetizer.next_sequence() - 1;
  stats_.last_timestamp = timestamp_90k;
  return true;
}

void RtpSession::SetRtcpEventCallback(std::function<void(const RtcpPacket&)> callback) {
  impl_->rtcp_callback = std::move(callback);
}

bool RtpSession::SendSenderReport(std::string* error) {
  if (!config_.enable_rtcp) {
    if (error)
      *error = "RTCP is disabled for this session";
    return false;
  }
  if (!is_open()) {
    if (error)
      *error = "RTP session is not connected";
    return false;
  }

  std::uint32_t ntp_seconds = 0;
  std::uint32_t ntp_fraction = 0;
  SystemNtpNow(&ntp_seconds, &ntp_fraction);

  RtcpSenderInfo info;
  info.ntp_seconds = ntp_seconds;
  info.ntp_fraction = ntp_fraction;
  info.rtp_timestamp = stats_.last_timestamp;
  info.packet_count = static_cast<std::uint32_t>(stats_.packets);
  info.octet_count = static_cast<std::uint32_t>(stats_.payload_bytes);

  std::string cname = config_.rtcp_cname;
  if (cname.empty()) {
    cname = "uvnvr@";
    cname += impl_->bound_port != 0 ? AddressToString(impl_->remote) : std::string("local");
    if (cname == "uvnvr@")
      cname = "uvnvr@local";
    else if (cname == "uvnvr@0.0.0.0")
      cname = "uvnvr@local";
  }
  const std::vector<std::uint8_t> bytes =
      BuildRtcpSenderReportCompound(config_.ssrc, info, cname);
  if (bytes.empty()) {
    if (error)
      *error = "cannot build the RTCP sender report";
    return false;
  }
  if (!impl_->SendRtcpPayload(bytes, error))
    return false;

  ++impl_->rtcp_stats.sender_reports_sent;
  impl_->last_sr_at_ms = MonotonicMillis();
  impl_->last_sr_wall_ms = impl_->last_sr_at_ms;
  impl_->last_sr_compact_ntp = RtcpCompactNtp(ntp_seconds, ntp_fraction);
  return true;
}

bool RtpSession::SendBye(std::string_view reason, std::string* error) {
  if (!config_.enable_rtcp) {
    if (error)
      *error = "RTCP is disabled for this session";
    return false;
  }
  if (!is_open()) {
    if (error)
      *error = "RTP session is not connected";
    return false;
  }
  const std::vector<std::uint8_t> bytes = BuildRtcpBye({config_.ssrc}, reason);
  if (bytes.empty()) {
    if (error)
      *error = "cannot build the RTCP BYE";
    return false;
  }
  if (!impl_->SendRtcpPayload(bytes, error))
    return false;
  ++impl_->rtcp_stats.byes_sent;
  return true;
}

bool RtpSession::PollRtcp(int timeout_ms, std::string* error) {
  if (!config_.enable_rtcp) {
    if (error)
      *error = "RTCP is disabled for this session";
    return false;
  }
  if (!is_open()) {
    if (error)
      *error = "RTP session is not connected";
    return false;
  }

  SocketHandle target = kInvalidSocket;
  if (impl_->rtcp_via_tcp)
    target = config_.mode == RtpTransportMode::kTcpPassive ? impl_->accepted : impl_->fd;
  else
    target = impl_->rtcp_fd;
  if (target == kInvalidSocket) {
    if (error)
      *error = "RTCP socket is not open";
    return false;
  }

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(target, &read_set);
  timeval timeout{};
  timeval* timeout_ptr = nullptr;
  if (timeout_ms >= 0) {
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    timeout_ptr = &timeout;
  }
  const int ready =
      select(static_cast<int>(target) + 1, &read_set, nullptr, nullptr, timeout_ptr);
  if (ready == 0)
    return false;  // no RTCP pending; not an error
  if (ready < 0) {
    if (error)
      *error = "RTCP select failed: " + SocketErrorText(LastSocketError());
    return false;
  }

  const std::int64_t now_ms = MonotonicMillis();
  if (!impl_->rtcp_via_tcp) {
    std::uint8_t buffer[4096];
    const int received = recv(target, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (received <= 0) {
      if (error)
        *error = "RTCP receive failed: " + SocketErrorText(LastSocketError());
      return false;
    }
    std::vector<RtcpPacket> packets;
    if (!ParseRtcpCompound(buffer, static_cast<std::size_t>(received), &packets, error))
      return false;
    impl_->rtcp_stats.bytes_received += static_cast<std::uint64_t>(received);
    for (const RtcpPacket& packet : packets)
      impl_->HandleRtcpPacket(packet, now_ms);
    return true;
  }

  std::uint8_t chunk[4096];
  const int received = recv(target, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
  if (received <= 0) {
    if (error)
      *error = "TCP media read for RTCP failed: " + SocketErrorText(LastSocketError());
    return false;
  }
  impl_->tcp_rtcp_buffer.insert(impl_->tcp_rtcp_buffer.end(), chunk,
                                chunk + static_cast<std::size_t>(received));
  bool processed = false;
  while (impl_->tcp_rtcp_buffer.size() >= 2) {
    const std::size_t length =
        (static_cast<std::size_t>(impl_->tcp_rtcp_buffer[0]) << 8) |
        impl_->tcp_rtcp_buffer[1];
    if (impl_->tcp_rtcp_buffer.size() < 2 + length)
      break;
    // Remove the frame before dispatching: a BYE callback closes the session
    // and clears this buffer, and the loop must not touch it afterwards.
    std::vector<std::uint8_t> frame(impl_->tcp_rtcp_buffer.begin() + 2,
                                    impl_->tcp_rtcp_buffer.begin() + 2 + length);
    impl_->tcp_rtcp_buffer.erase(impl_->tcp_rtcp_buffer.begin(),
                                 impl_->tcp_rtcp_buffer.begin() + 2 + length);
    // A platform may send RTP on this connection too. RTCP is identified by
    // payload types 200..207; anything else is not addressed to this parser.
    if (length >= 2 && (frame[0] >> 6) == 2 && frame[1] >= 200 && frame[1] <= 207) {
      std::vector<RtcpPacket> packets;
      if (!ParseRtcpCompound(frame.data(), frame.size(), &packets, error))
        return false;
      impl_->rtcp_stats.bytes_received += length;
      for (const RtcpPacket& packet : packets)
        impl_->HandleRtcpPacket(packet, now_ms);
      processed = true;
    }
  }
  return processed;
}

bool RtpSession::Tick(std::int64_t now_ms, std::string* error) {
  if (!config_.enable_rtcp || !is_open())
    return true;
  if (config_.sender_report_interval_ms == 0)
    return true;
  if (impl_->last_sr_at_ms >= 0 &&
      now_ms - impl_->last_sr_at_ms <
          static_cast<std::int64_t>(config_.sender_report_interval_ms))
    return true;
  if (!SendSenderReport(error))
    return false;
  impl_->last_sr_at_ms = now_ms;
  return true;
}

const RtcpStats& RtpSession::rtcp_stats() const { return impl_->rtcp_stats; }

std::uint16_t RtpSession::rtcp_local_port() const {
  if (!config_.enable_rtcp)
    return 0;
  return impl_->rtcp_bound_port != 0 ? impl_->rtcp_bound_port : config_.rtcp_local_port;
}

std::string RtpSession::rtcp_peer_address() const {
  if (!impl_->rtcp_peer.empty())
    return impl_->rtcp_peer;
  return impl_->peer;
}

bool RtpSession::is_open() const {
  if (config_.mode == RtpTransportMode::kTcpPassive)
    return impl_->accepted != kInvalidSocket;
  return impl_->fd != kInvalidSocket;
}

std::uint16_t RtpSession::local_port() const {
  return impl_->bound_port != 0 ? impl_->bound_port : config_.local_port;
}

std::string RtpSession::local_address() const { return config_.local_address; }

std::string RtpSession::peer_address() const { return impl_->peer; }

std::string RtpSession::BuildSessionSdp(const std::string& advertise_address,
                                        const std::string& origin_id,
                                        const std::string& session_name) const {
  SdpVideoSession session;
  session.address = advertise_address;
  session.port = local_port();
  session.origin_id = origin_id;
  session.session_name = session_name;
  session.mode = config_.mode;
  session.setup = config_.mode == RtpTransportMode::kUdp
                      ? RtpSetup::kNone
                      : (config_.mode == RtpTransportMode::kTcpPassive ? RtpSetup::kPassive
                                                                      : RtpSetup::kActive);
  session.connection_new = config_.mode != RtpTransportMode::kUdp;
  session.payload_type = config_.payload_type;
  session.has_ssrc = true;
  session.ssrc = config_.ssrc;
  session.direction = "sendonly";
  return BuildSdp(session);
}

}  // namespace uvnvr::gb28181
