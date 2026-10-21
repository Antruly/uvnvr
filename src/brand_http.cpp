#include "brand_http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
bool InitSockets(std::string* error) {
  static bool ready = false;
  if (ready)
    return true;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    if (error != nullptr)
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

std::string HexLower(const unsigned char* data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0f]);
  }
  return out;
}

std::string Md5Hex(const std::string& value) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_Digest(value.data(), value.size(), digest, &size, EVP_md5(), nullptr) != 1)
    return {};
  return HexLower(digest, size);
}

std::string Base64(const std::string& value) {
  std::string out(4 * ((value.size() + 2) / 3), '\0');
  const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                      reinterpret_cast<const unsigned char*>(value.data()),
                                      static_cast<int>(value.size()));
  if (written <= 0)
    return {};
  out.resize(static_cast<size_t>(written));
  return out;
}

std::string RandomHex(size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1)
    return "0123456789abcdef";
  return HexLower(buffer.data(), buffer.size());
}

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

std::string ExtractTag(const std::string& xml, const std::string& tag) {
  const std::string open = "<" + tag;
  size_t pos = xml.find(open);
  if (pos == std::string::npos)
    return {};
  const size_t open_end = xml.find('>', pos);
  if (open_end == std::string::npos)
    return {};
  const size_t close = xml.find("</" + tag, open_end);
  if (close == std::string::npos)
    return {};
  return Trim(xml.substr(open_end + 1, close - open_end - 1));
}

std::map<std::string, std::string> ParseDigestParams(const std::string& header) {
  std::map<std::string, std::string> params;
  const size_t start = Lower(header).find("digest ");
  if (start == std::string::npos)
    return params;
  size_t pos = start + 7;
  while (pos < header.size()) {
    while (pos < header.size() &&
           (header[pos] == ',' || std::isspace(static_cast<unsigned char>(header[pos]))))
      ++pos;
    const size_t equals = header.find('=', pos);
    if (equals == std::string::npos)
      break;
    const std::string key = Lower(Trim(header.substr(pos, equals - pos)));
    pos = equals + 1;
    std::string value;
    if (pos < header.size() && header[pos] == '"') {
      ++pos;
      while (pos < header.size() && header[pos] != '"') {
        if (header[pos] == '\\' && pos + 1 < header.size())
          ++pos;
        value.push_back(header[pos++]);
      }
      if (pos < header.size())
        ++pos;
    } else {
      const size_t comma = header.find(',', pos);
      value =
          Trim(header.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
      pos = comma == std::string::npos ? header.size() : comma;
    }
    params[key] = value;
  }
  return params;
}

bool BuildAuthorization(const std::string& challenge, const std::string& method,
                        const std::string& uri, const std::string& username,
                        const std::string& password, std::string* authorization,
                        std::string* error) {
  const std::string lowered = Lower(challenge);
  if (lowered.rfind("basic", 0) == 0) {
    *authorization = "Basic " + Base64(username + ":" + password);
    return true;
  }
  if (lowered.find("digest") == std::string::npos) {
    if (error != nullptr)
      *error = "unsupported camera authentication scheme";
    return false;
  }
  const auto params = ParseDigestParams(challenge);
  const auto get = [&params](const char* key) {
    const auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  };
  const std::string realm = get("realm");
  const std::string nonce = get("nonce");
  const std::string opaque = get("opaque");
  const std::string qop = get("qop");
  std::string algorithm = get("algorithm");
  if (algorithm.empty())
    algorithm = "MD5";
  if (nonce.empty()) {
    if (error != nullptr)
      *error = "camera sent an empty digest nonce";
    return false;
  }
  const std::string cnonce = RandomHex(8);
  const std::string nc = "00000001";
  std::string ha1 = Md5Hex(username + ":" + realm + ":" + password);
  if (Lower(algorithm) == "md5-sess")
    ha1 = Md5Hex(ha1 + ":" + nonce + ":" + cnonce);
  const std::string ha2 = Md5Hex(method + ":" + uri);
  const bool has_qop = Lower(qop).find("auth") != std::string::npos;
  const std::string response =
      has_qop ? Md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2)
              : Md5Hex(ha1 + ":" + nonce + ":" + ha2);
  if (response.empty()) {
    if (error != nullptr)
      *error = "cannot compute the digest response";
    return false;
  }
  std::ostringstream out;
  out << "Digest username=\"" << username << "\", realm=\"" << realm << "\", nonce=\"" << nonce
      << "\", uri=\"" << uri << "\", response=\"" << response << "\", algorithm=" << algorithm;
  if (has_qop)
    out << ", qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\"";
  if (!opaque.empty())
    out << ", opaque=\"" << opaque << "\"";
  *authorization = out.str();
  return true;
}

