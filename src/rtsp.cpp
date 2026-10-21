#include "rtsp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace uvnvr {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket s) {
  if (s != kInvalidSocket)
    closesocket(s);
}
bool InitSockets(std::string* error) {
  static bool ready = false;
  if (ready)
    return true;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    if (error)
      *error = "WSAStartup failed";
    return false;
  }
  ready = true;
  return true;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket s) {
  if (s >= 0)
    close(s);
}
bool InitSockets(std::string*) {
  return true;
}
#endif

std::string Base64Decode(const std::string& input) {
  std::vector<unsigned char> output(input.size() * 3 / 4 + 4);
  const int written =
      EVP_DecodeBlock(output.data(), reinterpret_cast<const unsigned char*>(input.data()),
                      static_cast<int>(input.size()));
  if (written < 0)
    return {};
  size_t size = static_cast<size_t>(written);
  if (!input.empty() && input.back() == '=')
    --size;
  if (input.size() > 1 && input[input.size() - 2] == '=')
    --size;
  return std::string(reinterpret_cast<char*>(output.data()), std::min(size, output.size()));
}

std::string RandomToken() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    return "session";
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

[[maybe_unused]] std::string PercentDecode(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(input[i + 1]);
      const int lo = hex(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

struct Request {
  std::string method;
  std::string url;
  std::string version;
  std::map<std::string, std::string> headers;
  std::string header(const std::string& name) const {
    const auto it = headers.find(Lower(name));
    return it == headers.end() ? std::string() : it->second;
  }
};

bool ReadRequest(Socket fd, Request* request) {
  std::string data;
  char buffer[4096];
  while (data.find("\r\n\r\n") == std::string::npos) {
    const int n = static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
    if (n <= 0)
      return false;
    data.append(buffer, static_cast<size_t>(n));
    if (data.size() > 65536)
      return false;
  }
  const size_t end = data.find("\r\n\r\n");
  std::istringstream stream(data.substr(0, end));
  std::string line;
  if (!std::getline(stream, line))
    return false;
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  std::istringstream first(line);
  first >> request->method >> request->url >> request->version;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string name = Lower(line.substr(0, colon));
    size_t value_start = colon + 1;
    while (value_start < line.size() && line[value_start] == ' ')
      ++value_start;
    request->headers[name] = line.substr(value_start);
  }
  return !request->method.empty();
}

bool SendAll(Socket fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n =
        static_cast<int>(::send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0));
    if (n <= 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Base64Encode(const std::string& input) {
  if (input.empty())
    return {};
  std::string out(((input.size() + 2) / 3) * 4, '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                      reinterpret_cast<const unsigned char*>(input.data()),
                                      static_cast<int>(input.size()));
  out.resize(written > 0 ? static_cast<size_t>(written) : 0);
  return out;
}

std::string HexBytes(const uint8_t* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0f]);
  }
  return out;
}

struct NalUnit {
  std::vector<uint8_t> data;
  uint32_t pts = 0;
  bool last = false;
};

// Extracts H.264/H.265 NAL units from an MPEG-TS stream and keeps its SPS/PPS
// for the SDP description. The parser is used by the short-lived DESCRIBE probe
// and by each PLAY session.
class TsParser {
public:
  bool CodecReady() const {
    return !codec_.empty();
  }
  bool HasParameters() const {
    return !sps_.empty() && !pps_.empty();
  }
  const std::string& Codec() const {
    return codec_;
  }

  std::string Sdp() const {
    std::ostringstream sdp;
    sdp << "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=UvNVR\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n";
    if (codec_ == "h265") {
      sdp << "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\na=fmtp:96 ";
      bool first = true;
      const auto add = [&](const char* name, const std::string& value) {
        if (value.empty())
          return;
        if (!first)
          sdp << ";";
        sdp << name << "=" << Base64Encode(value);
        first = false;
      };
      add("sprop-vps", vps_);
      add("sprop-sps", sps_);
      add("sprop-pps", pps_);
      sdp << "\r\na=control:trackID=0\r\n";
    } else {
      sdp << "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 "
             "packetization-mode=1";
      if (!profile_level_id_.empty())
        sdp << ";profile-level-id=" << profile_level_id_;
      if (!sps_.empty() && !pps_.empty())
        sdp << ";sprop-parameter-sets=" << Base64Encode(sps_) << "," << Base64Encode(pps_);
      sdp << "\r\na=control:trackID=0\r\n";
    }
    return sdp.str();
  }

