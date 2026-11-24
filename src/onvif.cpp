#include "onvif.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <string_view>
#include <uv.h>
#include <vector>
#ifdef _WIN32
#else
#include <fcntl.h>
#endif
#include <thread>

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

std::string Md5Hex(const std::string& input) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(input.data(), input.size(), digest, &length, EVP_md5(), nullptr) != 1)
    return {};
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

std::string RandomHex(size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1)
    return "0123456789abcdef";
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char byte : buffer) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::string HeaderValue(const std::string& headers, const std::string& name);

struct HttpRawResponse {
  int status = 0;
  std::string headers;
  std::string body;
};


// Connect with a bounded wait: a camera that is powered off or filtered must
// fail quickly instead of hanging on the platform SYN timeout.
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

bool HttpExchange(const std::string& url, const std::string& body, int timeout_seconds,
                  const std::string& extra_headers, HttpRawResponse* out, std::string* error) {
  if (url.rfind("http://", 0) != 0) {
    if (error)
      *error = "only http:// ONVIF endpoints are supported";
    return false;
  }
  if (!EnsureWinsock(error))
    return false;
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
    return false;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
    if (error)
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
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#endif
    if (ConnectWithTimeout(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen),
                           timeout_seconds))
      break;
    CloseSocket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(result);
  if (socket == kInvalidSocket) {
    if (error)
      *error = "cannot connect to " + host + ":" + port;
    return false;
  }

  const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + authority +
                              "\r\nContent-Type: application/soap+xml; charset=utf-8\r\n" +
                              extra_headers + "Content-Length: " + std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < request.size()) {
    const int n = static_cast<int>(
        ::send(socket, request.data() + sent, static_cast<int>(request.size() - sent), 0));
    if (n <= 0) {
      CloseSocket(socket);
      if (error)
        *error = "failed to send SOAP request";
      return false;
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
      *error = response.empty() ? "camera closed the connection without a response"
                                : "invalid HTTP response from camera";
    return false;
  }
  const std::string head = response.substr(0, body_start);
  out->headers = head;
  out->body = response.substr(body_start + 4);
  if (head.compare(0, 5, "HTTP/") == 0) {
    const size_t space = head.find(' ');
    if (space != std::string::npos)
      out->status = std::atoi(head.c_str() + space + 1);
  }
  if (std::getenv("UVNVR_ONVIF_TRACE") != nullptr) {
    std::fprintf(stderr, "[onvif] POST %s auth=%d -> %d %s\n", url.c_str(),
                 extra_headers.empty() ? 0 : 1, out->status,
                 HeaderValue(out->headers, "WWW-Authenticate").c_str());
    if (!extra_headers.empty())
      std::fprintf(stderr, "[onvif]   header: %s\n", extra_headers.c_str());
  }
  return true;
}

std::string LowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string HeaderValue(const std::string& headers, const std::string& name) {
  const std::string lower = LowerCopy(headers);
  const std::string key = LowerCopy(name) + ":";
  size_t pos = lower.find("\n" + key);
  if (pos == std::string::npos) {
    if (lower.compare(0, key.size(), key) != 0)
      return {};
    pos = 0;
  } else {
    pos += 1;
  }
  const size_t value_start = pos + key.size();
  const size_t line_end = headers.find("\r\n", value_start);
  std::string value = headers.substr(value_start, line_end == std::string::npos
                                                      ? std::string::npos
                                                      : line_end - value_start);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.erase(value.begin());
  return value;
}

std::string AuthParam(const std::string& challenge, const std::string& key) {
  const std::string lower = LowerCopy(challenge);
  const std::string needle = LowerCopy(key) + "=";
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
      std::string value = challenge.substr(value_start, comma == std::string::npos
                                                           ? std::string::npos
                                                           : comma - value_start);
      while (!value.empty() && value.back() == ' ')
        value.pop_back();
      return value;
    }
    pos = lower.find(needle, pos + 1);
  }
  return {};
}

// Builds the Authorization header for a camera that answered 401. Digest is
// what most ONVIF cameras require; Basic is kept for the few that only offer
// it. Returns an empty string when the challenge is not understood.
// Some cameras hand out one fixed nonce and reject a replayed nonce count, so
// the client has to count its own uses per (realm, nonce, user) the way a
// browser does. Keyed globally because the camera keeps the same nonce across
// every endpoint and connection.
std::mutex g_digest_nonce_mutex;
std::map<std::string, uint32_t> g_digest_nonce_counts;