struct HttpResponse {
  int status = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

bool SendRaw(const std::string& host, int port, const std::string& request, HttpResponse* response,
             std::string* error) {
  if (!InitSockets(error))
    return false;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port > 0 ? port : 80);
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
    if (error != nullptr)
      *error = "cannot connect to " + host + ":" + port_text;
    return false;
  }
  size_t sent = 0;
  while (sent < request.size()) {
    const int n = static_cast<int>(
        ::send(socket, request.data() + sent, static_cast<int>(request.size() - sent), 0));
    if (n <= 0) {
      CloseSocket(socket);
      if (error != nullptr)
        *error = "camera rejected the HTTP request";
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  std::string raw;
  std::array<char, 4096> buffer{};
  while (true) {
    const int n = static_cast<int>(::recv(socket, buffer.data(), buffer.size(), 0));
    if (n <= 0)
      break;
    raw.append(buffer.data(), static_cast<size_t>(n));
    if (raw.size() > 2 * 1024 * 1024)
      break;
  }
  CloseSocket(socket);
  const size_t body_start = raw.find("\r\n\r\n");
  if (body_start == std::string::npos) {
    if (error != nullptr)
      *error = "invalid HTTP response from camera";
    return false;
  }
  const std::string head = raw.substr(0, body_start);
  response->body = raw.substr(body_start + 4);
  std::istringstream lines(head);
  std::string line;
  if (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream status(line);
    std::string version;
    status >> version >> response->status;
  }
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    response->headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
  }
  return true;
}

bool HttpRequest(const std::string& host, int port, const std::string& method,
                 const std::string& path, const std::string& body, const std::string& content_type,
                 const std::string& username, const std::string& password, HttpResponse* response,
                 std::string* error) {
  const auto build = [&](const std::string& authorization) {
    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << (port > 0 ? port : 80) << "\r\n"
            << "Connection: close\r\n";
    if (!body.empty())
      request << "Content-Type: " << content_type << "\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
    if (!authorization.empty())
      request << "Authorization: " << authorization << "\r\n";
    request << "\r\n" << body;
    return request.str();
  };
  if (!SendRaw(host, port, build(""), response, error))
    return false;
  if (response->status != 401 || username.empty())
    return true;
  const auto challenge = response->headers.find("www-authenticate");
  if (challenge == response->headers.end()) {
    if (error != nullptr)
      *error = "camera requires authentication but sent no challenge";
    return false;
  }
  std::string authorization;
  if (!BuildAuthorization(challenge->second, method, path, username, password, &authorization,
                          error))
    return false;
  return SendRaw(host, port, build(authorization), response, error);
}

std::string LocalIsoTime(int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm local{};
  std::tm utc{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
  gmtime_s(&utc, &seconds);
#else
  localtime_r(&seconds, &local);
  gmtime_r(&seconds, &utc);
#endif
  char date[32];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local);
  int offset_minutes = (local.tm_hour * 60 + local.tm_min) - (utc.tm_hour * 60 + utc.tm_min);
  if (offset_minutes > 12 * 60)
    offset_minutes -= 24 * 60;
  if (offset_minutes < -12 * 60)
    offset_minutes += 24 * 60;
  const char sign = offset_minutes < 0 ? '-' : '+';
  const int absolute = std::abs(offset_minutes);
  char offset[16];
  std::snprintf(offset, sizeof(offset), "%c%02d:%02d", sign, absolute / 60, absolute % 60);
  return std::string(date) + offset;
}

std::string LocalPlainTime(int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &tm);
  return text;
}

std::string PercentEncodeSpaces(const std::string& value) {
  std::string out;
  for (const char c : value) {
    if (c == ' ')
      out += "%20";
    else
      out.push_back(c);
  }
  return out;
}