  void Feed(const uint8_t* data, size_t len, std::vector<NalUnit>* out) {
    pending_.insert(pending_.end(), data, data + len);
    size_t offset = 0;
    while (pending_.size() - offset >= 188) {
      if (pending_[offset] != 0x47) {
        ++offset;
        continue;
      }
      ParsePacket(pending_.data() + offset, out);
      offset += 188;
    }
    if (offset > 0)
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
    if (pending_.size() > 64 * 1024)
      pending_.clear();
  }

private:
  static uint32_t NowTimestamp() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count() *
                                 90);
  }

  void ParsePacket(const uint8_t* packet, std::vector<NalUnit>* out) {
    const int pid = ((packet[1] & 0x1f) << 8) | packet[2];
    const bool pusi = (packet[1] & 0x40) != 0;
    const int adaptation = (packet[3] >> 4) & 0x03;
    size_t payload = 4;
    if (adaptation & 0x02) {
      const size_t adaptation_length = packet[4];
      payload = 5 + adaptation_length;
    }
    if (!(adaptation & 0x01) || payload >= 188)
      return;
    const uint8_t* data = packet + payload;
    const size_t length = 188 - payload;
    if (pid == 0 && pusi) {
      ParsePat(data, length);
    } else if (pid == pmt_pid_ && pusi) {
      ParsePmt(data, length);
    } else if (pid == video_pid_) {
      ParsePes(data, length, pusi, out);
    }
  }

  void ParsePat(const uint8_t* data, size_t length) {
    if (length == 0)
      return;
    const size_t pointer = data[0];
    if (1 + pointer + 12 > length)
      return;
    const uint8_t* section = data + 1 + pointer;
    if (section[0] != 0x00)
      return;
    const size_t section_length = ((section[1] & 0x0f) << 8) | section[2];
    if (section_length < 9)
      return;
    const size_t end = std::min(3 + section_length, length - 1 - pointer);
    for (size_t i = 8; i + 4 <= end; i += 4) {
      const int program = (section[i] << 8) | section[i + 1];
      if (program == 0)
        continue;
      pmt_pid_ = ((section[i + 2] & 0x1f) << 8) | section[i + 3];
      return;
    }
  }

  void ParsePmt(const uint8_t* data, size_t length) {
    if (length == 0)
      return;
    const size_t pointer = data[0];
    if (1 + pointer + 12 > length)
      return;
    const uint8_t* section = data + 1 + pointer;
    if (section[0] != 0x02)
      return;
    const size_t section_length = ((section[1] & 0x0f) << 8) | section[2];
    const size_t end = std::min(3 + section_length, length - 1 - pointer);
    if (end < 12)
      return;
    const size_t program_info_length = ((section[10] & 0x0f) << 8) | section[11];
    size_t i = 12 + program_info_length;
    while (i + 5 <= end) {
      const int stream_type = section[i];
      const int pid = ((section[i + 1] & 0x1f) << 8) | section[i + 2];
      const size_t es_info_length = ((section[i + 3] & 0x0f) << 8) | section[i + 4];
      if (video_pid_ < 0 && (stream_type == 0x1b || stream_type == 0x24)) {
        video_pid_ = pid;
        codec_ = stream_type == 0x1b ? "h264" : "h265";
      }
      i += 5 + es_info_length;
    }
  }

  void ParsePes(const uint8_t* data, size_t length, bool pusi, std::vector<NalUnit>* out) {
    if (pusi) {
      FlushPes(out);
      pes_.clear();
      pes_header_done_ = false;
      pes_payload_offset_ = 9;
    }
    pes_.insert(pes_.end(), data, data + length);
    if (pes_header_done_ || pes_.size() < 9)
      return;
    if (!(pes_[0] == 0 && pes_[1] == 0 && pes_[2] == 1)) {
      pes_header_done_ = true;
      return;
    }
    const uint8_t flags = pes_[7];
    const size_t header_length = pes_[8];
    if (header_length >= 5) {
      pes_payload_offset_ = 9 + header_length;
      if (flags & 0x80) {
        const uint8_t* p = pes_.data() + 9;
        pes_pts_ = (static_cast<uint32_t>((p[0] >> 1) & 0x07) << 30) |
                   (static_cast<uint32_t>(p[1]) << 22) |
                   (static_cast<uint32_t>((p[2] >> 1) & 0x7f) << 15) |
                   (static_cast<uint32_t>(p[3]) << 7) | (static_cast<uint32_t>(p[4]) >> 1);
        pes_has_pts_ = true;
      }
    }
    pes_header_done_ = true;
  }

  void FlushPes(std::vector<NalUnit>* out) {
    if (pes_.size() <= pes_payload_offset_)
      return;
    const size_t first = out != nullptr ? out->size() : 0;
    size_t i = pes_payload_offset_;
    while (i + 3 <= pes_.size()) {
      if (!(pes_[i] == 0 && pes_[i + 1] == 0 && pes_[i + 2] == 1)) {
        ++i;
        continue;
      }
      const size_t nal_start = i + 3;
      size_t next = nal_start;
      while (next + 3 <= pes_.size()) {
        if (pes_[next] == 0 && pes_[next + 1] == 0 && pes_[next + 2] == 1)
          break;
        ++next;
      }
      size_t nal_end = next;
      while (nal_end > nal_start && pes_[nal_end - 1] == 0)
        --nal_end;
      if (nal_end > nal_start)
        HandleNal(pes_.data() + nal_start, nal_end - nal_start, out);
      i = next;
    }
    if (out != nullptr && out->size() > first)
      out->back().last = true;
  }

  void HandleNal(const uint8_t* nal, size_t length, std::vector<NalUnit>* out) {
    if (length == 0 || codec_.empty())
      return;
    if (codec_ == "h264") {
      const int type = nal[0] & 0x1f;
      if (type == 7) {
        sps_.assign(reinterpret_cast<const char*>(nal), length);
        if (length >= 4)
          profile_level_id_ = HexBytes(nal + 1, 3);
      } else if (type == 8) {
        pps_.assign(reinterpret_cast<const char*>(nal), length);
      } else if (type == 9 || type == 12) {
        return;
      }
    } else {
      const int type = (nal[0] >> 1) & 0x3f;
      if (type == 32) {
        vps_.assign(reinterpret_cast<const char*>(nal), length);
      } else if (type == 33) {
        sps_.assign(reinterpret_cast<const char*>(nal), length);
      } else if (type == 34) {
        pps_.assign(reinterpret_cast<const char*>(nal), length);
      } else if (type == 35 || type == 38) {
        return;
      }
    }
    if (out == nullptr)
      return;
    NalUnit unit;
    unit.data.assign(nal, nal + length);
    unit.pts = pes_has_pts_ ? pes_pts_ : NowTimestamp();
    out->push_back(std::move(unit));
  }

  std::vector<uint8_t> pending_;
  std::vector<uint8_t> pes_;
  int pmt_pid_ = -1;
  int video_pid_ = -1;
  std::string codec_;
  std::string sps_;
  std::string pps_;
  std::string vps_;
  std::string profile_level_id_;
  size_t pes_payload_offset_ = 9;
  bool pes_header_done_ = false;
  bool pes_has_pts_ = false;
  uint32_t pes_pts_ = 0;
};

