// GB 35114 A-level -> B-level interoperability harness.
//
// This tool plays either side of a GB 35114 session over real UDP sockets:
//
//   device    a UvNVR device: mutual SM2 authentication and VKEK negotiation
//             with VkekManager, A-level SM3/SM2 signatures on every key frame,
//             and B-level SM4-CTR sealing of the H.264 elementary stream.
//   platform  a reference 35114 platform simulator: it validates the device
//             identity, issues the signed/encrypted VKEK, receives the media
//             over the GB/T 28181 RTP framing, verifies the signatures,
//             decrypts the stream and rejects modified records.
//
// The negotiation records travel in SIP MESSAGE bodies, exactly as the
// production SIP stack carries them; the media travels as RTP payload type 96
// through uvnvr_gb28181's RtpSession/ParseRtpPacket. The platform records every
// datagram it sends or receives into a libpcap file, because the lab host does
// not grant CAP_NET_RAW to tcpdump. The pcap is an endpoint capture of the real
// UDP traffic, with the loopback IPv4/UDP headers reconstructed from the
// socket addresses that the kernel reported.
//
// This is acceptance evidence for the interop issue, not a production
// component: it is built only with UVNVR_ENABLE_GB28181 and
// UVNVR_ENABLE_GB35114 on, and it is not linked into the server.

#include "crypto/sm.h"
#include "gb28181/rtp.h"
#include "gb35114/es_crypto.h"
#include "gb35114/sign.h"
#include "gb35114/stream_keys.h"
#include "gb35114/vkek.h"

#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace gm = uvnvr::gm;
namespace gb35114 = uvnvr::gb35114;
namespace gb28181 = uvnvr::gb28181;

namespace {

constexpr std::uint16_t kDefaultNegotiatePort = 0; // OS-assigned
constexpr std::uint32_t kRtpSsrc = 0x35114027;
constexpr int kSocketTimeoutMs = 5000;
constexpr std::int64_t kClockSkewMs = 5 * 60 * 1000;

int failures = 0;

void Fail(const std::string& text) {
  std::fprintf(stderr, "error: %s\n", text.c_str());
  ++failures;
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string ReadFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& path, std::string_view data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(output);
}

gm::Bytes ReadBytes(const fs::path& path) {
  const std::string text = ReadFile(path);
  return gm::Bytes(text.begin(), text.end());
}

std::string_view BytesView(const gm::Bytes& bytes) {
  static const char kEmpty = '\0';
  return std::string_view(bytes.empty() ? &kEmpty : reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

std::string Sha256Hex(const gm::Bytes& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(data.data(), data.size(), digest, &length, EVP_sha256(), nullptr) != 1)
    return {};
  return gm::ToHex(gm::Bytes(digest, digest + length));
}

std::string RandomHex(std::size_t bytes) {
  gm::Bytes random;
  std::string error;
  if (!gm::RandomBytes(bytes, &random, &error))
    return "00000000";
  return gm::ToHex(random);
}

// ---- SIP MESSAGE carrier -------------------------------------------------

std::string BuildSipMessage(std::string_view type, std::string_view from_uri,
                            std::string_view to_uri, std::uint16_t local_port,
                            std::string_view body, std::string_view content_type) {
  std::string message;
  message += "MESSAGE sip:platform@127.0.0.1 SIP/2.0\r\n";
  message += "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(local_port) + ";branch=z9hG4bK" +
             RandomHex(8) + "\r\n";
  message += "From: <" + std::string(from_uri) + ">;tag=" + RandomHex(4) + "\r\n";
  message += "To: <" + std::string(to_uri) + ">\r\n";
  message += "Call-ID: " + RandomHex(8) + "@127.0.0.1\r\n";
  message += "CSeq: 1 MESSAGE\r\n";
  message += "Max-Forwards: 70\r\n";
  message += "Content-Type: " + std::string(content_type) + "\r\n";
  message += "X-GB35114-Message: " + std::string(type) + "\r\n";
  message += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  message += body;
  return message;
}

struct SipMessage {
  std::string type;
  std::string content_type;
  gm::Bytes body;
};

std::string HeaderValue(std::string_view headers, std::string_view name) {
  const std::string needle = std::string(name) + ": ";
  std::size_t position = headers.find(needle);
  if (position == std::string_view::npos)
    return {};
  position += needle.size();
  const std::size_t end = headers.find("\r\n", position);
  return std::string(headers.substr(position, end - position));
}

bool ParseSipMessage(const gm::Bytes& datagram, SipMessage* message) {
  const std::string text(datagram.begin(), datagram.end());
  const std::size_t split = text.find("\r\n\r\n");
  if (split == std::string::npos)
    return false;
  const std::string_view headers(text.data(), split);
  if (headers.rfind("MESSAGE ", 0) != 0)
    return false;
  const std::size_t content_length = std::strtoul(HeaderValue(headers, "Content-Length").c_str(),
                                                  nullptr, 10);
  message->type = HeaderValue(headers, "X-GB35114-Message");
  message->content_type = HeaderValue(headers, "Content-Type");
  const std::size_t body_offset = split + 4;
  if (text.size() < body_offset + content_length)
    return false;
  message->body.assign(datagram.begin() + static_cast<std::ptrdiff_t>(body_offset),
                       datagram.begin() + static_cast<std::ptrdiff_t>(body_offset + content_length));
  return !message->type.empty();
}

// ---- UDP socket and pcap recording --------------------------------------

struct PcapWriter {
  std::FILE* file = nullptr;

  bool Open(const fs::path& path) {
    file = std::fopen(path.string().c_str(), "wb");
    if (!file)
      return false;
    const std::uint32_t magic = 0xA1B2C3D4;
    const std::uint16_t major = 2;
    const std::uint16_t minor = 4;
    const std::int32_t zone = 0;
    const std::uint32_t sigfigs = 0;
    const std::uint32_t snap = 65535;
    const std::uint32_t network = 1; // Ethernet
    std::fwrite(&magic, sizeof(magic), 1, file);
    std::fwrite(&major, sizeof(major), 1, file);
    std::fwrite(&minor, sizeof(minor), 1, file);
    std::fwrite(&zone, sizeof(zone), 1, file);
    std::fwrite(&sigfigs, sizeof(sigfigs), 1, file);
    std::fwrite(&snap, sizeof(snap), 1, file);
    std::fwrite(&network, sizeof(network), 1, file);
    std::fflush(file);
    return true;
  }

  ~PcapWriter() {
    if (file)
      std::fclose(file);
  }

  static std::uint16_t Checksum(const std::uint8_t* data, std::size_t size) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < size; i += 2)
      sum += static_cast<std::uint16_t>((data[i] << 8) | data[i + 1]);
    if ((size & 1) != 0)
      sum += static_cast<std::uint16_t>(data[size - 1] << 8);
    while ((sum >> 16) != 0)
      sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
  }