uint32_t NextNonceCount(const std::string& realm, const std::string& nonce,
                        const std::string& username) {
  std::lock_guard<std::mutex> lock(g_digest_nonce_mutex);
  if (g_digest_nonce_counts.size() > 512)
    g_digest_nonce_counts.clear();
  uint32_t& count = g_digest_nonce_counts[realm + "|" + nonce + "|" + username];
  count += 1;
  return count;
}

std::string BuildAuthorization(const std::string& url, const std::string& headers,
                               const std::string& username, const std::string& password) {
  const std::string challenge = HeaderValue(headers, "WWW-Authenticate");
  if (challenge.empty())
    return {};
  const std::string lower = LowerCopy(challenge);
  if (lower.find("digest") == std::string::npos) {
    if (lower.find("basic") == std::string::npos)
      return {};
    const std::string credentials = username + ":" + password;
    return "Authorization: Basic " + Base64(reinterpret_cast<const unsigned char*>(
                                                credentials.data()), credentials.size());
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
  const size_t path_start = url.find('/', 7);
  const std::string uri = path_start == std::string::npos ? "/" : url.substr(path_start);
  const std::string ha1 = Md5Hex(username + ":" + realm + ":" + password);
  const std::string ha2 = Md5Hex("POST:" + uri);
  const std::string cnonce = RandomHex(8);
  char count_text[16];
  std::snprintf(count_text, sizeof(count_text), "%08x",
                NextNonceCount(realm, nonce, username));
  const std::string count = count_text;
  std::string response;
  std::string header = "Authorization: Digest ";
  if (!qop.empty()) {
    response = Md5Hex(ha1 + ":" + nonce + ":" + count + ":" + cnonce + ":" + qop + ":" + ha2);
  } else {
    response = Md5Hex(ha1 + ":" + nonce + ":" + ha2);
  }
  header += "username=\"" + username + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
            "\", uri=\"" + uri + "\", algorithm=" + algorithm + ", response=\"" + response + "\"";
  if (!qop.empty())
    header += ", qop=" + qop + ", nc=" + count + ", cnonce=\"" + cnonce + "\"";
  if (!opaque.empty())
    header += ", opaque=\"" + opaque + "\"";
  return header;
}

std::string HttpPostTimeout(const std::string& url, const std::string& body, int timeout_seconds,
                            const std::string& username, const std::string& password,
                            std::string* error) {
  HttpRawResponse response;
  if (!HttpExchange(url, body, timeout_seconds, "", &response, error))
    return {};
  if (response.status == 401 && !username.empty()) {
    const std::string authorization = BuildAuthorization(url, response.headers, username, password);
    if (!authorization.empty()) {
      HttpRawResponse retried;
      if (!HttpExchange(url, body, timeout_seconds, authorization + "\r\n", &retried, error))
        return {};
      response = std::move(retried);
      // A camera that rotates its nonce answers the first digest attempt with
      // a new challenge; honour it once instead of failing the whole call.
      if (response.status == 401) {
        const std::string second = BuildAuthorization(url, response.headers, username, password);
        if (!second.empty() && second != authorization) {
          HttpRawResponse retried_again;
          if (!HttpExchange(url, body, timeout_seconds, second + "\r\n", &retried_again, error))
            return {};
          response = std::move(retried_again);
        }
      }
    }
  }
  if (response.status >= 400) {
    if (error) {
      *error = "camera returned HTTP " + std::to_string(response.status);
      if (response.status == 401)
        *error += " (authentication rejected)";
    }
    return {};
  }
  return response.body;
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
         "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
         "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
         "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
         "xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\">"
         "<s:Header>" +
         security + "</s:Header><s:Body>" + body + "</s:Body></s:Envelope>";
}

std::string SoapCall(const std::string& xaddr, const std::string& security, const std::string& body,
                     const std::string& username, const std::string& password,
                     std::string* error, int timeout_seconds = 5) {
  // Cameras behind HTTP authentication reject a request that carries both the
  // Digest Authorization and a WS-Security token: the extra header makes the
  // camera answer 401/500 and consumes its nonce counter, so the retry that
  // follows fails as well. Try the plain envelope first and only fall back to
  // WS-Security when the device clearly does not accept HTTP credentials.
  if (!security.empty() && !username.empty()) {
    std::string plain_error;
    std::string plain = HttpPostTimeout(xaddr, Envelope("", body), timeout_seconds, username,
                                        password, &plain_error);
    if (!plain.empty()) {
      if (error)
        error->clear();
      return plain;
    }
    if (plain_error.find("cannot connect") != std::string::npos ||
        plain_error.find("cannot resolve") != std::string::npos) {
      if (error)
        *error = plain_error;
      return {};
    }
    const bool auth_rejected = plain_error.find("(authentication rejected)") != std::string::npos ||
                               plain_error.find("HTTP 401") != std::string::npos;
    if (!auth_rejected) {
      // The device answered but refused the operation itself (SOAP fault):
      // sending the same call with a WS-Security token will not change that.
      if (error)
        *error = plain_error;
      return {};
    }
  }
  return HttpPostTimeout(xaddr, Envelope(security, body), timeout_seconds, username, password,
                         error);
}

std::string DeviceServiceUrl(const std::string& xaddr) {
  if (xaddr.find("/onvif/device_service") != std::string::npos)
    return xaddr;
  std::string base = xaddr;
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  return base + "/onvif/device_service";
}

struct XmlBlock {
  std::string open_tag;
  std::string inner;
  std::string close_tag;
};

bool ExtractFirstBlock(const std::string& xml, const std::string& tag, XmlBlock* block) {
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' ||
        xml[pos + 1] == '!') {
      ++pos;
      continue;
    }
    size_t name_end = pos + 1;
    while (name_end < xml.size() &&
           (std::isalnum(static_cast<unsigned char>(xml[name_end])) || xml[name_end] == '_' ||
            xml[name_end] == '-' || xml[name_end] == ':' || xml[name_end] == '.')) {
      ++name_end;
    }
    std::string name = xml.substr(pos + 1, name_end - pos - 1);
    const size_t colon = name.rfind(':');
    if (colon != std::string::npos)
      name = name.substr(colon + 1);
    if (name != tag) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string::npos)
      return false;
    if (xml[open_end - 1] == '/') {
      pos = open_end + 1;
      continue;
    }
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
      if (close_name == tag) {
        block->open_tag = xml.substr(pos, open_end - pos + 1);
        block->inner = xml.substr(open_end + 1, scan - open_end - 1);
        block->close_tag = xml.substr(scan, end - scan);
        return true;
      }
      scan = end;
    }
    return false;
  }
  return false;
}

