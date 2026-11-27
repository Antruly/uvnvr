#include "detect.h"

#include "brand_http.h"
#include "onvif.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
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
    if (error != nullptr)
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

std::string Lower(std::string value) {
  for (char& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::string Md5Hex(const std::string& value) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_Digest(value.data(), value.size(), digest, &size, EVP_md5(), nullptr) != 1)
    return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<size_t>(size) * 2);
  for (unsigned int i = 0; i < size; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

std::string RandomHex(size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1)
    return "0123456789abcdef";
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char byte : buffer) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool ConnectWithTimeout(Socket socket, const struct sockaddr* addr, int addr_len, int seconds) {
  if (::connect(socket, addr, addr_len) == 0)
    return true;
  bool pending = false;
#ifdef _WIN32
  pending = (WSAGetLastError() == WSAEWOULDBLOCK);
  if (!pending)
    return false;
  u_long mode = 1;
  ioctlsocket(socket, FIONBIO, &mode);
#else
  pending = (errno == EINPROGRESS);
  if (!pending)
    return false;
  const int flags = fcntl(socket, F_GETFL, 0);
  fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
  fd_set writable;
  FD_ZERO(&writable);
  FD_SET(socket, &writable);
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  bool connected = false;
  if (::select(static_cast<int>(socket) + 1, nullptr, &writable, nullptr, &tv) > 0) {
    int so_error = 0;
#ifdef _WIN32
    int len = sizeof(so_error);
#else
    socklen_t len = sizeof(so_error);
#endif
    getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    connected = (so_error == 0);
  }
#ifdef _WIN32
  mode = 0;
  ioctlsocket(socket, FIONBIO, &mode);
#else
  fcntl(socket, F_SETFL, flags);
#endif
  return connected;
}

struct HeaderMap {
  int status = 0;
  std::string status_text;
  std::map<std::string, std::string> headers;
};

bool OpenSocket(const std::string& host, int port, int connect_seconds, Socket* out,
                std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
    if (error != nullptr)
      *error = "cannot resolve " + host;
    return false;
  }
  Socket socket = kInvalidSocket;
  for (auto* entry = result; entry != nullptr; entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket == kInvalidSocket)
      continue;
#ifdef SO_RCVTIMEO
    struct timeval timeout;
    timeout.tv_sec = connect_seconds + 1;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#endif
    if (ConnectWithTimeout(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen),
                           connect_seconds))
      break;
    CloseSocket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(result);
  if (socket == kInvalidSocket) {
    if (error != nullptr)
      *error = "connection refused or host unreachable (" + host + ":" + port_text + ")";
    return false;
  }
  *out = socket;
  return true;
}

bool SendAll(Socket socket, const std::string& data, std::string* error) {
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = static_cast<int>(
        ::send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0));
    if (n <= 0) {
      if (error != nullptr)
        *error = "the RTSP server closed the connection while sending";
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

// Reads a text response header block (RTSP uses the same layout as HTTP).
bool ReadHeaderBlock(Socket socket, HeaderMap* out, std::string* error) {
  std::string raw;
  std::array<char, 2048> buffer{};
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const int n = static_cast<int>(::recv(socket, buffer.data(), buffer.size(), 0));
    if (n <= 0)
      break;
    raw.append(buffer.data(), static_cast<size_t>(n));
    header_end = raw.find("\r\n\r\n");
    if (raw.size() > 64 * 1024)
      break;
  }
  if (header_end == std::string::npos) {
    if (error != nullptr)
      *error = raw.empty() ? "the RTSP server closed the connection without a response"
                           : "invalid RTSP response";
    return false;
  }
  std::istringstream lines(raw.substr(0, header_end));
  std::string line;
  bool first = true;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (first) {
      first = false;
      std::istringstream status(line);
      std::string version;
      status >> version >> out->status;
      std::getline(status, out->status_text);
      out->status_text = Trim(out->status_text);
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    out->headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
  }
  return true;
}

std::string HeaderValue(const HeaderMap& headers, const std::string& name) {
  const auto it = headers.headers.find(Lower(name));
  return it == headers.headers.end() ? std::string() : it->second;
}