  void Write(const std::string& src_address, std::uint16_t src_port,
             const std::string& dst_address, std::uint16_t dst_port, const std::uint8_t* payload,
             std::size_t size) {
    if (!file)
      return;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count();

    std::vector<std::uint8_t> frame;
    frame.resize(14 + 20 + 8 + size, 0);
    std::uint8_t* ethernet = frame.data();
    // Loopback frames captured by tcpdump use an all-zero Ethernet header.
    ethernet[12] = 0x08;
    ethernet[13] = 0x00;

    std::uint8_t* ip = ethernet + 14;
    const std::size_t ip_total = 20 + 8 + size;
    ip[0] = 0x45;
    ip[2] = static_cast<std::uint8_t>(ip_total >> 8);
    ip[3] = static_cast<std::uint8_t>(ip_total & 0xFF);
    ip[4] = static_cast<std::uint8_t>(reinterpret_cast<std::uintptr_t>(this) & 0xFF);
    ip[5] = 0x01;
    ip[6] = 0x40; // don't fragment
    ip[8] = 64;   // ttl
    ip[9] = 17;   // UDP
    struct in_addr src {};
    struct in_addr dst {};
    inet_pton(AF_INET, src_address.c_str(), &src);
    inet_pton(AF_INET, dst_address.c_str(), &dst);
    std::memcpy(ip + 12, &src, 4);
    std::memcpy(ip + 16, &dst, 4);
    const std::uint16_t checksum = Checksum(ip, 20);
    ip[10] = static_cast<std::uint8_t>(checksum >> 8);
    ip[11] = static_cast<std::uint8_t>(checksum & 0xFF);

    std::uint8_t* udp = ip + 20;
    udp[0] = static_cast<std::uint8_t>(src_port >> 8);
    udp[1] = static_cast<std::uint8_t>(src_port & 0xFF);
    udp[2] = static_cast<std::uint8_t>(dst_port >> 8);
    udp[3] = static_cast<std::uint8_t>(dst_port & 0xFF);
    const std::size_t udp_length = 8 + size;
    udp[4] = static_cast<std::uint8_t>(udp_length >> 8);
    udp[5] = static_cast<std::uint8_t>(udp_length & 0xFF);
    std::memcpy(udp + 8, payload, size);

    const std::uint32_t ts_sec = static_cast<std::uint32_t>(seconds.count());
    const std::uint32_t ts_usec = static_cast<std::uint32_t>(micros);
    const std::uint32_t incl = static_cast<std::uint32_t>(frame.size());
    std::fwrite(&ts_sec, sizeof(ts_sec), 1, file);
    std::fwrite(&ts_usec, sizeof(ts_usec), 1, file);
    std::fwrite(&incl, sizeof(incl), 1, file);
    std::fwrite(&incl, sizeof(incl), 1, file);
    std::fwrite(frame.data(), 1, frame.size(), file);
    std::fflush(file);
  }
};

class UdpSocket {
 public:
  ~UdpSocket() { Close(); }

  bool Open(std::uint16_t port, const std::string& bind_address, PcapWriter* pcap,
            std::string* error) {
    pcap_ = pcap;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      *error = "socket() failed";
      return false;
    }
    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    inet_pton(AF_INET, bind_address.c_str(), &local.sin_addr);
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
      *error = "bind() failed";
      Close();
      return false;
    }
    local_address_ = bind_address;
    local_port_ = LocalPort();
    return true;
  }

  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd_for_poll() const { return fd_; }

  std::uint16_t LocalPort() const {
    if (fd_ < 0)
      return 0;
    struct sockaddr_in local {};
    socklen_t length = sizeof(local);
    if (getsockname(fd_, reinterpret_cast<struct sockaddr*>(&local), &length) != 0)
      return 0;
    return ntohs(local.sin_port);
  }

  bool SendTo(const std::string& address, std::uint16_t port, const std::string& payload) {
    struct sockaddr_in remote {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    inet_pton(AF_INET, address.c_str(), &remote.sin_addr);
    const ssize_t sent =
        ::sendto(fd_, payload.data(), payload.size(), 0,
                 reinterpret_cast<struct sockaddr*>(&remote), sizeof(remote));
    if (pcap_)
      pcap_->Write(local_address_, local_port_, address, port,
                   reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    return sent == static_cast<ssize_t>(payload.size());
  }

  bool Recv(gm::Bytes* payload, std::string* from_address, std::uint16_t* from_port,
            int timeout_ms) {
    struct pollfd descriptor {};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready <= 0)
      return false;
    struct sockaddr_in remote {};
    socklen_t length = sizeof(remote);
    std::vector<std::uint8_t> buffer(65536);
    const ssize_t received =
        ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&remote), &length);
    if (received <= 0)
      return false;
    payload->assign(buffer.begin(), buffer.begin() + received);
    char text[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &remote.sin_addr, text, sizeof(text));
    *from_address = text;
    *from_port = ntohs(remote.sin_port);
    if (pcap_)
      pcap_->Write(*from_address, *from_port, local_address_, local_port_, payload->data(),
                   payload->size());
    return true;
  }

 private:
  int fd_ = -1;
  std::string local_address_ = "127.0.0.1";
  std::uint16_t local_port_ = 0;
  PcapWriter* pcap_ = nullptr;
};