bool ReplaceTagValue(std::string* xml, const std::string& tag, const std::string& value) {
  size_t pos = 0;
  while ((pos = xml->find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml->size() || (*xml)[pos + 1] == '/' || (*xml)[pos + 1] == '?' ||
        (*xml)[pos + 1] == '!') {
      ++pos;
      continue;
    }
    size_t name_end = pos + 1;
    while (name_end < xml->size() &&
           (std::isalnum(static_cast<unsigned char>((*xml)[name_end])) || (*xml)[name_end] == '_' ||
            (*xml)[name_end] == '-' || (*xml)[name_end] == ':' || (*xml)[name_end] == '.')) {
      ++name_end;
    }
    std::string name = xml->substr(pos + 1, name_end - pos - 1);
    const size_t colon = name.rfind(':');
    if (colon != std::string::npos)
      name = name.substr(colon + 1);
    if (name != tag) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml->find('>', name_end);
    if (open_end == std::string::npos)
      return false;
    if ((*xml)[open_end - 1] == '/') {
      pos = open_end + 1;
      continue;
    }
    size_t scan = open_end + 1;
    while ((scan = xml->find("</", scan)) != std::string::npos) {
      size_t end = scan + 2;
      while (end < xml->size() &&
             (std::isalnum(static_cast<unsigned char>((*xml)[end])) || (*xml)[end] == '_' ||
              (*xml)[end] == '-' || (*xml)[end] == ':' || (*xml)[end] == '.')) {
        ++end;
      }
      std::string close_name = xml->substr(scan + 2, end - (scan + 2));
      const size_t close_colon = close_name.rfind(':');
      if (close_colon != std::string::npos)
        close_name = close_name.substr(close_colon + 1);
      if (close_name == tag) {
        xml->replace(open_end + 1, scan - open_end - 1, value);
        return true;
      }
      scan = end;
    }
    return false;
  }
  return false;
}

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::string AttributeValue(const std::string& tag, const std::string& name) {
  const std::string needle = name + "=";
  size_t pos = tag.find(needle);
  while (pos != std::string::npos) {
    if (pos == 0 || std::isspace(static_cast<unsigned char>(tag[pos - 1])) || tag[pos - 1] == '<') {
      break;
    }
    pos = tag.find(needle, pos + needle.size());
  }
  if (pos == std::string::npos)
    return {};
  pos += needle.size();
  if (pos >= tag.size())
    return {};
  const char quote = tag[pos];
  if (quote != '"' && quote != '\'')
    return {};
  const size_t end = tag.find(quote, pos + 1);
  if (end == std::string::npos)
    return {};
  return tag.substr(pos + 1, end - pos - 1);
}