std::string Response(int status, const std::string& reason, const std::string& cseq,
                     const std::string& extra_headers, const std::string& body = {}) {
  std::ostringstream out;
  out << "RTSP/1.0 " << status << " " << reason << "\r\n";
  if (!cseq.empty())
    out << "CSeq: " << cseq << "\r\n";
  out << "Server: UvNVR\r\n";
  if (!extra_headers.empty())
    out << extra_headers;
  if (!body.empty()) {
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Content-Type: application/sdp\r\n";
  }
  out << "\r\n" << body;
  return out.str();
}

} // namespace

struct RtspServer::Impl {
  struct Session {
    Socket fd = kInvalidSocket;
    Socket udp = kInvalidSocket;
    sockaddr_in client{};
    bool tcp = true;
    int rtp_channel = 0;
    int rtcp_channel = 1;
    int64_t channel_id = 0;
    std::string stream;
    std::string session_id;
    uint64_t subscriber_id = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> playing{false};
    std::thread sender;
    std::mutex mutex;
    std::mutex write_mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> queue;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    TsParser parser;
    uint64_t packet_count = 0;
    uint64_t octet_count = 0;
    int64_t last_rtcp_ms = 0;
    std::atomic<bool> detached{false};

    ~Session() {
      alive = false;
      playing = false;
      cv.notify_all();
      if (sender.joinable())
        sender.join();
      if (udp != kInvalidSocket)
        CloseSocket(udp);
      if (fd != kInvalidSocket)
        CloseSocket(fd);
    }