// ---- small wire helpers --------------------------------------------------

void PutBe32(gm::Bytes* out, std::uint32_t value) {
  out->push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
  out->push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
  out->push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
  out->push_back(static_cast<unsigned char>(value & 0xFF));
}

std::uint32_t ReadBe32(const gm::Bytes& data, std::size_t offset) {
  return (static_cast<std::uint32_t>(data[offset]) << 24) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
         static_cast<std::uint32_t>(data[offset + 3]);
}

gm::Bytes MakeSigEnvelope(std::uint8_t type, const gm::Bytes& payload) {
  gm::Bytes out = {'G', 'S', 'I', 'G', type, 0, 0, 0};
  PutBe32(&out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool ParseSigEnvelope(const gm::Bytes& wire, std::uint8_t* type, gm::Bytes* payload) {
  if (wire.size() < 12 || std::memcmp(wire.data(), "GSIG", 4) != 0)
    return false;
  *type = wire[4];
  const std::uint32_t length = ReadBe32(wire, 8);
  if (wire.size() != 12 + length)
    return false;
  payload->assign(wire.begin() + 12, wire.end());
  return true;
}

struct EndMarker {
  std::uint32_t frame_count = 0;
  std::uint32_t signature_count = 0;
  std::string plaintext_sha256;
};

gm::Bytes EncodeEndMarker(const EndMarker& marker) {
  gm::Bytes out;
  PutBe32(&out, marker.frame_count);
  PutBe32(&out, marker.signature_count);
  out.insert(out.end(), marker.plaintext_sha256.begin(), marker.plaintext_sha256.end());
  return out;
}

bool DecodeEndMarker(const gm::Bytes& wire, EndMarker* marker) {
  if (wire.size() != 8 + 64)
    return false;
  marker->frame_count = ReadBe32(wire, 0);
  marker->signature_count = ReadBe32(wire, 4);
  marker->plaintext_sha256.assign(wire.begin() + 8, wire.end());
  return true;
}

struct RtpDatagram {
  std::uint32_t timestamp = 0;
  std::uint16_t sequence = 0;
  bool marker = false;
  gm::Bytes payload;
};

} // namespace

// ---- platform role -------------------------------------------------------

namespace {

struct PlatformOptions {
  fs::path workdir;
  std::string bind = "127.0.0.1";
  std::uint16_t negotiate_port = kDefaultNegotiatePort;
  std::uint16_t media_port = 0;
  std::uint16_t signature_port = 0;
  fs::path pcap;
  std::string stream_id = "34020000001320000001";
  int timeout_ms = 30000;
};

std::optional<SipMessage> RecvSip(UdpSocket* socket, std::string* from_address,
                                  std::uint16_t* from_port, int timeout_ms) {
  gm::Bytes datagram;
  if (!socket->Recv(&datagram, from_address, from_port, timeout_ms))
    return std::nullopt;
  SipMessage parsed;
  if (!ParseSipMessage(datagram, &parsed))
    return std::nullopt;
  return parsed;
}

int RunPlatform(const PlatformOptions& options) {
  const std::string device_public_key =
      ReadFile(options.workdir / "device_public_key.pem");
  const std::string platform_private_key =
      ReadFile(options.workdir / "platform_private_key.pem");
  if (device_public_key.empty() || platform_private_key.empty()) {
    Fail("missing device_public_key.pem or platform_private_key.pem in " +
         options.workdir.string());
    return 1;
  }

  PcapWriter pcap;
  if (!pcap.Open(options.pcap)) {
    Fail("cannot open pcap " + options.pcap.string());
    return 1;
  }

  std::string error;
  UdpSocket negotiate;
  UdpSocket media;
  UdpSocket signature;
  if (!negotiate.Open(options.negotiate_port, options.bind, &pcap, &error) ||
      !media.Open(options.media_port, options.bind, &pcap, &error) ||
      !signature.Open(options.signature_port, options.bind, &pcap, &error)) {
    Fail("cannot bind UDP sockets: " + error);
    return 1;
  }

  std::printf("platform ready negotiate=%u media=%u signature=%u pcap=%s\n",
              negotiate.LocalPort(), media.LocalPort(), signature.LocalPort(),
              options.pcap.string().c_str());
  std::fflush(stdout);

  std::string device_address;
  std::uint16_t device_port = 0;
  auto hello_message =
      RecvSip(&negotiate, &device_address, &device_port, options.timeout_ms);
  if (!hello_message || hello_message->type != "ClientHello") {
    Fail("did not receive a ClientHello SIP MESSAGE");
    return 1;
  }
  gm::Bytes client_hello_wire;
  if (!gb35114::DecodeMessageBase64(
          std::string(hello_message->body.begin(), hello_message->body.end()),
          &client_hello_wire, &error)) {
    Fail("ClientHello base64: " + error);
    return 1;
  }
  gb35114::ClientHello hello;
  if (!gb35114::ParseClientHello(client_hello_wire, &hello, &error)) {
    Fail("ClientHello parse: " + error);
    return 1;
  }
  const bool device_authenticated =
      gm::Sm2Verify(device_public_key, BytesView(gb35114::ClientHelloSignedData(hello)),
                    hello.signature, &error);
  std::printf("platform: ClientHello from device=%s nonce=%s identity=%s\n",
              hello.device_id.c_str(), gm::ToHex(hello.device_nonce).substr(0, 16).c_str(),
              device_authenticated ? "authenticated" : "rejected");
  if (!device_authenticated) {
    Fail("device ClientHello signature did not verify");
    return 1;
  }

  // ServerHello: the platform proves possession of its identity key and selects
  // the A-level signature suite and the B-level SM4-CTR cipher suite.
  gb35114::ServerHello server_hello;
  server_hello.session_id = gm::Bytes(16);
  gm::Bytes session_random;
  if (!gm::RandomBytes(16, &session_random, &error))
    return 1;
  server_hello.session_id = session_random;
  if (!gm::RandomBytes(gb35114::kNonceSize, &server_hello.platform_nonce, &error))
    return 1;
  server_hello.device_nonce = hello.device_nonce;
  server_hello.signature_suite = gb35114::kSignatureSuiteSm2Sm3;
  server_hello.cipher_suite = gb35114::kCipherSuiteSm4Ctr;
  server_hello.sign_mode = gb35114::kSignModeIFrames;
  server_hello.record_version = gb35114::kSignatureRecordVersion;
  server_hello.timestamp_ms = NowMs();
  if (!gm::Sm2Sign(platform_private_key, BytesView(gb35114::ServerHelloSignedData(server_hello)),
                   &server_hello.signature, &error)) {
    Fail("ServerHello signing: " + error);
    return 1;
  }
  const gm::Bytes server_hello_wire = gb35114::SerializeServerHello(server_hello, &error);
  if (server_hello_wire.empty()) {
    Fail("ServerHello serialisation: " + error);
    return 1;
  }
  const std::string server_hello_body = gb35114::EncodeMessageBase64(server_hello_wire);
  if (!negotiate.SendTo(device_address, device_port,
                        BuildSipMessage("ServerHello", "sip:platform@127.0.0.1",
                                        "sip:device@127.0.0.1", negotiate.LocalPort(),
                                        server_hello_body, "application/vnd.uvnvr.gb35114"))) {
    Fail("cannot send ServerHello");
    return 1;
  }

  // VKEK delivery: random 16-byte working key, encrypted to the device and
  // signed by the platform, bound to the transcript of both hello messages.
  gm::Bytes vkek;
  if (!gm::RandomBytes(gb35114::kVkekSize, &vkek, &error)) {
    Fail("VKEK random: " + error);
    return 1;
  }
  gb35114::VkekDelivery delivery;
  delivery.session_id = server_hello.session_id;
  delivery.key_id = 1;
  delivery.key_version = 1;
  delivery.sequence = 1;
  delivery.not_before_ms = NowMs() - 1000;
  delivery.not_after_ms = NowMs() + 3600 * 1000;
  delivery.timestamp_ms = NowMs();
  delivery.device_nonce = hello.device_nonce;
  delivery.transcript_hash = gb35114::ComputeTranscriptHash(client_hello_wire, server_hello_wire);
  if (!gm::Sm2Encrypt(device_public_key, vkek, &delivery.encrypted_vkek, &error)) {
    Fail("VKEK encryption: " + error);
    return 1;
  }
  if (!gm::Sm2Sign(platform_private_key,
                   BytesView(gb35114::VkekDeliverySignedData(delivery)), &delivery.signature,
                   &error)) {
    Fail("VKEK delivery signing: " + error);
    return 1;
  }
  const gm::Bytes delivery_wire = gb35114::SerializeVkekDelivery(delivery, &error);
  if (delivery_wire.empty()) {
    Fail("VKEK delivery serialisation: " + error);
    return 1;
  }
  if (!negotiate.SendTo(device_address, device_port,
                        BuildSipMessage("VkekDelivery", "sip:platform@127.0.0.1",
                                        "sip:device@127.0.0.1", negotiate.LocalPort(),
                                        gb35114::EncodeMessageBase64(delivery_wire),
                                        "application/vnd.uvnvr.gb35114"))) {
    Fail("cannot send VkekDelivery");
    return 1;
  }

  auto ack_message =
      RecvSip(&negotiate, &device_address, &device_port, options.timeout_ms);
  if (!ack_message || ack_message->type != "VkekAck") {
    Fail("did not receive a VkekAck");
    return 1;
  }
  gm::Bytes ack_wire;
  if (!gb35114::DecodeMessageBase64(std::string(ack_message->body.begin(), ack_message->body.end()),
                                    &ack_wire, &error)) {
    Fail("VkekAck base64: " + error);
    return 1;
  }
  gb35114::VkekAck ack;
  if (!gb35114::ParseVkekAck(ack_wire, &ack, &error)) {
    Fail("VkekAck parse: " + error);
    return 1;
  }
  const bool ack_valid =
      ack.status == gb35114::kVkekAckAccepted &&
      gm::Sm2Verify(device_public_key, BytesView(gb35114::VkekAckSignedData(ack)), ack.signature,
                    &error);
  std::printf("platform: VkekAck key_id=%llu status=%s signature=%s\n",
              static_cast<unsigned long long>(ack.key_id),
              ack.status == gb35114::kVkekAckAccepted ? "accepted" : "rejected",
              ack_valid ? "verified" : "invalid");
  if (!ack_valid) {
    Fail("VkekAck did not verify");
    return 1;
  }

  // The B-level stream keys are derived from the negotiated VKEK. The session
  // id is the 16-byte HKDF salt, so the media keys are bound to this
  // authenticated session.
  gb35114::VideoKeys keys;
  if (!gb35114::DeriveStreamKeys(vkek, delivery.key_id, server_hello.session_id, options.stream_id,
                                 &keys, &error)) {
    Fail("stream key derivation: " + error);
    return 1;
  }

  // Media phase: collect RTP and the side-channel records until the end marker.
  std::vector<RtpDatagram> rtp;
  std::vector<std::pair<std::uint8_t, gm::Bytes>> records;
  std::optional<EndMarker> end_marker;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(options.timeout_ms);
  while (std::chrono::steady_clock::now() < deadline && !end_marker) {
    struct pollfd descriptors[2] = {{media.fd_for_poll(), POLLIN, 0},
                                    {signature.fd_for_poll(), POLLIN, 0}};
    const int ready = ::poll(descriptors, 2, 500);
    if (ready <= 0)
      continue;
    if ((descriptors[0].revents & POLLIN) != 0) {
      gm::Bytes datagram;
      std::string from;
      std::uint16_t port = 0;
      if (media.Recv(&datagram, &from, &port, 0)) {
        gb28181::RtpPacketHeader header;
        if (gb28181::ParseRtpPacket(datagram.data(), datagram.size(), &header, &error)) {
          RtpDatagram packet;
          packet.timestamp = header.timestamp;
          packet.sequence = header.sequence;
          packet.marker = header.marker;
          packet.payload.assign(datagram.begin() +
                                   static_cast<std::ptrdiff_t>(header.header_size),
                               datagram.end());
          rtp.push_back(std::move(packet));
        }
      }
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      gm::Bytes datagram;
      std::string from;
      std::uint16_t port = 0;
      if (signature.Recv(&datagram, &from, &port, 0)) {
        std::uint8_t type = 0;
        gm::Bytes payload;
        if (!ParseSigEnvelope(datagram, &type, &payload)) {
          Fail("malformed side-channel datagram");
          continue;
        }
        if (type == 0) {
          EndMarker marker;
          if (DecodeEndMarker(payload, &marker))
            end_marker = marker;
        } else {
          records.emplace_back(type, std::move(payload));
        }
      }
    }
  }
  if (!end_marker) {
    Fail("media phase timed out before the end marker");
    return 1;
  }

  // Reassemble the RTP payloads, then decrypt every sealed frame.
  std::map<std::uint32_t, gm::Bytes> frames;
  for (const RtpDatagram& packet : rtp)
    frames[packet.timestamp].insert(frames[packet.timestamp].end(), packet.payload.begin(),
                                    packet.payload.end());
  gm::Bytes plaintext;
  std::uint32_t decrypted = 0;
  for (const auto& [timestamp, sealed] : frames) {
    gm::Bytes frame;
    gb35114::SealedFrameInfo info;
    if (!gb35114::OpenEsFrame(keys, sealed, &frame, &info, &error)) {
      Fail("sealed frame decryption at timestamp " + std::to_string(timestamp) + ": " + error);
      return 1;
    }
    plaintext.insert(plaintext.end(), frame.begin(), frame.end());
    ++decrypted;
  }

  // A-level verification against the decrypted access units.
  std::vector<gm::Bytes> access_units;
  {
    gb35114::AccessUnitReader reader(gb35114::InputFormat::kAnnexB);
    reader.SetSink([&access_units](gb35114::AccessUnit&& unit, std::string*) {
      access_units.push_back(std::move(unit.data));
      return true;
    });
    if (!reader.Push(plaintext.data(), plaintext.size(), &error) ||
        !reader.Flush(&error)) {
      Fail("cannot split the decrypted stream: " + error);
      return 1;
    }
  }

  gb35114::VerifyOptions verify_options;
  verify_options.public_key_pem = device_public_key;
  std::uint32_t signatures_ok = 0;
  std::uint32_t signatures_bad = 0;
  bool signature_tamper_rejected = false;
  bool cipher_tamper_rejected = false;
  for (const auto& [type, payload] : records) {
    if (type == 1) {
      gb35114::FrameSignature frame_signature;
      std::size_t consumed = 0;
      if (!gb35114::ParseFrameSignature(payload.data(), payload.size(), &frame_signature,
                                        &consumed, &error)) {
        ++signatures_bad;
        continue;
      }
      if (frame_signature.frame_index >= access_units.size()) {
        ++signatures_bad;
        continue;
      }
      const gm::Bytes& frame = access_units[frame_signature.frame_index];
      if (gb35114::VerifyFrameSignature(verify_options, frame.data(), frame.size(),
                                        frame_signature, &error))
        ++signatures_ok;
      else
        ++signatures_bad;
    } else if (type == 2) {
      gb35114::FrameSignature frame_signature;
      std::size_t consumed = 0;
      if (gb35114::ParseFrameSignature(payload.data(), payload.size(), &frame_signature,
                                       &consumed, &error) &&
          !access_units.empty()) {
        const gm::Bytes& frame = access_units.front();
        signature_tamper_rejected =
            !gb35114::VerifyFrameSignature(verify_options, frame.data(), frame.size(),
                                           frame_signature, &error);
      }
    } else if (type == 3) {
      gm::Bytes output;
      gb35114::SealedFrameInfo info;
      cipher_tamper_rejected = !gb35114::OpenEsFrame(keys, payload, &output, &info, &error);
    }
  }

  const std::string plaintext_sha256 = Sha256Hex(plaintext);
  const bool plaintext_match =
      plaintext_sha256 == end_marker->plaintext_sha256 && decrypted == end_marker->frame_count;
  const bool identity_ok = device_authenticated && ack_valid;
  const bool signature_ok =
      signatures_bad == 0 && signatures_ok == end_marker->signature_count &&
      signatures_ok > 0;
  const bool overall = identity_ok && signature_ok && plaintext_match && decrypted > 0 &&
                       signature_tamper_rejected && cipher_tamper_rejected;

  std::printf("platform: identity=%s signatures=%u/%u tampered_signature=%s\n",
              identity_ok ? "ok" : "fail", signatures_ok, end_marker->signature_count,
              signature_tamper_rejected ? "rejected" : "accepted");
  std::printf("platform: cipher_frames=%u plaintext_sha256=%s plaintext_match=%s "
              "tampered_ciphertext=%s\n",
              decrypted, plaintext_sha256.c_str(), plaintext_match ? "yes" : "no",
              cipher_tamper_rejected ? "rejected" : "accepted");
  std::printf("platform: overall=%s\n", overall ? "ok" : "fail");
  std::fflush(stdout);

  std::string report;
  report += "identity=" + std::string(identity_ok ? "ok" : "fail") + "\n";
  report += "signature_verified=" + std::to_string(signatures_ok) + "\n";
  report += "signature_total=" + std::to_string(end_marker->signature_count) + "\n";
  report += "signature_tamper=" +
            std::string(signature_tamper_rejected ? "rejected" : "accepted") + "\n";
  report += "cipher_frames=" + std::to_string(decrypted) + "\n";
  report += "plaintext_sha256=" + plaintext_sha256 + "\n";
  report += "plaintext_match=" + std::string(plaintext_match ? "yes" : "no") + "\n";
  report += "cipher_tamper=" +
            std::string(cipher_tamper_rejected ? "rejected" : "accepted") + "\n";
  report += "overall=" + std::string(overall ? "ok" : "fail") + "\n";
  negotiate.SendTo(device_address, device_port,
                   BuildSipMessage("Report", "sip:platform@127.0.0.1", "sip:device@127.0.0.1",
                                   negotiate.LocalPort(), report, "text/plain"));
  return overall ? 0 : 1;
}

} // namespace

// ---- device role ---------------------------------------------------------

namespace {

struct DeviceOptions {
  fs::path workdir;
  std::string host = "127.0.0.1";
  std::uint16_t negotiate_port = 0;
  std::uint16_t media_port = 0;
  std::uint16_t signature_port = 0;
  fs::path h264;
  std::string stream_id = "34020000001320000001";
  int timeout_ms = 30000;
};

bool SendRecord(UdpSocket* socket, const std::string& host, std::uint16_t port,
                std::string_view type, const gm::Bytes& wire) {
  return socket->SendTo(host, port,
                        BuildSipMessage(type, "sip:device@127.0.0.1", "sip:platform@127.0.0.1",
                                        socket->LocalPort(),
                                        gb35114::EncodeMessageBase64(wire),
                                        "application/vnd.uvnvr.gb35114"));
}

int RunDevice(const DeviceOptions& options) {
  const std::string device_private_key =
      ReadFile(options.workdir / "device_private_key.pem");
  const std::string platform_public_key =
      ReadFile(options.workdir / "platform_public_key.pem");
  if (device_private_key.empty() || platform_public_key.empty()) {
    Fail("missing device_private_key.pem or platform_public_key.pem");
    return 1;
  }
  const gm::Bytes h264 = ReadBytes(options.h264);
  if (h264.empty()) {
    Fail("cannot read the H.264 fixture " + options.h264.string());
    return 1;
  }

  std::string error;
  UdpSocket negotiate;
  if (!negotiate.Open(0, "127.0.0.1", nullptr, &error)) {
    Fail("cannot bind the negotiation socket: " + error);
    return 1;
  }

  gb35114::VkekConfig config;
  config.device_id = options.stream_id;
  config.device_private_key_pem = device_private_key;
  config.platform_public_key_pem = platform_public_key;
  config.allowed_clock_skew_ms = kClockSkewMs;
  gb35114::VkekManager manager;
  if (!manager.Configure(std::move(config), &error)) {
    Fail("VkekManager configure: " + error);
    return 1;
  }

  gm::Bytes client_hello_wire;
  if (!manager.CreateClientHello(&client_hello_wire, &error)) {
    Fail("CreateClientHello: " + error);
    return 1;
  }
  if (!SendRecord(&negotiate, options.host, options.negotiate_port, "ClientHello",
                  client_hello_wire)) {
    Fail("cannot send ClientHello");
    return 1;
  }
  std::printf("device: ClientHello sent (%zu bytes)\n", client_hello_wire.size());

  gm::Bytes server_hello_wire;
  for (int attempt = 0; attempt < 3;) {
    gm::Bytes datagram;
    std::string from;
    std::uint16_t port = 0;
    if (!negotiate.Recv(&datagram, &from, &port, options.timeout_ms)) {
      Fail("timed out waiting for ServerHello");
      return 1;
    }
    SipMessage message;
    if (!ParseSipMessage(datagram, &message))
      continue;
    if (message.type != "ServerHello")
      continue;
    if (!gb35114::DecodeMessageBase64(std::string(message.body.begin(), message.body.end()),
                                      &server_hello_wire, &error)) {
      Fail("ServerHello base64: " + error);
      return 1;
    }
    ++attempt;
    break;
  }
  const gb35114::VkekStatus hello_status = manager.ProcessServerHello(server_hello_wire, &error);
  std::printf("device: ServerHello %s\n",
              std::string(gb35114::VkekStatusName(hello_status)).c_str());
  if (hello_status != gb35114::VkekStatus::kOk) {
    Fail("platform authentication failed: " + error);
    return 1;
  }

  gm::Bytes delivery_wire;
  for (;;) {
    gm::Bytes datagram;
    std::string from;
    std::uint16_t port = 0;
    if (!negotiate.Recv(&datagram, &from, &port, options.timeout_ms)) {
      Fail("timed out waiting for VkekDelivery");
      return 1;
    }
    SipMessage message;
    if (!ParseSipMessage(datagram, &message))
      continue;
    if (message.type != "VkekDelivery")
      continue;
    if (!gb35114::DecodeMessageBase64(std::string(message.body.begin(), message.body.end()),
                                      &delivery_wire, &error)) {
      Fail("VkekDelivery base64: " + error);
      return 1;
    }
    break;
  }
  gm::Bytes ack_wire;
  const gb35114::VkekStatus delivery_status =
      manager.ProcessVkekDelivery(delivery_wire, &ack_wire, &error);
  std::printf("device: VkekDelivery %s\n",
              std::string(gb35114::VkekStatusName(delivery_status)).c_str());
  if (delivery_status != gb35114::VkekStatus::kOk) {
    Fail("VKEK delivery rejected: " + error);
    return 1;
  }
  if (!SendRecord(&negotiate, options.host, options.negotiate_port, "VkekAck", ack_wire)) {
    Fail("cannot send VkekAck");
    return 1;
  }

  gb35114::SessionInfo session;
  gm::Bytes vkek;
  if (!manager.GetSessionInfo(&session, &error) ||
      !manager.CopyActiveKey(&vkek, nullptr, &error)) {
    Fail("cannot read the negotiated session: " + error);
    return 1;
  }
  const auto key_info = manager.active_key_info();
  gb35114::VideoKeys keys;
  if (!gb35114::DeriveStreamKeys(vkek, static_cast<std::uint32_t>(key_info.key_id),
                                 session.session_id, options.stream_id, &keys, &error)) {
    Fail("stream key derivation: " + error);
    return 1;
  }
  std::printf("device: B-level keys derived key_id=%llu session=%s\n",
              static_cast<unsigned long long>(key_info.key_id),
              gm::ToHex(session.session_id).substr(0, 16).c_str());

  // Split the Annex B stream into access units, sign the key frames and seal
  // every unit. The sealed frames are then carried as RTP payload type 96.
  std::vector<gb35114::AccessUnit> units;
  {
    gb35114::AccessUnitReader reader(gb35114::InputFormat::kAnnexB);
    reader.SetSink([&units](gb35114::AccessUnit&& unit, std::string*) {
      units.push_back(std::move(unit));
      return true;
    });
    if (!reader.Push(h264.data(), h264.size(), &error) || !reader.Flush(&error)) {
      Fail("access-unit split: " + error);
      return 1;
    }
  }
  if (units.empty()) {
    Fail("the fixture produced no access units");
    return 1;
  }

  gb28181::RtpSessionConfig rtp_config;
  rtp_config.mode = gb28181::RtpTransportMode::kUdp;
  rtp_config.local_address = "127.0.0.1";
  rtp_config.remote_address = options.host;
  rtp_config.remote_port = options.media_port;
  rtp_config.payload_type = 96;
  rtp_config.ssrc = kRtpSsrc;
  gb28181::RtpSession rtp(rtp_config);
  if (!rtp.Open(&error)) {
    Fail("RTP session: " + error);
    return 1;
  }

  UdpSocket sig_socket;
  if (!sig_socket.Open(0, "127.0.0.1", nullptr, &error)) {
    Fail("cannot bind the signature socket: " + error);
    return 1;
  }

  gm::Bytes plaintext;
  gm::Bytes first_sealed;
  gm::Bytes first_record;
  std::uint32_t signature_count = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const gb35114::AccessUnit& unit = units[i];
    plaintext.insert(plaintext.end(), unit.data.begin(), unit.data.end());

    if (unit.key_frame) {
      gb35114::FrameSignature signature;
      if (!gb35114::CreateFrameSignature(device_private_key, 1,
                                         static_cast<std::uint64_t>(i), unit.pts, unit.has_pts,
                                         true, unit.data.data(), unit.data.size(), &signature,
                                         &error)) {
        Fail("frame signing: " + error);
        return 1;
      }
      const gm::Bytes record = gb35114::SerializeFrameSignature(signature, &error);
      if (record.empty()) {
        Fail("signature serialisation: " + error);
        return 1;
      }
      if (first_record.empty())
        first_record = record;
      const gm::Bytes envelope = MakeSigEnvelope(1, record);
      if (!sig_socket.SendTo(options.host, options.signature_port,
                             std::string(envelope.begin(), envelope.end()))) {
        Fail("cannot send the signature record");
        return 1;
      }
      ++signature_count;
    }

    gm::Bytes nonce;
    if (!gm::RandomBytes(gb35114::kSealedFrameNonceSize, &nonce, &error)) {
      Fail("nonce: " + error);
      return 1;
    }
    gm::Bytes sealed;
    if (!gb35114::SealEsFrame(keys, i, unit.key_frame, nonce, unit.data, &sealed, &error)) {
      Fail("frame sealing: " + error);
      return 1;
    }
    if (first_sealed.empty())
      first_sealed = sealed;
    if (!rtp.SendFrame(sealed.data(), sealed.size(),
                       static_cast<std::uint32_t>(i) * 3000, unit.key_frame, &error)) {
      Fail("RTP send: " + error);
      return 1;
    }
  }

  // Negative probes: a modified A-level digest and a modified B-level
  // ciphertext must both be rejected by the platform.
  if (!first_record.empty()) {
    gm::Bytes tampered = first_record;
    tampered[48] ^= 0x01; // first digest byte in the 88-byte record header
    const gm::Bytes envelope = MakeSigEnvelope(2, tampered);
    sig_socket.SendTo(options.host, options.signature_port,
                      std::string(envelope.begin(), envelope.end()));
  }
  if (!first_sealed.empty()) {
    gm::Bytes tampered = first_sealed;
    tampered.back() ^= 0x01;
    const gm::Bytes envelope = MakeSigEnvelope(3, tampered);
    sig_socket.SendTo(options.host, options.signature_port,
                     std::string(envelope.begin(), envelope.end()));
  }

  EndMarker marker;
  marker.frame_count = static_cast<std::uint32_t>(units.size());
  marker.signature_count = signature_count;
  marker.plaintext_sha256 = Sha256Hex(plaintext);
  const gm::Bytes marker_wire = EncodeEndMarker(marker);
  const gm::Bytes marker_envelope = MakeSigEnvelope(0, marker_wire);
  sig_socket.SendTo(options.host, options.signature_port,
                    std::string(marker_envelope.begin(), marker_envelope.end()));

  std::printf("device: sent %zu sealed frames, %u signatures, plaintext_sha256=%s\n",
              units.size(), signature_count, marker.plaintext_sha256.c_str());

  // Wait for the platform verdict.
  std::string report;
  for (;;) {
    gm::Bytes datagram;
    std::string from;
    std::uint16_t port = 0;
    if (!negotiate.Recv(&datagram, &from, &port, options.timeout_ms)) {
      Fail("timed out waiting for the platform report");
      return 1;
    }
    SipMessage message;
    if (!ParseSipMessage(datagram, &message))
      continue;
    if (message.type != "Report")
      continue;
    report.assign(message.body.begin(), message.body.end());
    break;
  }
  std::printf("device: platform report\n%s", report.c_str());
  const bool ok = report.find("overall=ok") != std::string::npos;
  rtp.Close();
  return ok ? 0 : 1;
}

} // namespace