// Finds every `<prefix:tag ...>...</prefix:tag>` block, ignoring prefixes.
std::vector<std::string> ExtractBlocks(const std::string& xml, const std::string& tag) {
  std::vector<std::string> blocks;
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' ||
        xml[pos + 1] == '!') {
      ++pos;
      continue;
    }
    size_t name_end = pos + 1;
    while (name_end < xml.size() &&
           (std::isalnum(static_cast<unsigned char>(xml[name_end])) || xml[name_end] == '_' ||
            xml[name_end] == '-' || xml[name_end] == ':' || xml[name_end] == '.')) {
      ++name_end;
    }
    std::string name = xml.substr(pos + 1, name_end - pos - 1);
    const size_t colon = name.rfind(':');
    if (colon != std::string::npos)
      name = name.substr(colon + 1);
    if (name != tag) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string::npos)
      break;
    if (open_end > 0 && xml[open_end - 1] == '/') {
      pos = open_end + 1;
      continue;
    }
    size_t scan = open_end + 1;
    size_t close_start = std::string::npos;
    size_t close_end = std::string::npos;
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
      if (close_name == tag) {
        close_start = scan;
        close_end = end;
        break;
      }
      scan = end;
    }
    if (close_start == std::string::npos)
      break;
    blocks.push_back(xml.substr(open_end + 1, close_start - open_end - 1));
    pos = close_end;
  }
  return blocks;
}

std::vector<OnvifEvent> ParseNotificationEvents(const std::string& xml) {
  std::vector<OnvifEvent> events;
  for (const std::string& block : ExtractBlocks(xml, "NotificationMessage")) {
    const std::string topic = Trim(ExtractTag(block, "Topic"));
    bool found = false;
    size_t pos = 0;
    while ((pos = block.find("SimpleItem", pos)) != std::string::npos) {
      const size_t tag_start = block.rfind('<', pos);
      const size_t tag_end = block.find('>', pos);
      if (tag_start == std::string::npos || tag_end == std::string::npos)
        break;
      const std::string tag = block.substr(tag_start, tag_end - tag_start);
      const std::string name = AttributeValue(tag, "Name");
      const std::string value = AttributeValue(tag, "Value");
      if (!name.empty() || !value.empty()) {
        events.push_back(OnvifEvent{topic, name, value});
        found = true;
      }
      pos = tag_end + 1;
    }
    if (!found)
      events.push_back(OnvifEvent{topic, "", ""});
  }
  return events;
}

// Defined below with the rest of the scope helpers.
std::string UrlDecode(std::string value);

struct LocalIpv4Address {
  in_addr address{};
};

// One entry per non-loopback IPv4 interface. Discovery sends a Probe out of
// each of them: the default multicast route only reaches the interface the OS
// picked, which hides cameras on a second NIC.
std::vector<LocalIpv4Address> LocalIpv4Addresses() {
  std::vector<LocalIpv4Address> addresses;
  uv_interface_address_t* entries = nullptr;
  int count = 0;
  if (uv_interface_addresses(&entries, &count) != 0)
    return addresses;
  for (int i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    if (entry.address.address4.sin_family != AF_INET || entry.is_internal)
      continue;
    const unsigned long host = ntohl(entry.address.address4.sin_addr.s_addr);
    if (host == 0 || (host >> 24) == 127)
      continue;
    LocalIpv4Address local;
    local.address = entry.address.address4.sin_addr;
    const bool duplicate =
        std::any_of(addresses.begin(), addresses.end(), [&](const LocalIpv4Address& existing) {
          return existing.address.s_addr == local.address.s_addr;
        });
    if (!duplicate)
      addresses.push_back(local);
  }
  uv_free_interface_addresses(entries, count);
  return addresses;
}

std::string ScopeTokenValue(const std::string& token, const std::string& key) {
  // Standard scope form: onvif://www.onvif.org/<key>/<value>.
  const std::string path = "onvif://www.onvif.org/" + key + "/";
  size_t pos = token.find(path);
  if (pos != std::string::npos)
    return UrlDecode(token.substr(pos + path.size()));
  // Some devices publish key=value pairs instead of the path form.
  const std::string query = key + "=";
  pos = token.find(query);
  if (pos != std::string::npos) {
    size_t end = pos + query.size();
    while (end < token.size() && token[end] != '&' && token[end] != ';')
      ++end;
    return UrlDecode(token.substr(pos + query.size(), end - pos - query.size()));
  }
  return {};
}

std::pair<std::string, std::string> ScopeIdentity(const std::string& scopes) {
  std::string name;
  std::string hardware;
  size_t start = 0;
  while (start < scopes.size()) {
    const size_t end = scopes.find_first_of(" \t\r\n", start);
    const std::string token =
        scopes.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!token.empty()) {
      if (name.empty())
        name = ScopeTokenValue(token, "name");
      if (hardware.empty())
        hardware = ScopeTokenValue(token, "hardware");
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return {name, hardware};
}

} // namespace