    void WriteTcp(const std::string& data) {
      std::lock_guard<std::mutex> lock(write_mutex);
      SendAll(fd, data);
    }

    void SendRtp(const std::vector<uint8_t>& payload, bool marker, uint32_t rtp_timestamp) {
      std::vector<uint8_t> packet(12 + payload.size());
      packet[0] = 0x80;
      packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | 96);
      const uint16_t current_sequence = ++sequence;
      packet[2] = static_cast<uint8_t>(current_sequence >> 8);
      packet[3] = static_cast<uint8_t>(current_sequence & 0xff);
      timestamp = rtp_timestamp;
      packet[4] = static_cast<uint8_t>(timestamp >> 24);
      packet[5] = static_cast<uint8_t>(timestamp >> 16);
      packet[6] = static_cast<uint8_t>(timestamp >> 8);
      packet[7] = static_cast<uint8_t>(timestamp & 0xff);
      packet[8] = static_cast<uint8_t>(ssrc >> 24);
      packet[9] = static_cast<uint8_t>(ssrc >> 16);
      packet[10] = static_cast<uint8_t>(ssrc >> 8);
      packet[11] = static_cast<uint8_t>(ssrc & 0xff);
      std::copy(payload.begin(), payload.end(), packet.begin() + 12);
      if (tcp) {
        std::string frame;
        frame.reserve(packet.size() + 4);
        frame.push_back('$');
        frame.push_back(static_cast<char>(rtp_channel));
        const uint16_t length = static_cast<uint16_t>(packet.size());
        frame.push_back(static_cast<char>(length >> 8));
        frame.push_back(static_cast<char>(length & 0xff));
        frame.append(reinterpret_cast<const char*>(packet.data()), packet.size());
        WriteTcp(frame);
      } else {
        ::sendto(udp, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                 0, reinterpret_cast<const sockaddr*>(&client), sizeof(client));
      }
      ++packet_count;
      octet_count += payload.size();
    }