// ---- entry point ---------------------------------------------------------

namespace {

int RunKeygen(const fs::path& directory) {
  std::string error;
  gm::Sm2KeyPair device;
  gm::Sm2KeyPair platform;
  if (!gm::GenerateSm2KeyPair(&device, &error) ||
      !gm::GenerateSm2KeyPair(&platform, &error)) {
    Fail("key generation: " + error);
    return 1;
  }
  if (!WriteFile(directory / "device_private_key.pem", device.private_key_pem) ||
      !WriteFile(directory / "device_public_key.pem", device.public_key_pem) ||
      !WriteFile(directory / "platform_private_key.pem", platform.private_key_pem) ||
      !WriteFile(directory / "platform_public_key.pem", platform.public_key_pem)) {
    Fail("cannot write the key files");
    return 1;
  }
  std::printf("wrote SM2 device and platform identities to %s\n", directory.string().c_str());
  return 0;
}

void Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  gb35114_interop keygen --dir <dir>\n"
               "  gb35114_interop platform --dir <dir> --pcap <file> [--port N] [--media-port N]\n"
               "                          [--signature-port N] [--stream-id ID] [--timeout-ms N]\n"
               "  gb35114_interop device --dir <dir> --negotiate-port N --media-port N\n"
               "                        --signature-port N --h264 <file> [--host HOST]\n"
               "                        [--stream-id ID] [--timeout-ms N]\n");
}