bool ProbeHikvision(const std::string& host, int port, const std::string& username,
                    const std::string& password, BrandDeviceInfo* info, std::string* error) {
  HttpResponse response;
  if (!HttpRequest(host, port, "GET", "/ISAPI/System/deviceInfo", "", "", username, password,
                   &response, error))
    return false;
  if (response.status != 200 || (response.body.find("DeviceInfo") == std::string::npos &&
                                 response.body.find("deviceName") == std::string::npos)) {
    return false;
  }
  info->protocol = "hikvision";
  info->brand = "Hikvision";
  info->name = ExtractTag(response.body, "deviceName");
  info->model = ExtractTag(response.body, "model");
  info->serial = ExtractTag(response.body, "serialNumber");
  info->firmware = ExtractTag(response.body, "firmwareVersion");
  if (info->name.empty())
    info->name = info->model;
  return true;
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
  std::map<std::string, std::string> values;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
  }
  return values;
}

bool ProbeDahua(const std::string& host, int port, const std::string& username,
                const std::string& password, BrandDeviceInfo* info, std::string* error) {
  HttpResponse response;
  if (!HttpRequest(host, port, "GET", "/cgi-bin/magicBox.cgi?action=getDeviceType", "", "",
                   username, password, &response, error))
    return false;
  if (response.status != 200)
    return false;
  const auto values = ParseKeyValues(response.body);
  const auto type = values.find("type");
  if (type == values.end() && response.body.find("type=") == std::string::npos)
    return false;
  info->protocol = "dahua";
  info->brand = "Dahua";
  info->model = type == values.end() ? "" : type->second;
  info->name = info->model;
  HttpResponse name_response;
  if (HttpRequest(host, port, "GET", "/cgi-bin/magicBox.cgi?action=getMachineName", "", "",
                  username, password, &name_response, error) &&
      name_response.status == 200) {
    const auto name_values = ParseKeyValues(name_response.body);
    const auto name = name_values.find("name");
    if (name != name_values.end() && !name->second.empty())
      info->name = name->second;
  }
  HttpResponse version_response;
  if (HttpRequest(host, port, "GET", "/cgi-bin/magicBox.cgi?action=getSoftwareVersion", "", "",
                  username, password, &version_response, error) &&
      version_response.status == 200) {
    const auto version_values = ParseKeyValues(version_response.body);
    const auto version = version_values.find("version");
    if (version != version_values.end())
      info->firmware = version->second;
    const auto serial = version_values.find("serialNumber");
    if (serial != version_values.end())
      info->serial = serial->second;
  }
  return true;
}

bool ReplaceXmlTag(std::string* xml, const std::string& tag, const std::string& value) {
  const std::string open = "<" + tag;
  size_t pos = xml->find(open);
  if (pos == std::string::npos)
    return false;
  const size_t open_end = xml->find('>', pos);
  if (open_end == std::string::npos)
    return false;
  const size_t close = xml->find("</" + tag, open_end);
  if (close == std::string::npos)
    return false;
  xml->replace(open_end + 1, close - open_end - 1, value);
  return true;
}

int IntValue(const std::string& text) {
  return std::atoi(Trim(text).c_str());
}

bool HikvisionEncoder(const std::string& host, int port, const std::string& username,
                      const std::string& password, BrandEncoderConfig* config, std::string* error) {
  HttpResponse response;
  if (!HttpRequest(host, port, "GET", "/ISAPI/Streaming/channels/101", "", "", username, password,
                   &response, error))
    return false;
  if (response.status != 200 || response.body.find("<Video>") == std::string::npos) {
    if (error != nullptr)
      *error = "the camera rejected the ISAPI streaming-channel request (HTTP " +
               std::to_string(response.status) + ")";
    return false;
  }
  config->encoding = ExtractTag(response.body, "videoCodecType");
  config->width = IntValue(ExtractTag(response.body, "videoResolutionWidth"));
  config->height = IntValue(ExtractTag(response.body, "videoResolutionHeight"));
  int frame_rate = IntValue(ExtractTag(response.body, "videoFrameRate"));
  if (frame_rate <= 0)
    frame_rate = IntValue(ExtractTag(response.body, "maxFrameRate"));
  config->fps = frame_rate > 120 ? frame_rate / 100 : frame_rate;
  config->bitrate_kbps = IntValue(ExtractTag(response.body, "vbrUpperCap"));
  if (config->bitrate_kbps <= 0)
    config->bitrate_kbps = IntValue(ExtractTag(response.body, "videoBitrate"));
  config->quality = IntValue(ExtractTag(response.body, "videoQuality"));
  return config->width > 0 && config->height > 0;
}