    void SendNal(const NalUnit& nal) {
      if (nal.data.empty())
        return;
      const uint32_t rtp_time = nal.pts != 0 ? nal.pts : static_cast<uint32_t>(NowMillis() * 90);
      constexpr size_t kMaxPayload = 1200;
      if (parser.Codec() == "h265") {
        const uint8_t type = (nal.data[0] >> 1) & 0x3f;
        if (nal.data.size() <= kMaxPayload) {
          SendRtp(nal.data, nal.last, rtp_time);
          return;
        }
        std::vector<uint8_t> payload;
        size_t offset = 2;
        bool first = true;
        while (offset < nal.data.size()) {
          const size_t count = std::min(kMaxPayload - 3, nal.data.size() - offset);
          const bool last_fragment = offset + count >= nal.data.size();
          payload.clear();
          payload.push_back(static_cast<uint8_t>((nal.data[0] & 0x81) | (49 << 1)));
          payload.push_back(nal.data[1]);
          payload.push_back(
              static_cast<uint8_t>((first ? 0x80 : 0x00) | (last_fragment ? 0x40 : 0x00) | type));
          payload.insert(payload.end(), nal.data.begin() + static_cast<long>(offset),
                         nal.data.begin() + static_cast<long>(offset + count));
          SendRtp(payload, nal.last && last_fragment, rtp_time);
          offset += count;
          first = false;
        }
        return;
      }
      const uint8_t type = nal.data[0] & 0x1f;
      if (nal.data.size() <= kMaxPayload) {
        SendRtp(nal.data, nal.last, rtp_time);
        return;
      }
      std::vector<uint8_t> payload;
      size_t offset = 1;
      bool first = true;
      while (offset < nal.data.size()) {
        const size_t count = std::min(kMaxPayload - 2, nal.data.size() - offset);
        const bool last_fragment = offset + count >= nal.data.size();
        payload.clear();
        payload.push_back(static_cast<uint8_t>((nal.data[0] & 0xe0) | 28));
        payload.push_back(
            static_cast<uint8_t>((first ? 0x80 : 0x00) | (last_fragment ? 0x40 : 0x00) | type));
        payload.insert(payload.end(), nal.data.begin() + static_cast<long>(offset),
                       nal.data.begin() + static_cast<long>(offset + count));
        SendRtp(payload, nal.last && last_fragment, rtp_time);
        offset += count;
        first = false;
      }
    }