bool DiscoverOnvif(std::vector<OnvifDevice>* devices, int timeout_ms, std::string* error,
                   int* probes_sent, int* responses) {
  if (devices == nullptr) {
    if (error)
      *error = "missing ONVIF discovery output";
    return false;
  }
  devices->clear();
  if (probes_sent)
    *probes_sent = 0;
  if (responses)
    *responses = 0;
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
  const std::vector<LocalIpv4Address> interfaces = LocalIpv4Addresses();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  auto next_probe = std::chrono::steady_clock::now();
  int sent = 0;
  int received = 0;
  int rounds = 0;
  std::array<char, 8192> buffer{};

  // Keep collecting for the whole window and retry the Probe twice so a lost
  // multicast datagram does not hide a device. Each round is sent out of every
  // IPv4 interface; the OS default route is used when none is found.
  while (std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (rounds < 3 && now >= next_probe) {
      int round_sent = 0;
      if (interfaces.empty()) {
        if (::sendto(socket, message.data(), static_cast<int>(message.size()), 0,
                     reinterpret_cast<const sockaddr*>(&target), sizeof(target)) >= 0)
          ++round_sent;
      } else {
        for (const auto& iface : interfaces) {
          setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                     reinterpret_cast<const char*>(&iface.address),
                     sizeof(iface.address));
          if (::sendto(socket, message.data(), static_cast<int>(message.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target)) >= 0)
            ++round_sent;
        }
      }
      if (round_sent == 0 && rounds == 0) {
        CloseSocket(socket);
        if (error)
          *error = "cannot send the ONVIF discovery probe";
        return false;
      }
      sent += round_sent;
      ++rounds;
      next_probe = now + std::chrono::milliseconds(200);
    }

    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
    if (remaining_ms <= 0)
      break;
    const int slice_ms = static_cast<int>(std::min<int64_t>(remaining_ms, 50));
#ifdef SO_RCVTIMEO
#ifdef _WIN32
    // Winsock's SO_RCVTIMEO expects a DWORD in milliseconds. Passing a
    // struct timeval made a 3000 ms window expire after about 3 ms.
    DWORD recv_timeout = static_cast<DWORD>(slice_ms);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout),
               sizeof(recv_timeout));
