#include "onvif.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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
bool EnsureWinsock(std::string* error) {
  static bool started = false;
  if (started)
    return true;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    if (error)
      *error = "WSAStartup failed";
    return false;
  }
  started = true;
  return true;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket s) {
  if (s >= 0)
    close(s);
}
bool EnsureWinsock(std::string*) {
  return true;
}
#endif

std::string Base64(const unsigned char* data, size_t size) {
  if (size == 0)
    return {};
  std::string out(4 * ((size + 2) / 3), '\0');
  const int written =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
  if (written <= 0)
    return {};
  out.resize(static_cast<size_t>(written));
  return out;
}

std::string RandomBytesBase64(size_t size) {
  std::vector<unsigned char> buffer(size);
  if (RAND_bytes(buffer.data(), static_cast<int>(size)) != 1)
    return {};
  return Base64(buffer.data(), buffer.size());
}

std::string IsoUtcNow() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::string XmlEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
    }
  }
  return out;
}

std::string HttpPost(const std::string& url, const std::string& body, std::string* error) {
  if (url.rfind("http://", 0) != 0) {
    if (error)
      *error = "only http:// ONVIF endpoints are supported";
    return {};
  }
  if (!EnsureWinsock(error))
    return {};
  const size_t host_start = 7;
  const size_t path_start = url.find('/', host_start);
  const std::string authority = url.substr(
      host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
  const std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
  std::string host = authority;
  std::string port = "80";
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    port = authority.substr(colon + 1);
  }
  if (host.empty()) {
    if (error)
      *error = "missing ONVIF host";
    return {};
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
    if (error)
      *error = "cannot resolve " + host;
    return {};
  }
  Socket socket = kInvalidSocket;
  for (auto* entry = result; entry != nullptr; entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket == kInvalidSocket)
      continue;
#ifdef SO_RCVTIMEO
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#endif
    if (::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0)
      break;
    CloseSocket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(result);
  if (socket == kInvalidSocket) {
    if (error)
      *error = "cannot connect to " + host + ":" + port;
    return {};
  }

  const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + authority +
                              "\r\nContent-Type: application/soap+xml; charset=utf-8\r\n" +
                              "Content-Length: " + std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < request.size()) {
    const int n = static_cast<int>(
        ::send(socket, request.data() + sent, static_cast<int>(request.size() - sent), 0));
    if (n <= 0) {
      CloseSocket(socket);
      if (error)
        *error = "failed to send SOAP request";
      return {};
    }
    sent += static_cast<size_t>(n);
  }

  std::string response;
  std::array<char, 8192> buffer{};
  while (true) {
    const int n = static_cast<int>(::recv(socket, buffer.data(), buffer.size(), 0));
    if (n <= 0)
      break;
    response.append(buffer.data(), static_cast<size_t>(n));
    if (response.size() > 4 * 1024 * 1024)
      break;
  }
  CloseSocket(socket);
  const size_t body_start = response.find("\r\n\r\n");
  if (body_start == std::string::npos) {
    if (error)
      *error = "invalid HTTP response from camera";
    return {};
  }
  return response.substr(body_start + 4);
}

std::string ExtractTag(const std::string& xml, const std::string& tag) {
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' ||
        xml[pos + 1] == '!') {
      ++pos;
      continue;
    }
    const size_t name_start = pos + 1;
    size_t name_end = name_start;
    while (name_end < xml.size() &&
           (std::isalnum(static_cast<unsigned char>(xml[name_end])) || xml[name_end] == '_' ||
            xml[name_end] == '-' || xml[name_end] == ':' || xml[name_end] == '.')) {
      ++name_end;
    }
    std::string name = xml.substr(name_start, name_end - name_start);
    const size_t colon = name.rfind(':');
    if (colon != std::string::npos)
      name = name.substr(colon + 1);
    if (name != tag) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string::npos)
      return {};
    if (xml[open_end - 1] == '/')
      return {};
    const std::string closing = "</" + xml.substr(name_start, name_end - name_start) + ">";
    const size_t close_start = xml.find(closing, open_end + 1);
    if (close_start == std::string::npos) {
      // Some devices use a different prefix in the closing tag. Search for
      // </*:tag> by walking the same name rules.
      size_t scan = open_end + 1;
      while ((scan = xml.find("</", scan)) != std::string::npos) {
        size_t end = scan + 2;
        while (end < xml.size() &&
               (std::isalnum(static_cast<unsigned char>(xml[end])) || xml[end] == '_' ||
                xml[end] == '-' || xml[end] == ':' || xml[end] == '.')) {
          ++end;
        }
        std::string close_name = xml.substr(scan + 2, end - (scan + 2));
        const size_t close_colon = close_name.rfind(':');
        if (close_colon != std::string::npos)
          close_name = close_name.substr(close_colon + 1);
        if (close_name == tag)
          return xml.substr(open_end + 1, scan - open_end - 1);
        scan = end;
      }
      return {};
    }
    return xml.substr(open_end + 1, close_start - open_end - 1);
  }
  return {};
}

std::string SecurityHeader(const std::string& username, const std::string& password) {
  if (username.empty())
    return {};
  const std::string nonce = RandomBytesBase64(16);
  const std::string created = IsoUtcNow();
  std::string digest_input = nonce + created + password;
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(digest_input.data()), digest_input.size(), digest);
  const std::string password_digest = Base64(digest, sizeof(digest));
  return "<wsse:Security s:mustUnderstand=\"1\" "
         "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
         "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
         "<wsse:UsernameToken><wsse:Username>" +
         XmlEscape(username) +
         "</wsse:Username>"
         "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
         password_digest +
         "</wsse:Password>"
         "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
         "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
         nonce +
         "</wsse:Nonce>"
         "<wsu:Created>" +
         created + "</wsu:Created></wsse:UsernameToken></wsse:Security>";
}