std::string AuthParam(const std::string& challenge, const std::string& key) {
  const std::string lower = Lower(challenge);
  const std::string needle = Lower(key) + "=";
  size_t pos = lower.find(needle);
  while (pos != std::string::npos) {
    const bool boundary = pos == 0 || challenge[pos - 1] == ' ' || challenge[pos - 1] == ',' ||
                          challenge[pos - 1] == '\t';
    if (boundary) {
      size_t value_start = pos + needle.size();
      if (value_start < challenge.size() && challenge[value_start] == '"') {
        const size_t close = challenge.find('"', value_start + 1);
        if (close == std::string::npos)
          return {};
        return challenge.substr(value_start + 1, close - value_start - 1);
      }
      const size_t comma = challenge.find(',', value_start);
      return Trim(challenge.substr(value_start, comma == std::string::npos
                                                   ? std::string::npos
                                                   : comma - value_start));
    }
    pos = lower.find(needle, pos + 1);
  }
  return {};
}

std::string BuildRtspAuthorization(const std::string& challenge, const std::string& method,
                                   const std::string& uri, const std::string& username,
                                   const std::string& password) {
  const std::string lower = Lower(challenge);
  if (lower.find("digest") == std::string::npos) {
    if (lower.find("basic") == std::string::npos)
      return {};
    const std::string credentials = username + ":" + password;
    std::string encoded(4 * ((credentials.size() + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                        reinterpret_cast<const unsigned char*>(credentials.data()),
                                        static_cast<int>(credentials.size()));
    if (written <= 0)
      return {};
    encoded.resize(static_cast<size_t>(written));
    return "Basic " + encoded;
  }
  const std::string realm = AuthParam(challenge, "realm");
  const std::string nonce = AuthParam(challenge, "nonce");
  if (nonce.empty())
    return {};
  const std::string qop = AuthParam(challenge, "qop");
  const std::string opaque = AuthParam(challenge, "opaque");
  std::string algorithm = AuthParam(challenge, "algorithm");
  if (algorithm.empty())
    algorithm = "MD5";
  const std::string ha1 = Md5Hex(username + ":" + realm + ":" + password);
  const std::string ha2 = Md5Hex(method + ":" + uri);
  std::string header = "Digest username=\"" + username + "\", realm=\"" + realm + "\", nonce=\"" +
                       nonce + "\", uri=\"" + uri + "\", algorithm=" + algorithm;
  if (!qop.empty()) {
    const std::string count = "00000001";
    const std::string cnonce = RandomHex(8);
    const std::string response =
        Md5Hex(ha1 + ":" + nonce + ":" + count + ":" + cnonce + ":" + qop + ":" + ha2);
    header += ", response=\"" + response + "\", qop=" + qop + ", nc=" + count + ", cnonce=\"" +
              cnonce + "\"";
  } else {
    header += ", response=\"" + Md5Hex(ha1 + ":" + nonce + ":" + ha2) + "\"";
  }
  if (!opaque.empty())
    header += ", opaque=\"" + opaque + "\"";
  return header;
}

int PortFromRtspUri(const std::string& uri) {
  const size_t scheme = uri.find("://");
  if (scheme == std::string::npos)
    return 0;
  const size_t start = scheme + 3;
  const size_t path = uri.find('/', start);
  const std::string authority =
      uri.substr(start, path == std::string::npos ? std::string::npos : path - start);
  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos)
    return 554;
  const int port = std::atoi(authority.c_str() + colon + 1);
  return port > 0 && port <= 65535 ? port : 554;
}

std::vector<int> HttpPorts(int requested) {
  std::vector<int> ports;
  if (requested > 0)
    ports.push_back(requested);
  for (const int port : {80, 8080, 8000}) {
    if (std::find(ports.begin(), ports.end(), port) == ports.end())
      ports.push_back(port);
  }
  return ports;
}

struct AttemptRecorder {
  CameraDetectResult* result;
  void Add(const std::string& protocol, int port, bool ok, const std::string& error) {
    DetectAttempt attempt;
    attempt.protocol = protocol;
    attempt.port = port;
    attempt.ok = ok;
    attempt.error = error;
    result->tried.push_back(std::move(attempt));
  }
};

} // namespace