bool SetHikvisionEncoder(const std::string& host, int port, const std::string& username,
                         const std::string& password, const BrandEncoderConfig& config,
                         std::string* error) {
  HttpResponse current;
  if (!HttpRequest(host, port, "GET", "/ISAPI/Streaming/channels/101", "", "", username, password,
                   &current, error))
    return false;
  if (current.status != 200) {
    if (error != nullptr)
      *error = "cannot read the current ISAPI streaming-channel settings";
    return false;
  }
  std::string xml = current.body;
  if (!config.encoding.empty() && !ReplaceXmlTag(&xml, "videoCodecType", config.encoding)) {
    if (error != nullptr)
      *error = "the camera does not expose videoCodecType";
    return false;
  }
  if (config.width > 0 &&
      !ReplaceXmlTag(&xml, "videoResolutionWidth", std::to_string(config.width))) {
    if (error != nullptr)
      *error = "the camera does not expose videoResolutionWidth";
    return false;
  }
  if (config.height > 0 &&
      !ReplaceXmlTag(&xml, "videoResolutionHeight", std::to_string(config.height))) {
    if (error != nullptr)
      *error = "the camera does not expose videoResolutionHeight";
    return false;
  }
  if (config.fps > 0) {
    const std::string value = std::to_string(config.fps * 100);
    const bool replaced = ReplaceXmlTag(&xml, "videoFrameRate", value);
    ReplaceXmlTag(&xml, "maxFrameRate", value);
    if (!replaced) {
      if (error != nullptr)
        *error = "the camera does not expose videoFrameRate";
      return false;
    }
  }
  if (config.bitrate_kbps > 0) {
    const std::string value = std::to_string(config.bitrate_kbps);
    const bool replaced = ReplaceXmlTag(&xml, "vbrUpperCap", value);
    if (!replaced && !ReplaceXmlTag(&xml, "videoBitrate", value)) {
      if (error != nullptr)
        *error = "the camera does not expose a bitrate field";
      return false;
    }
  }
  HttpResponse response;
  if (!HttpRequest(host, port, "PUT", "/ISAPI/Streaming/channels/101", xml, "application/xml",
                   username, password, &response, error))
    return false;
  if (response.status != 200 || response.body.find("statusCode>4") != std::string::npos) {
    if (error != nullptr)
      *error = "the camera rejected the ISAPI encoder update (HTTP " +
               std::to_string(response.status) + ")";
    return false;
  }
  return true;
}

const std::string* FindDahuaValue(const std::map<std::string, std::string>& values,
                                  const std::string& suffix) {
  for (const auto& [key, value] : values) {
    if (key.size() >= suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        key.find("MainFormat[0].Video.") != std::string::npos) {
      return &value;
    }
  }
  return nullptr;
}

bool DahuaEncoder(const std::string& host, int port, const std::string& username,
                  const std::string& password, BrandEncoderConfig* config, std::string* error) {
  HttpResponse response;
  if (!HttpRequest(host, port, "GET", "/cgi-bin/configManager.cgi?action=getConfig&name=Encode", "",
                   "", username, password, &response, error))
    return false;
  if (response.status != 200) {
    if (error != nullptr)
      *error = "the camera rejected the Dahua Encode config request (HTTP " +
               std::to_string(response.status) + ")";
    return false;
  }
  const auto values = ParseKeyValues(response.body);
  const std::string* encoding = FindDahuaValue(values, ".Video.Compression");
  const std::string* width = FindDahuaValue(values, ".Video.Width");
  const std::string* height = FindDahuaValue(values, ".Video.Height");
  const std::string* fps = FindDahuaValue(values, ".Video.FPS");
  const std::string* bitrate = FindDahuaValue(values, ".Video.BitRate");
  if (width == nullptr || height == nullptr || fps == nullptr) {
    if (error != nullptr)
      *error = "the camera returned no main-stream encoder fields";
    return false;
  }
  config->encoding = encoding != nullptr ? *encoding : "H.264";
  config->width = IntValue(*width);
  config->height = IntValue(*height);
  config->fps = IntValue(*fps);
  config->bitrate_kbps = bitrate != nullptr ? IntValue(*bitrate) : 0;
  return true;
}