std::string Envelope(const std::string& security, const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
         "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
         "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
         "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
         "<s:Header>" +
         security + "</s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}

std::string SoapCall(const std::string& xaddr, const std::string& security, const std::string& body,
                     std::string* error) {
  return HttpPost(xaddr, Envelope(security, body), error);
}

std::string DeviceServiceUrl(const std::string& xaddr) {
  if (xaddr.find("/onvif/device_service") != std::string::npos)
    return xaddr;
  std::string base = xaddr;
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  return base + "/onvif/device_service";
}

} // namespace

bool DiscoverOnvif(std::vector<OnvifDevice>* devices, int timeout_ms, std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  const Socket socket = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket == kInvalidSocket) {
    if (error)
      *error = "cannot create the discovery socket";
    return false;
  }
  int reuse = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
#ifdef SO_RCVTIMEO
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(3702);
  inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);

  const std::string message =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
      "<e:Header><w:MessageID>uuid:" +
      RandomBytesBase64(12) +
      "</w:MessageID><w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
      "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>"
      "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
      "</e:Envelope>";
  if (::sendto(socket, message.data(), static_cast<int>(message.size()), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target)) < 0) {
    CloseSocket(socket);
    if (error)
      *error = "cannot send the ONVIF discovery probe";
    return false;
  }

  devices->clear();
  std::array<char, 8192> buffer{};
  while (true) {
    sockaddr_in from{};
#ifdef _WIN32
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = static_cast<int>(::recvfrom(socket, buffer.data(), buffer.size(), 0,
                                              reinterpret_cast<sockaddr*>(&from), &from_len));
    if (n <= 0)
      break;
    const std::string response(buffer.data(), static_cast<size_t>(n));
    std::string xaddrs = ExtractTag(response, "XAddrs");
    if (xaddrs.empty())
      xaddrs = ExtractTag(response, "XAddr");
    size_t start = 0;
    while (start < xaddrs.size()) {
      const size_t end = xaddrs.find_first_of(" \t\r\n", start);
      const std::string address =
          xaddrs.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (address.rfind("http://", 0) == 0) {
        OnvifDevice device;
        device.xaddr = address;
        const size_t host_start = 7;
        const size_t slash = address.find('/', host_start);
        device.ip = address.substr(host_start, slash == std::string::npos ? std::string::npos
                                                                          : slash - host_start);
        const size_t colon = device.ip.find(':');
        if (colon != std::string::npos)
          device.ip = device.ip.substr(0, colon);
        const bool duplicate =
            std::any_of(devices->begin(), devices->end(),
                        [&](const OnvifDevice& entry) { return entry.xaddr == device.xaddr; });
        if (!duplicate && !device.ip.empty())
          devices->push_back(device);
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
  }
  CloseSocket(socket);
  return true;
}

bool ProbeOnvif(const std::string& xaddr, const std::string& username, const std::string& password,
                OnvifInfo* info, std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  const std::string service = DeviceServiceUrl(xaddr);
  const std::string security = SecurityHeader(username, password);

  const std::string device_xml =
      SoapCall(service, security,
               "<GetDeviceInformation xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>", error);
  if (device_xml.empty())
    return false;
  info->manufacturer = ExtractTag(device_xml, "Manufacturer");
  info->model = ExtractTag(device_xml, "Model");
  info->firmware = ExtractTag(device_xml, "FirmwareVersion");
  info->serial = ExtractTag(device_xml, "SerialNumber");

  const std::string profiles_xml = SoapCall(
      service, security, "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>", error);
  if (profiles_xml.empty())
    return false;

  std::vector<std::string> tokens;
  size_t pos = 0;
  while ((pos = profiles_xml.find("Profiles", pos)) != std::string::npos) {
    const size_t token_pos = profiles_xml.find("token=\"", pos);
    if (token_pos == std::string::npos)
      break;
    const size_t token_start = token_pos + 7;
    const size_t token_end = profiles_xml.find('"', token_start);
    if (token_end == std::string::npos)
      break;
    const std::string token = profiles_xml.substr(token_start, token_end - token_start);
    if (std::find(tokens.begin(), tokens.end(), token) == tokens.end())
      tokens.push_back(token);
    pos = token_end;
  }

  for (const auto& token : tokens) {
    OnvifProfile profile;
    profile.token = token;
    const std::string stream_body =
        "<GetStreamUri xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<StreamSetup><Stream xmlns=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</Stream>"
        "<Transport xmlns=\"http://www.onvif.org/ver10/schema\"><Protocol>RTSP</Protocol>"
        "</Transport></StreamSetup><ProfileToken>" +
        XmlEscape(token) + "</ProfileToken></GetStreamUri>";
    const std::string stream_xml = SoapCall(service, security, stream_body, error);
    if (stream_xml.empty())
      continue;
    profile.stream_uri = ExtractTag(stream_xml, "Uri");
    const size_t profile_pos = profiles_xml.find("token=\"" + token + "\"");
    if (profile_pos != std::string::npos) {
      const size_t profile_end = profiles_xml.find("</", profile_pos);
      const std::string block = profiles_xml.substr(profile_pos, profile_end - profile_pos);
      profile.name = ExtractTag(block, "Name");
    }
    if (profile.name.empty())
      profile.name = token;
    info->profiles.push_back(std::move(profile));
  }
  return !info->profiles.empty();
}

} // namespace uvnvr