bool ProbeRtspOptions(const std::string& host, int port, const std::string& username,
                      const std::string& password, std::string* error) {
  Socket socket = kInvalidSocket;
  if (!OpenSocket(host, port, 2, &socket, error))
    return false;
  const std::string uri = "rtsp://" + host + ":" + std::to_string(port) + "/";
  const auto request = [&uri](int cseq, const std::string& authorization) {
    std::string text = "OPTIONS " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq) +
                       "\r\nUser-Agent: UvNVR\r\n";
    if (!authorization.empty())
      text += "Authorization: " + authorization + "\r\n";
    text += "\r\n";
    return text;
  };

  HeaderMap response;
  std::string io_error;
  if (!SendAll(socket, request(1, ""), &io_error) ||
      !ReadHeaderBlock(socket, &response, &io_error)) {
    CloseSocket(socket);
    if (error != nullptr)
      *error = io_error;
    return false;
  }
  if (response.status == 401 && !username.empty()) {
    const std::string challenge = HeaderValue(response, "WWW-Authenticate");
    const std::string authorization =
        BuildRtspAuthorization(challenge, "OPTIONS", uri, username, password);
    if (!authorization.empty()) {
      HeaderMap retried;
      if (SendAll(socket, request(2, authorization), &io_error) &&
          ReadHeaderBlock(socket, &retried, &io_error))
        response = std::move(retried);
    }
  }
  CloseSocket(socket);
  if (response.status >= 200 && response.status < 300)
    return true;
  if (response.status == 401) {
    if (error != nullptr)
      *error = username.empty() ? "the RTSP server requires authentication"
                                : "the RTSP server rejected the credentials";
    // A 401 still proves an RTSP service is listening.
    return true;
  }
  if (error != nullptr)
    *error = "the RTSP server returned " + std::to_string(response.status) +
             (response.status_text.empty() ? "" : " " + response.status_text);
  return false;
}