bool NextValue(int argc, char** argv, int* index, std::string* value) {
  if (*index + 1 >= argc)
    return false;
  *value = argv[++*index];
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "keygen") {
    fs::path directory;
    for (int i = 2; i < argc; ++i) {
      const std::string token = argv[i];
      if (token == "--dir") {
        std::string value;
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        directory = value;
      }
    }
    if (directory.empty()) {
      Usage();
      return 2;
    }
    fs::create_directories(directory);
    return RunKeygen(directory);
  }
  if (command == "platform") {
    PlatformOptions options;
    for (int i = 2; i < argc; ++i) {
      const std::string token = argv[i];
      std::string value;
      if (token == "--dir") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.workdir = value;
      } else if (token == "--pcap") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.pcap = value;
      } else if (token == "--port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.negotiate_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--media-port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.media_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--signature-port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.signature_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--stream-id") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.stream_id = value;
      } else if (token == "--timeout-ms") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.timeout_ms = std::stoi(value);
      }
    }
    if (options.workdir.empty() || options.pcap.empty()) {
      Usage();
      return 2;
    }
    return RunPlatform(options);
  }
  if (command == "device") {
    DeviceOptions options;
    for (int i = 2; i < argc; ++i) {
      const std::string token = argv[i];
      std::string value;
      if (token == "--dir") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.workdir = value;
      } else if (token == "--host") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.host = value;
      } else if (token == "--negotiate-port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.negotiate_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--media-port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.media_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--signature-port") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.signature_port = static_cast<std::uint16_t>(std::stoul(value));
      } else if (token == "--h264") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.h264 = value;
      } else if (token == "--stream-id") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.stream_id = value;
      } else if (token == "--timeout-ms") {
        if (!NextValue(argc, argv, &i, &value))
          return 2;
        options.timeout_ms = std::stoi(value);
      }
    }
    if (options.workdir.empty() || options.h264.empty() || options.negotiate_port == 0 ||
        options.media_port == 0 || options.signature_port == 0) {
      Usage();
      return 2;
    }
    return RunDevice(options);
  }
  Usage();
  return 2;
}