bool SetDahuaEncoder(const std::string& host, int port, const std::string& username,
                     const std::string& password, const BrandEncoderConfig& config,
                     std::string* error) {
  std::string path = "/cgi-bin/configManager.cgi?action=setConfig";
  if (!config.encoding.empty())
    path += "&Encode[0].MainFormat[0].Video.Compression=" + config.encoding;
  if (config.width > 0)
    path += "&Encode[0].MainFormat[0].Video.Width=" + std::to_string(config.width);
  if (config.height > 0)
    path += "&Encode[0].MainFormat[0].Video.Height=" + std::to_string(config.height);
  if (config.fps > 0)
    path += "&Encode[0].MainFormat[0].Video.FPS=" + std::to_string(config.fps);
  if (config.bitrate_kbps > 0)
    path += "&Encode[0].MainFormat[0].Video.BitRate=" + std::to_string(config.bitrate_kbps);
  HttpResponse response;
  if (!HttpRequest(host, port, "GET", path, "", "", username, password, &response, error))
    return false;
  if (response.status != 200 || Lower(response.body).find("error") != std::string::npos) {
    if (error != nullptr)
      *error = "the camera rejected the Dahua encoder update";
    return false;
  }
  return true;
}

} // namespace

bool GetBrandEncoderConfig(const std::string& protocol, const std::string& host, int port,
                           const std::string& username, const std::string& password,
                           BrandEncoderConfig* config, std::string* error) {
  if (protocol == "hikvision")
    return HikvisionEncoder(host, port, username, password, config, error);
  if (protocol == "dahua")
    return DahuaEncoder(host, port, username, password, config, error);
  if (error != nullptr)
    *error = "the channel has no supported brand encoder API";
  return false;
}

bool SetBrandEncoderConfig(const std::string& protocol, const std::string& host, int port,
                           const std::string& username, const std::string& password,
                           const BrandEncoderConfig& config, std::string* error) {
  if (protocol == "hikvision")
    return SetHikvisionEncoder(host, port, username, password, config, error);
  if (protocol == "dahua")
    return SetDahuaEncoder(host, port, username, password, config, error);
  if (error != nullptr)
    *error = "the channel has no supported brand encoder API";
  return false;
}

bool ProbeBrandHttp(const std::string& host, int port, const std::string& username,
                    const std::string& password, const std::string& preferred,
                    BrandDeviceInfo* info, std::string* error) {
  if (info == nullptr) {
    if (error != nullptr)
      *error = "missing device info output";
    return false;
  }
  const auto try_hikvision = [&]() {
    std::string probe_error;
    return ProbeHikvision(host, port, username, password, info, &probe_error);
  };
  const auto try_dahua = [&]() {
    std::string probe_error;
    return ProbeDahua(host, port, username, password, info, &probe_error);
  };
  if (preferred == "hikvision") {
    if (try_hikvision() || try_dahua())
      return true;
  } else if (preferred == "dahua") {
    if (try_dahua() || try_hikvision())
      return true;
  } else if (try_hikvision() || try_dahua()) {
    return true;
  }
  if (error != nullptr)
    *error = "no Hikvision or Dahua HTTP service answered";
  return false;
}

bool SetBrandTime(const std::string& protocol, const std::string& host, int port,
                  const std::string& username, const std::string& password, int64_t epoch_ms,
                  std::string* error) {
  HttpResponse response;
  if (protocol == "hikvision") {
    const std::string body = "<Time><timeMode>manual</timeMode><localTime>" +
                             LocalIsoTime(epoch_ms) + "</localTime></Time>";
    if (!HttpRequest(host, port, "PUT", "/ISAPI/System/time", body, "application/xml", username,
                     password, &response, error))
      return false;
    if (response.status != 200) {
      if (error != nullptr)
        *error = "the camera rejected the ISAPI time update (HTTP " +
                 std::to_string(response.status) + ")";
      return false;
    }
    return true;
  }
  if (protocol == "dahua") {
    const std::string path = "/cgi-bin/global.cgi?action=setCurrentTime&time=" +
                             PercentEncodeSpaces(LocalPlainTime(epoch_ms));
    if (!HttpRequest(host, port, "GET", path, "", "", username, password, &response, error))
      return false;
    if (response.status != 200 || Lower(response.body).find("error") != std::string::npos) {
      if (error != nullptr)
        *error = "the camera rejected the Dahua time update";
      return false;
    }
    return true;
  }
  if (error != nullptr)
    *error = "the channel has no supported brand time API";
  return false;
}

} // namespace uvnvr