#else
    struct timeval tv {};
    tv.tv_sec = slice_ms / 1000;
    tv.tv_usec = (slice_ms % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
#endif

    sockaddr_in from{};
#ifdef _WIN32
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = static_cast<int>(::recvfrom(socket, buffer.data(), buffer.size(), 0,
                                              reinterpret_cast<sockaddr*>(&from), &from_len));
    if (n == 0)
      continue;
    if (n < 0) {
#ifdef _WIN32
      const int code = WSAGetLastError();
      if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK || code == WSAEINTR)
        continue;
      if (code == WSAECONNRESET) {
        // ICMP port-unreachable can surface as a reset on a UDP socket. Keep
        // the socket alive and continue collecting replies from other devices.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
#endif
      break;
    }
    ++received;
    const std::string response(buffer.data(), static_cast<size_t>(n));
    std::string xaddrs = ExtractTag(response, "XAddrs");
    if (xaddrs.empty())
      xaddrs = ExtractTag(response, "XAddr");
    const std::string scopes = Trim(ExtractTag(response, "Scopes"));
    const auto [scope_name, scope_hardware] = ScopeIdentity(scopes);
    std::string endpoint;
    for (const std::string& block : ExtractBlocks(response, "EndpointReference")) {
      endpoint = Trim(ExtractTag(block, "Address"));
      if (!endpoint.empty())
        break;
    }

    size_t start = 0;
    while (start < xaddrs.size()) {
      const size_t end = xaddrs.find_first_of(" \t\r\n", start);
      const std::string address =
          xaddrs.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (address.rfind("http://", 0) == 0 || address.rfind("https://", 0) == 0) {
        OnvifDevice device;
        device.xaddr = address;
        device.endpoint = endpoint;
        device.scopes = scopes;
        device.name = scope_name;
        device.hardware = scope_hardware;
        const size_t host_start = address.find("://") + 3;
        const size_t slash = address.find('/', host_start);
        std::string authority =
            address.substr(host_start, slash == std::string::npos ? std::string::npos
                                                                  : slash - host_start);
        const size_t at = authority.rfind('@');
        if (at != std::string::npos)
          authority = authority.substr(at + 1);
        if (!authority.empty() && authority.front() == '[') {
          const size_t close = authority.find(']');
          device.ip = authority.substr(1, close == std::string::npos ? std::string::npos
                                                                     : close - 1);
        } else {
          const size_t colon = authority.find(':');
          device.ip =
              authority.substr(0, colon == std::string::npos ? std::string::npos : colon);
        }
        if (device.ip.empty())
          continue;
        const auto existing =
            std::find_if(devices->begin(), devices->end(),
                         [&](const OnvifDevice& entry) { return entry.ip == device.ip; });
        if (existing == devices->end()) {
          devices->push_back(device);
        } else {
          // A retransmitted ProbeMatch or a second XAddr for the same host can
          // carry the same data; keep the first non-empty value of each field.
          if (existing->xaddr.empty())
            existing->xaddr = device.xaddr;
          if (existing->endpoint.empty())
            existing->endpoint = device.endpoint;
          if (existing->scopes.empty())
            existing->scopes = device.scopes;
          if (existing->name.empty())
            existing->name = device.name;
          if (existing->hardware.empty())
            existing->hardware = device.hardware;
        }
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
  }
  if (probes_sent)
    *probes_sent = sent;
  if (responses)
    *responses = received;
  CloseSocket(socket);
  return sent > 0;
}

std::string ResolveMediaService(const std::string& device, const std::string& security,
                                const std::string& username, const std::string& password,
                                std::string* error);

namespace {

std::string UrlDecode(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int high = hex(value[i + 1]);
      const int low = hex(value[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i] == '+' ? ' ' : value[i]);
  }
  return out;
}

// GetScopes returns one ScopeItem per scope. The device name is published as
// onvif://www.onvif.org/name/<value>; hardware/location carry related values.
// Return the first non-empty name scope.
std::string ScopeValue(const std::string& xml, const std::string& key) {
  const std::string prefix = "onvif://www.onvif.org/" + key + "/";
  size_t pos = 0;
  while ((pos = xml.find(prefix, pos)) != std::string::npos) {
    size_t end = pos + prefix.size();
    while (end < xml.size() && xml[end] != '<' && xml[end] != '&' && xml[end] != '"' &&
           !std::isspace(static_cast<unsigned char>(xml[end])))
      ++end;
    const std::string value = UrlDecode(xml.substr(pos + prefix.size(),
                                                  end - pos - prefix.size()));
    if (!value.empty())
      return value;
    pos = end;
  }
  return {};
}

// Fills identity fields (manufacturer/model/firmware/serial/name) through the
// device service. Returns false only when the device service did not answer.
bool ReadOnvifIdentity(const std::string& service, const std::string& security,
                       const std::string& username, const std::string& password, int timeout_seconds,
                       OnvifInfo* info, std::string* error) {
  const std::string device_xml =
      SoapCall(service, security,
               "<GetDeviceInformation xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>", username,
               password, error, timeout_seconds);
  if (device_xml.empty())
    return false;
  info->manufacturer = ExtractTag(device_xml, "Manufacturer");
  info->model = ExtractTag(device_xml, "Model");
  info->firmware = ExtractTag(device_xml, "FirmwareVersion");
  info->serial = ExtractTag(device_xml, "SerialNumber");

  std::string scope_error;
  const std::string scopes_xml =
      SoapCall(service, security,
               "<GetScopes xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>", username, password,
               &scope_error, timeout_seconds);
  if (!scopes_xml.empty()) {
    info->device_name = ScopeValue(scopes_xml, "name");
    if (info->device_name.empty())
      info->device_name = ScopeValue(scopes_xml, "hardware");
  }
  return true;
}

} // namespace

bool ProbeOnvifIdentity(const std::string& xaddr, const std::string& username,
                        const std::string& password, OnvifInfo* info, std::string* error,
                        int timeout_seconds) {
  if (info == nullptr) {
    if (error != nullptr)
      *error = "missing ONVIF output";
    return false;
  }
  if (!EnsureWinsock(error))
    return false;
  const std::string service = DeviceServiceUrl(xaddr);
  if (!ReadOnvifIdentity(service, SecurityHeader(username, password), username, password,
                         timeout_seconds, info, error))
    return false;
  if (info->device_name.empty())
    info->device_name = info->model;
  return true;
}