    void SendRtcpSr() {
      const int64_t now_ms = NowMillis();
      if (now_ms - last_rtcp_ms < 5000)
        return;
      last_rtcp_ms = now_ms;
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const uint64_t seconds =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count()) +
          2208988800ULL;
      const uint64_t nanos = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000LL);
      const uint32_t fraction = static_cast<uint32_t>((nanos << 32) / 1000000000ULL);
      std::vector<uint8_t> packet(28, 0);
      packet[0] = 0x80;
      packet[1] = 200;
      packet[3] = 6;
      const auto put32 = [&](size_t offset, uint32_t value) {
        packet[offset] = static_cast<uint8_t>(value >> 24);
        packet[offset + 1] = static_cast<uint8_t>(value >> 16);
        packet[offset + 2] = static_cast<uint8_t>(value >> 8);
        packet[offset + 3] = static_cast<uint8_t>(value);
      };
      put32(4, ssrc);
      put32(8, static_cast<uint32_t>(seconds));
      put32(12, fraction);
      put32(16, timestamp);
      put32(20, static_cast<uint32_t>(packet_count));
      put32(24, static_cast<uint32_t>(octet_count));
      if (tcp) {
        std::string frame;
        frame.reserve(packet.size() + 4);
        frame.push_back('$');
        frame.push_back(static_cast<char>(rtcp_channel));
        const uint16_t length = static_cast<uint16_t>(packet.size());
        frame.push_back(static_cast<char>(length >> 8));
        frame.push_back(static_cast<char>(length & 0xff));
        frame.append(reinterpret_cast<const char*>(packet.data()), packet.size());
        WriteTcp(frame);
      } else {
        sockaddr_in rtcp_client = client;
        rtcp_client.sin_port = htons(static_cast<uint16_t>(ntohs(client.sin_port) + 1));
        ::sendto(udp, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                 0, reinterpret_cast<const sockaddr*>(&rtcp_client), sizeof(rtcp_client));
      }
    }

    void Sender() {
      while (alive.load()) {
        std::shared_ptr<std::vector<uint8_t>> chunk;
        {
          std::unique_lock<std::mutex> lock(mutex);
          cv.wait_for(lock, std::chrono::milliseconds(100),
                      [this] { return !alive.load() || (!queue.empty() && playing.load()); });
          if (!alive.load())
            break;
          if (!playing.load())
            continue;
          if (queue.empty())
            continue;
          chunk = std::move(queue.front());
          queue.pop_front();
          if (queue.size() > 512)
            queue.pop_front();
        }
        std::vector<NalUnit> nals;
        parser.Feed(chunk->data(), chunk->size(), &nals);
        for (const auto& nal : nals)
          SendNal(nal);
        SendRtcpSr();
      }
    }

    void Enqueue(const uint8_t* data, size_t len) {
      if (!alive.load() || len == 0)
        return;
      auto copy = std::make_shared<std::vector<uint8_t>>(data, data + len);
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() > 1024)
          queue.pop_front();
        queue.push_back(std::move(copy));
      }
      cv.notify_one();
    }
  };

  Impl(AuthFn auth_in, AttachFn attach_in, DetachFn detach_in, int port_in)
      : auth(std::move(auth_in)), attach(std::move(attach_in)), detach(std::move(detach_in)),
        port(port_in) {}
  ~Impl() {
    Stop();
  }

  AuthFn auth;
  AttachFn attach;
  DetachFn detach;
  int port;
  Socket listen_fd = kInvalidSocket;
  std::atomic<bool> stopping{false};
  std::thread acceptor;

  bool Bind(int requested_port, std::string* error) {
    if (!InitSockets(error))
      return false;
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == kInvalidSocket) {
      if (error)
        *error = "cannot create the RTSP socket";
      return false;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      CloseSocket(listen_fd);
      listen_fd = kInvalidSocket;
      if (error)
        *error = "cannot bind RTSP port " + std::to_string(requested_port);
      return false;
    }
    if (::listen(listen_fd, 16) != 0) {
      CloseSocket(listen_fd);
      listen_fd = kInvalidSocket;
      if (error)
        *error = "cannot listen on the RTSP port";
      return false;
    }
    port = requested_port;
    return true;
  }

  bool Start(std::string* error) {
    if (!Bind(port, error)) {
      if (port != 554)
        return false;
      bool bound = false;
      std::string last_error;
      for (const int fallback : {8554, 8555, 8556}) {
        if (Bind(fallback, &last_error)) {
          bound = true;
          break;
        }
      }
      if (!bound) {
        if (error)
          *error = last_error;
        return false;
      }
    }
    stopping = false;
    acceptor = std::thread([this] { AcceptLoop(); });
    return true;
  }

  void Stop() {
    stopping = true;
    if (listen_fd != kInvalidSocket) {
      CloseSocket(listen_fd);
      listen_fd = kInvalidSocket;
    }
    if (acceptor.joinable())
      acceptor.join();
  }

  void AcceptLoop() {
    while (!stopping.load()) {
      sockaddr_in peer{};
#ifdef _WIN32
      int length = sizeof(peer);
#else
      socklen_t length = sizeof(peer);
#endif
      Socket fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &length);
      if (fd == kInvalidSocket) {
        if (stopping.load())
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      auto session = std::make_shared<Session>();
      session->fd = fd;
      session->client = peer;
      session->session_id = RandomToken();
      session->ssrc = static_cast<uint32_t>(std::stoul(RandomToken().substr(0, 8), nullptr, 16));
      std::thread([this, session] { ClientLoop(session); }).detach();
    }
  }

  bool ProbeCodec(int64_t channel, const std::string& stream, TsParser* parser,
                  std::string* error) {
    struct State {
      std::mutex mutex;
      std::condition_variable cv;
      TsParser parser;
    };
    auto state = std::make_shared<State>();
    auto sink = [state](const uint8_t* data, size_t len) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->parser.CodecReady())
        return;
      state->parser.Feed(data, len, nullptr);
      if (state->parser.CodecReady())
        state->cv.notify_all();
    };
    std::string attach_error;
    const uint64_t id = attach(channel, stream, sink, &attach_error);
    if (id == 0) {
      if (error)
        *error = attach_error.empty() ? "the stream is not running" : attach_error;
      return false;
    }
    bool ready = false;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      ready = state->cv.wait_for(lock, std::chrono::seconds(2),
                                 [&state] { return state->parser.CodecReady(); });
    }
    detach(channel, stream, id);
    if (!ready) {
      if (error)
        *error = "codec information is not available";
      return false;
    }
    *parser = std::move(state->parser);
    return true;
  }

  bool ParsePath(const std::string& url, int64_t* channel, std::string* stream) {
    std::string path = url;
    const size_t scheme = path.find("rtsp://");
    if (scheme == 0) {
      const size_t slash = path.find('/', 7);
      path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    const size_t query = path.find('?');
    if (query != std::string::npos)
      path = path.substr(0, query);
    if (path.rfind("/live/", 0) != 0)
      return false;
    std::string rest = path.substr(6);
    const size_t slash = rest.find('/');
    const std::string channel_text = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash == std::string::npos) {
      *stream = "main";
    } else {
      const std::string tail = rest.substr(slash + 1);
      const size_t next = tail.find('/');
      *stream = next == std::string::npos ? tail : tail.substr(0, next);
    }
    if (*stream != "main" && *stream != "sub")
      return false;
    try {
      *channel = std::stoll(channel_text);
    } catch (...) {
      return false;
    }
    return *channel > 0;
  }

  bool Credentials(const Request& request, const std::string& url, std::string* username,
                   std::string* password) {
    const std::string authorization = request.header("Authorization");
    if (authorization.rfind("Basic ", 0) == 0) {
      const std::string decoded = Base64Decode(authorization.substr(6));
      const size_t colon = decoded.find(':');
      if (colon != std::string::npos) {
        *username = decoded.substr(0, colon);
        *password = decoded.substr(colon + 1);
        return true;
      }
    }
    const size_t scheme = url.find("rtsp://");
    if (scheme != std::string::npos) {
      const size_t at = url.find('@', scheme + 7);
      if (at != std::string::npos) {
        const std::string userinfo = url.substr(scheme + 7, at - scheme - 7);
        const size_t colon = userinfo.find(':');
        if (colon != std::string::npos) {
          *username = userinfo.substr(0, colon);
          *password = userinfo.substr(colon + 1);
          return true;
        }
      }
    }
    return false;
  }

  void ClientLoop(std::shared_ptr<Session> session) {
    std::string username;
    std::string password;
    bool authenticated = false;
    while (session->alive.load()) {
      Request request;
      if (!ReadRequest(session->fd, &request))
        break;
      const std::string cseq = request.header("CSeq");
      int64_t channel = 0;
      std::string stream;
      if (!ParsePath(request.url, &channel, &stream)) {
        session->WriteTcp(Response(404, "Not Found", cseq, ""));
        break;
      }
      if (!authenticated) {
        if (!Credentials(request, request.url, &username, &password) || !auth(username, password)) {
          session->WriteTcp(
              Response(401, "Unauthorized", cseq, "WWW-Authenticate: Basic realm=\"UvNVR\"\r\n"));
          continue;
        }
        authenticated = true;
      }

      if (request.method == "OPTIONS") {
        session->WriteTcp(
            Response(200, "OK", cseq,
                     "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, "
                     "SET_PARAMETER\r\n"));
      } else if (request.method == "DESCRIBE") {
        TsParser probe;
        std::string probe_error;
        if (!ProbeCodec(channel, stream, &probe, &probe_error)) {
          session->WriteTcp(Response(503, "Service Unavailable", cseq, "", ""));
          continue;
        }
        const std::string sdp = probe.Sdp();
        session->WriteTcp(
            Response(200, "OK", cseq,
                     "Content-Base: " + request.url + "\r\nCache-Control: no-cache\r\n", sdp));
      } else if (request.method == "SETUP") {
        const std::string transport = request.header("Transport");
        session->channel_id = channel;
        session->stream = stream;
        if (transport.find("RTP/AVP/TCP") != std::string::npos) {
          session->tcp = true;
          const size_t interleaved = transport.find("interleaved=");
          if (interleaved != std::string::npos) {
            const int first = std::atoi(transport.c_str() + interleaved + 12);
            session->rtp_channel = first;
            session->rtcp_channel = first + 1;
          }
          session->WriteTcp(Response(
              200, "OK", cseq,
              "Transport: RTP/AVP/TCP;unicast;interleaved=" + std::to_string(session->rtp_channel) +
                  "-" + std::to_string(session->rtcp_channel) +
                  ";ssrc=" + std::to_string(session->ssrc) + "\r\nSession: " + session->session_id +
                  ";timeout=60\r\n"));
        } else {
          const size_t client_port = transport.find("client_port=");
          int rtp_port = 0;
          if (client_port != std::string::npos) {
            rtp_port = std::atoi(transport.c_str() + client_port + 12);
          }
          if (rtp_port <= 0) {
            session->WriteTcp(Response(461, "Unsupported Transport", cseq, ""));
            continue;
          }
          session->tcp = false;
          session->client.sin_port = htons(static_cast<uint16_t>(rtp_port));
          session->udp = ::socket(AF_INET, SOCK_DGRAM, 0);
          sockaddr_in local{};
          local.sin_family = AF_INET;
          local.sin_addr.s_addr = htonl(INADDR_ANY);
          local.sin_port = 0;
          ::bind(session->udp, reinterpret_cast<sockaddr*>(&local), sizeof(local));
#ifdef _WIN32
          int local_len = sizeof(local);
#else
          socklen_t local_len = sizeof(local);
#endif
          getsockname(session->udp, reinterpret_cast<sockaddr*>(&local), &local_len);
          const int server_port = ntohs(local.sin_port);
          session->WriteTcp(Response(
              200, "OK", cseq,
              "Transport: RTP/AVP;unicast;client_port=" + std::to_string(rtp_port) + "-" +
                  std::to_string(rtp_port + 1) + ";server_port=" + std::to_string(server_port) +
                  "-" + std::to_string(server_port + 1) + ";ssrc=" + std::to_string(session->ssrc) +
                  "\r\nSession: " + session->session_id + ";timeout=60\r\n"));
        }
      } else if (request.method == "PLAY") {
        if (!session->playing.load()) {
          if (!session->detached.exchange(true)) {
            std::string attach_error;
            auto sink = [session](const uint8_t* data, size_t len) { session->Enqueue(data, len); };
            session->subscriber_id =
                attach(session->channel_id, session->stream, sink, &attach_error);
            if (session->subscriber_id == 0) {
              session->WriteTcp(Response(404, "Not Found", cseq, ""));
              session->alive = false;
              break;
            }
          }
          session->playing = true;
          if (!session->sender.joinable()) {
            session->sender = std::thread([session] { session->Sender(); });
          }
        }
        session->WriteTcp(Response(200, "OK", cseq,
                                   "Session: " + session->session_id + "\r\nRTP-Info: url=" +
                                       request.url + ";seq=1;rtptime=0\r\nRange: npt=0.000-\r\n"));
      } else if (request.method == "PAUSE") {
        session->playing = false;
        session->WriteTcp(Response(200, "OK", cseq, "Session: " + session->session_id + "\r\n"));
      } else if (request.method == "TEARDOWN") {
        session->playing = false;
        session->alive = false;
        session->WriteTcp(Response(200, "OK", cseq, "Session: " + session->session_id + "\r\n"));
        break;
      } else if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER") {
        session->WriteTcp(Response(200, "OK", cseq, "Session: " + session->session_id + "\r\n"));
      } else {
        session->WriteTcp(Response(501, "Not Implemented", cseq, ""));
      }
    }
    session->playing = false;
    session->alive = false;
    session->cv.notify_all();
    if (session->sender.joinable())
      session->sender.join();
    if (session->subscriber_id != 0) {
      detach(session->channel_id, session->stream, session->subscriber_id);
      session->subscriber_id = 0;
    }
  }
};

RtspServer::RtspServer(AuthFn auth, AttachFn attach, DetachFn detach, int port)
    : impl_(std::make_unique<Impl>(std::move(auth), std::move(attach), std::move(detach), port)) {}

RtspServer::~RtspServer() = default;

bool RtspServer::Start(std::string* error) {
  return impl_->Start(error);
}

void RtspServer::Stop() {
  impl_->Stop();
}

int RtspServer::Port() const {
  return impl_->port;
}

} // namespace uvnvr