bool DetectCamera(const std::string& host, int port, const std::string& username,
                  const std::string& password, const std::string& protocol,
                  CameraDetectResult* result, std::string* error) {
  if (result == nullptr) {
    if (error != nullptr)
      *error = "missing detection output";
    return false;
  }
  result->tried.clear();
  const std::string host_value = Trim(host);
  if (host_value.empty()) {
    if (error != nullptr)
      *error = "host is required";
    return false;
  }
  std::string selected = Lower(Trim(protocol));
  if (selected.empty())
    selected = "onvif";
  if (selected == "auto")
    selected = "";

  AttemptRecorder attempts{result};

  const auto try_onvif = [&](int http_port) {
    const std::string xaddr =
        "http://" + host_value + ":" + std::to_string(http_port) + "/onvif/device_service";
    OnvifInfo info;
    std::string onvif_error;
    if (!ProbeOnvifIdentity(xaddr, username, password, &info, &onvif_error, 2)) {
      attempts.Add("onvif", http_port, false, onvif_error);
      return false;
    }
    attempts.Add("onvif", http_port, true, "");
    // GetProfiles/GetStreamUri are optional for identification: some cameras
    // answer GetDeviceInformation but have no usable media service.
    OnvifInfo media;
    if (ProbeOnvif(xaddr, username, password, &media, &onvif_error) && !media.profiles.empty()) {
      info.profiles = std::move(media.profiles);
      if (info.device_name.empty())
        info.device_name = media.device_name;
    }
    result->detected = "onvif";
    result->http_port = http_port;
    result->vendor = info.manufacturer;
    result->model = info.model;
    result->firmware = info.firmware;
    result->serial = info.serial;
    result->device_name = info.device_name;
    result->name_source = info.device_name.empty() ? "" : "onvif";
    result->stream_uris.clear();
    for (const auto& profile : info.profiles) {
      if (!profile.stream_uri.empty())
        result->stream_uris.push_back(profile.stream_uri);
    }
    result->rtsp_port = result->stream_uris.empty() ? 554
                                                    : PortFromRtspUri(result->stream_uris.front());
    return true;
  };

  // Hikvision keeps RTSP on 554 by default but plenty of units are configured
  // for 8000 or 10554; probe the common ports instead of assuming.
  const auto detect_rtsp_port = [&](const std::vector<int>& candidates) {
    for (const int candidate : candidates) {
      std::string probe_error;
      if (ProbeRtspOptions(host_value, candidate, username, password, &probe_error))
        return candidate;
    }
    return 554;
  };

  const auto try_hikvision = [&](int http_port) {
    BrandDeviceInfo info;
    std::string brand_error;
    if (!ProbeHikvisionDevice(host_value, http_port, username, password, &info, &brand_error)) {
      attempts.Add("hikvision", http_port, false, brand_error);
      return false;
    }
    attempts.Add("hikvision", http_port, true, "");
    result->detected = "hikvision";
    result->http_port = http_port;
    result->vendor = info.brand.empty() ? "Hikvision" : info.brand;
    result->model = info.model;
    result->firmware = info.firmware;
    result->serial = info.serial;
    result->device_name = info.name;
    result->name_source = info.name.empty() ? "" : "isapi";
    result->rtsp_port = detect_rtsp_port({554, 8000, 10554, 8554});
    result->stream_uris.clear();
    result->stream_uris.push_back("rtsp://" + host_value + ":" +
                                  std::to_string(result->rtsp_port) +
                                  "/Streaming/Channels/101");
    return true;
  };

  const auto try_dahua = [&](int http_port) {
    BrandDeviceInfo info;
    std::string brand_error;
    if (!ProbeDahuaDevice(host_value, http_port, username, password, &info, &brand_error)) {
      attempts.Add("dahua", http_port, false, brand_error);
      return false;
    }
    attempts.Add("dahua", http_port, true, "");
    result->detected = "dahua";
    result->http_port = http_port;
    result->vendor = info.brand.empty() ? "Dahua" : info.brand;
    result->model = info.model;
    result->firmware = info.firmware;
    result->serial = info.serial;
    result->device_name = info.name;
    result->name_source = info.name.empty() ? "" : "cgi";
    result->rtsp_port = 554;
    result->stream_uris.clear();
    return true;
  };

  if (selected == "rtsp") {
    const int rtsp_port = port > 0 ? port : 554;
    std::string rtsp_error;
    if (ProbeRtspOptions(host_value, rtsp_port, username, password, &rtsp_error)) {
      attempts.Add("rtsp", rtsp_port, true, "");
      result->detected = "rtsp";
      result->rtsp_port = rtsp_port;
      return true;
    }
    attempts.Add("rtsp", rtsp_port, false, rtsp_error);
    if (error != nullptr)
      *error = "no RTSP service answered";
    return false;
  }

  if (selected == "onvif" || selected == "hikvision" || selected == "dahua") {
    for (const int http_port : HttpPorts(port)) {
      const bool ok = selected == "onvif"    ? try_onvif(http_port)
                      : selected == "hikvision" ? try_hikvision(http_port)
                                                : try_dahua(http_port);
      if (ok)
        return true;
    }
    if (error != nullptr)
      *error = "no " + selected + " service answered on the probed ports";
    return false;
  }

  // auto: protocol order is ONVIF, Hikvision, Dahua, RTSP. Protocols are
  // tried per port so a camera on 8080 does not wait for three failed probes
  // on 80 first. A camera that answers ONVIF and also its vendor HTTP API is
  // reported as the vendor protocol, which gives the UI a device name and the
  // vendor RTSP path.
  for (const int http_port : HttpPorts(port)) {
    if (try_onvif(http_port)) {
      if (try_hikvision(http_port) || try_dahua(http_port))
        return true;
      return true;
    }
    if (try_hikvision(http_port) || try_dahua(http_port))
      return true;
  }

  const int rtsp_port = port > 0 ? port : 554;
  std::string rtsp_error;
  if (ProbeRtspOptions(host_value, rtsp_port, username, password, &rtsp_error)) {
    attempts.Add("rtsp", rtsp_port, true, "");
    result->detected = "rtsp";
    result->rtsp_port = rtsp_port;
    return true;
  }
  attempts.Add("rtsp", rtsp_port, false, rtsp_error);
  if (error != nullptr)
    *error = "no supported camera protocol answered";
  return false;
}

} // namespace uvnvr