bool ProbeOnvif(const std::string& xaddr, const std::string& username, const std::string& password,
                OnvifInfo* info, std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  const std::string service = DeviceServiceUrl(xaddr);
  const std::string security = SecurityHeader(username, password);

  if (!ReadOnvifIdentity(service, security, username, password, 5, info, error))
    return false;
  if (info->device_name.empty())
    info->device_name = info->model;

  // Some devices expose the media service on its own path, and answering
  // GetProfiles on the device endpoint returns a SOAP fault. Resolve the
  // media XAddr first and fall back to the device service.
  std::string media_error;
  const std::string media_service =
      ResolveMediaService(service, security, username, password, &media_error);
  const std::string profiles_xml = SoapCall(
      media_service, security, "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>",
      username, password, error);
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
    const std::string stream_xml = SoapCall(media_service, security, stream_body, username,
                                            password, error);
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

std::string ResolveMediaService(const std::string& device, const std::string& security,
                                const std::string& username, const std::string& password,
                                std::string* error) {
  const std::string capabilities = SoapCall(
      device, security,
      "<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>", username,
      password, error);
  if (!capabilities.empty()) {
    const std::string media = ExtractTag(capabilities, "Media");
    const std::string xaddr = Trim(ExtractTag(media, "XAddr"));
    if (!xaddr.empty())
      return xaddr;
  }
  return device;
}

bool GetOnvifEncoderConfig(const std::string& host, int port, const std::string& username,
                           const std::string& password, OnvifEncoderConfig* config,
                           std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  const std::string device =
      "http://" + host + ":" + std::to_string(port) + "/onvif/device_service";
  const std::string security = SecurityHeader(username, password);
  const std::string media = ResolveMediaService(device, security, username, password, error);
  const std::string xml = SoapCall(media, security, "<trt:GetVideoEncoderConfigurations/>", username, password, error);
  if (xml.empty())
    return false;
  XmlBlock block;
  if (!ExtractFirstBlock(xml, "Configurations", &block)) {
    if (error)
      *error = "the camera returned no video encoder configuration";
    return false;
  }
  config->token = AttributeValue(block.open_tag, "token");
  config->encoding = Trim(ExtractTag(block.inner, "Encoding"));
  config->width = std::atoi(Trim(ExtractTag(block.inner, "Width")).c_str());
  config->height = std::atoi(Trim(ExtractTag(block.inner, "Height")).c_str());
  config->fps = std::atoi(Trim(ExtractTag(block.inner, "FrameRateLimit")).c_str());
  config->bitrate_kbps = std::atoi(Trim(ExtractTag(block.inner, "BitrateLimit")).c_str());
  config->quality = std::atoi(Trim(ExtractTag(block.inner, "Quality")).c_str());
  if (config->token.empty()) {
    if (error)
      *error = "the camera encoder configuration has no token";
    return false;
  }
  return true;
}

bool SetOnvifEncoderConfig(const std::string& host, int port, const std::string& username,
                           const std::string& password, const OnvifEncoderConfig& config,
                           std::string* error) {
  if (!EnsureWinsock(error))
    return false;
  const std::string device =
      "http://" + host + ":" + std::to_string(port) + "/onvif/device_service";
  const std::string security = SecurityHeader(username, password);
  const std::string media = ResolveMediaService(device, security, username, password, error);
  const std::string xml = SoapCall(media, security, "<trt:GetVideoEncoderConfigurations/>", username, password, error);
  if (xml.empty())
    return false;
  XmlBlock block;
  if (!ExtractFirstBlock(xml, "Configurations", &block)) {
    if (error)
      *error = "the camera returned no video encoder configuration";
    return false;
  }
  const std::string token =
      config.token.empty() ? AttributeValue(block.open_tag, "token") : config.token;
  if (token.empty()) {
    if (error)
      *error = "the camera encoder configuration has no token";
    return false;
  }
  std::string inner = block.inner;
  if (!config.encoding.empty() && !ReplaceTagValue(&inner, "Encoding", config.encoding)) {
    if (error)
      *error = "the camera does not expose the Encoding field";
    return false;
  }
  if (config.width > 0 && !ReplaceTagValue(&inner, "Width", std::to_string(config.width))) {
    if (error)
      *error = "the camera does not expose the Width field";
    return false;
  }
  if (config.height > 0 && !ReplaceTagValue(&inner, "Height", std::to_string(config.height))) {
    if (error)
      *error = "the camera does not expose the Height field";
    return false;
  }
  if (config.fps > 0 && !ReplaceTagValue(&inner, "FrameRateLimit", std::to_string(config.fps))) {
    if (error)
      *error = "the camera does not expose the FrameRateLimit field";
    return false;
  }
  if (config.bitrate_kbps > 0 &&
      !ReplaceTagValue(&inner, "BitrateLimit", std::to_string(config.bitrate_kbps))) {
    if (error)
      *error = "the camera does not expose the BitrateLimit field";
    return false;
  }
  if (config.quality > 0 && !ReplaceTagValue(&inner, "Quality", std::to_string(config.quality))) {
    if (error)
      *error = "the camera does not expose the Quality field";
    return false;
  }
  const std::string body = "<trt:SetVideoEncoderConfiguration><trt:Configuration token=\"" +
                           XmlEscape(token) + "\">" + inner +
                           "</trt:Configuration><trt:ForcePersistence>true</trt:ForcePersistence>"
                           "</trt:SetVideoEncoderConfiguration>";
  const std::string response = SoapCall(media, security, body, username, password, error);
  return !response.empty();
}

bool ResolveOnvifEventUrl(const std::string& host, int port, const std::string& username,
                          const std::string& password, std::string* url, std::string* error) {
  if (url == nullptr) {
    if (error != nullptr)
      *error = "missing event URL output";
    return false;
  }
  const std::string fallback = "http://" + host + ":" + std::to_string(port) + "/onvif/events";
  const std::string device =
      "http://" + host + ":" + std::to_string(port) + "/onvif/device_service";
  std::string soap_error;
  const std::string response =
      SoapCall(device, SecurityHeader(username, password), username, password,
               "<tds:GetCapabilities><tds:Category>Events</tds:Category></tds:GetCapabilities>",
               &soap_error);
  if (!response.empty()) {
    const std::string events = ExtractTag(response, "Events");
    const std::string xaddr = Trim(ExtractTag(events, "XAddr"));
    if (!xaddr.empty()) {
      *url = xaddr;
      return true;
    }
  }
  // Many cameras expose the event service at the conventional path even when
  // GetCapabilities is not implemented or requires different credentials.
  *url = fallback;
  if (error != nullptr)
    error->clear();
  return true;
}

bool CreateOnvifEventSubscription(const std::string& event_url, const std::string& username,
                                  const std::string& password, OnvifEventSubscription* subscription,
                                  std::string* error) {
  if (subscription == nullptr) {
    if (error != nullptr)
      *error = "missing subscription output";
    return false;
  }
  const std::string body = "<tev:CreatePullPointSubscription>"
                           "<tev:InitialTerminationTime>PT120S</tev:InitialTerminationTime>"
                           "</tev:CreatePullPointSubscription>";
  const std::string response = SoapCall(event_url, SecurityHeader(username, password), body, username, password, error);
  if (response.empty())
    return false;
  const std::string reference = ExtractTag(response, "SubscriptionReference");
  std::string address = Trim(ExtractTag(reference, "Address"));
  if (address.empty())
    address = Trim(ExtractTag(response, "Address"));
  if (address.empty()) {
    if (error != nullptr)
      *error = "the camera did not return a subscription address";
    return false;
  }
  subscription->address = address;
  return true;
}

bool PullOnvifEvents(const std::string& address, const std::string& username,
                     const std::string& password, int timeout_ms, std::vector<OnvifEvent>* events,
                     std::string* error) {
  if (events == nullptr) {
    if (error != nullptr)
      *error = "missing event output";
    return false;
  }
  const int seconds = std::max(1, timeout_ms / 1000);
  const std::string body =
      "<tev:PullMessages><tev:Timeout>PT" + std::to_string(seconds) +
      "S</tev:Timeout><tev:MessageLimit>32</tev:MessageLimit></tev:PullMessages>";
  const std::string response = HttpPostTimeout(
      address, Envelope(SecurityHeader(username, password), body), seconds + 5, username, password,
      error);
  if (response.empty())
    return false;
  *events = ParseNotificationEvents(response);
  return true;
}

bool SetOnvifTime(const std::string& host, int port, const std::string& username,
                  const std::string& password, int64_t epoch_ms, std::string* error) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  char body[512];
  std::snprintf(body, sizeof(body),
                "<tds:SetSystemDateAndTime>"
                "<tds:DateTimeType>Manual</tds:DateTimeType>"
                "<tds:DaylightSavings>false</tds:DaylightSavings>"
                "<tds:UTCDateTime>"
                "<tt:Time><tt:Hour>%02d</tt:Hour><tt:Minute>%02d</tt:Minute>"
                "<tt:Second>%02d</tt:Second></tt:Time>"
                "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month>"
                "<tt:Day>%d</tt:Day></tt:Date>"
                "</tds:UTCDateTime>"
                "</tds:SetSystemDateAndTime>",
                tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  const std::string service =
      "http://" + host + ":" + std::to_string(port > 0 ? port : 80) + "/onvif/device_service";
  const std::string response = SoapCall(service, SecurityHeader(username, password), body, username, password, error);
  if (response.empty())
    return false;
  if (response.find("Fault") != std::string::npos ||
      response.find("faultstring") != std::string::npos) {
    const std::string reason = ExtractTag(response, "Text");
    if (error != nullptr)
      *error = reason.empty() ? "the camera rejected the time update" : reason;
    return false;
  }
  return true;
}

} // namespace uvnvr
