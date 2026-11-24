#include "discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <string_view>
#include <thread>
#include <uv.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace uvnvr {
namespace {

using json = nlohmann::json;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) {
  if (socket != kInvalidSocket)
    closesocket(socket);
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
void CloseSocket(Socket socket) {
  if (socket >= 0)
    close(socket);
}
bool EnsureWinsock(std::string*) {
  return true;
}
#endif

// ---------------------------------------------------------------------------
// Small text helpers

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

bool IEquals(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

bool StartsWithCI(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && IEquals(value.substr(0, prefix.size()), prefix);
}

bool IsNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' ||
         c == '.';
}

std::string LocalName(std::string name) {
  const size_t colon = name.rfind(':');
  if (colon != std::string::npos)
    name = name.substr(colon + 1);
  return name;
}

// Returns the inner text of the first <tag>...</tag>, ignoring namespace
// prefixes and ASCII case. SADP firmware is inconsistent about both.
std::string ExtractTagCI(const std::string& xml, std::string_view tag) {
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' ||
        xml[pos + 1] == '!') {
      ++pos;
      continue;
    }
    const size_t name_start = pos + 1;
    size_t name_end = name_start;
    while (name_end < xml.size() && IsNameChar(xml[name_end]))
      ++name_end;
    const std::string name = LocalName(xml.substr(name_start, name_end - name_start));
    if (!IEquals(name, tag)) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string::npos)
      return {};
    if (xml[open_end - 1] == '/')
      return {};
    size_t scan = open_end + 1;
    while ((scan = xml.find("</", scan)) != std::string::npos) {
      size_t end = scan + 2;
      while (end < xml.size() && IsNameChar(xml[end]))
        ++end;
      const std::string close_name = LocalName(xml.substr(scan + 2, end - (scan + 2)));
      if (IEquals(close_name, tag))
        return Trim(xml.substr(open_end + 1, scan - open_end - 1));
      scan = end;
    }
    return {};
  }
  return {};
}

std::string ExtractAnyTagCI(const std::string& xml,
                            std::initializer_list<std::string_view> tags) {
  for (const std::string_view tag : tags) {
    std::string value = ExtractTagCI(xml, tag);
    if (!value.empty())
      return value;
  }
  return {};
}

// One block per <tag>...</tag>, with case-insensitive names. Used for each
// ProbeMatch in a datagram.
std::vector<std::string> ExtractBlocksCI(const std::string& xml, std::string_view tag) {
  std::vector<std::string> blocks;
  size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    if (pos + 1 >= xml.size() || xml[pos + 1] == '/' || xml[pos + 1] == '?' ||
        xml[pos + 1] == '!') {
      ++pos;
      continue;
    }
    const size_t name_start = pos + 1;
    size_t name_end = name_start;
    while (name_end < xml.size() && IsNameChar(xml[name_end]))
      ++name_end;
    const std::string name = LocalName(xml.substr(name_start, name_end - name_start));
    if (!IEquals(name, tag)) {
      pos = name_end;
      continue;
    }
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string::npos)
      break;
    if (xml[open_end - 1] == '/') {
      pos = open_end + 1;
      continue;
    }
    size_t scan = open_end + 1;
    size_t close_start = std::string::npos;
    size_t close_end = std::string::npos;
    while ((scan = xml.find("</", scan)) != std::string::npos) {
      size_t end = scan + 2;
      while (end < xml.size() && IsNameChar(xml[end]))
        ++end;
      const std::string close_name = LocalName(xml.substr(scan + 2, end - (scan + 2)));
      if (IEquals(close_name, tag)) {
        close_start = scan;
        close_end = end;
        break;
      }
      scan = end;
    }
    if (close_start == std::string::npos)
      break;
    blocks.push_back(Trim(xml.substr(open_end + 1, close_start - open_end - 1)));
    pos = close_end;
  }
  return blocks;
}

int ParsePort(const std::string& value, int fallback) {
  const std::string text = Trim(value);
  if (text.empty())
    return fallback;
  int port = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
  if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || port < 1 ||
      port > 65535)
    return fallback;
  return port;
}

// Accepts 44:19:b6:..., 44-19-b6-... and 4419b6... and returns the canonical
// upper-case colon form. Unknown shapes are returned trimmed and unchanged.
std::string NormaliseMac(const std::string& value) {
  std::string compact;
  compact.reserve(value.size());
  for (const char c : value) {
    if (std::isxdigit(static_cast<unsigned char>(c)))
      compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (compact.size() != 12)
    return Trim(value);
  std::string mac;
  mac.reserve(17);
  for (size_t i = 0; i < compact.size(); ++i) {
    if (i > 0 && i % 2 == 0)
      mac.push_back(':');
    mac.push_back(compact[i]);
  }
  return mac;
}

std::string LowerAlnum(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string IpFromSockaddr(const sockaddr_in& address) {
  char text[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr)
    return {};
  return text;
}

bool IsValidIpv4(const std::string& text) {
  in_addr address{};
  return inet_pton(AF_INET, text.c_str(), &address) == 1;
}

// ---------------------------------------------------------------------------
// Network layout

struct LocalInterface {
  in_addr address{};
  std::string ip;
  std::string broadcast;
};

std::vector<LocalInterface> LocalInterfaces() {
  std::vector<LocalInterface> interfaces;
  uv_interface_address_t* entries = nullptr;
  int count = 0;
  if (uv_interface_addresses(&entries, &count) != 0)
    return interfaces;
  for (int i = 0; i < count; ++i) {
    const auto& entry = entries[i];
    if (entry.address.address4.sin_family != AF_INET || entry.is_internal)
      continue;
    const uint32_t host = ntohl(entry.address.address4.sin_addr.s_addr);
    if (host == 0 || (host >> 24) == 127)
      continue;
    LocalInterface local;
    local.address = entry.address.address4.sin_addr;
    local.ip = IpFromSockaddr(entry.address.address4);
    const uint32_t mask_host = ntohl(entry.netmask.netmask4.sin_addr.s_addr);
    if (mask_host != 0) {
      const uint32_t broadcast_host = (host & mask_host) | ~mask_host;
      in_addr broadcast{};
      broadcast.s_addr = htonl(broadcast_host);
      local.broadcast = IpFromSockaddr(sockaddr_in{AF_INET, 0, broadcast});
    }
    const bool duplicate =
        std::any_of(interfaces.begin(), interfaces.end(), [&](const LocalInterface& existing) {
          return existing.address.s_addr == local.address.s_addr;
        });
    if (!duplicate)
      interfaces.push_back(local);
  }
  uv_free_interface_addresses(entries, count);
  return interfaces;
}

struct Target {
  sockaddr_in address{};
  std::string label;
  bool multicast = false;
};

bool ParseTarget(const std::string& text, int default_port, Target* target, std::string* error) {
  std::string value = Trim(text);
  if (value.empty()) {
    if (error)
      *error = "empty target";
    return false;
  }
  std::string host = value;
  int port = default_port;
  const size_t colon = value.rfind(':');
  if (colon != std::string::npos) {
    host = value.substr(0, colon);
    const std::string port_text = value.substr(colon + 1);
    int parsed = 0;
    const auto result =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != port_text.data() + port_text.size() ||
        parsed < 1 || parsed > 65535) {
      if (error)
        *error = "invalid port in target " + value;
      return false;
    }
    port = parsed;
  }
  in_addr address{};
  if (inet_pton(AF_INET, host.c_str(), &address) != 1) {
    if (error)
      *error = "target " + value + " is not a valid IPv4 address";
    return false;
  }
  target->address = sockaddr_in{};
  target->address.sin_family = AF_INET;
  target->address.sin_port = htons(static_cast<uint16_t>(port));
  target->address.sin_addr = address;
  target->label = host + ":" + std::to_string(port);
  const uint32_t host_order = ntohl(address.s_addr);
  target->multicast = (host_order >> 28) == 0xe;
  return true;
}

std::vector<Target> BuildTargets(const std::vector<std::string>& configured, int default_port,
                                 const std::vector<std::string>& fallbacks,
                                 std::vector<std::string>* warnings) {
  std::vector<Target> targets;
  std::map<std::string, bool> seen;
  const std::vector<std::string>& source = configured.empty() ? fallbacks : configured;
  for (const std::string& entry : source) {
    Target target;
    std::string parse_error;
    if (!ParseTarget(entry, default_port, &target, &parse_error)) {
      if (warnings)
        warnings->push_back(parse_error);
      continue;
    }
    if (!seen.emplace(target.label, true).second)
      continue;
    targets.push_back(std::move(target));
  }
  return targets;
}

void SendTargets(Socket socket, const std::vector<Target>& targets,
                 const std::vector<LocalInterface>& interfaces, const std::string& payload,
                 int* sent) {
  for (const Target& target : targets) {
    if (target.multicast && !interfaces.empty()) {
      // Multicast follows the default route otherwise, which hides cameras on
      // a second NIC. Send once per local IPv4 address.
      for (const LocalInterface& iface : interfaces) {
        setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&iface.address), sizeof(iface.address));
        if (::sendto(socket, payload.data(), static_cast<int>(payload.size()), 0,
                     reinterpret_cast<const sockaddr*>(&target.address),
                     sizeof(target.address)) >= 0)
          ++*sent;
      }
    } else if (::sendto(socket, payload.data(), static_cast<int>(payload.size()), 0,
                        reinterpret_cast<const sockaddr*>(&target.address),
                        sizeof(target.address)) >= 0) {
      ++*sent;
    }
  }
}

Socket OpenProbeSocket(std::string* error) {
  const Socket socket = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket == kInvalidSocket) {
    if (error)
      *error = "cannot create the discovery socket";
    return kInvalidSocket;
  }
  int reuse = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  int broadcast = 1;
  setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
             sizeof(broadcast));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = 0;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
    CloseSocket(socket);
    if (error)
      *error = "cannot bind the discovery socket";
    return kInvalidSocket;
  }
  return socket;
}

// Waits for one datagram. Returns false when the deadline expired or the
// socket failed; the caller stops collecting either way.
bool WaitReadable(Socket socket, const std::chrono::steady_clock::time_point& deadline) {
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0)
      return false;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(remaining / 1000);
    timeout.tv_usec = static_cast<long>((remaining % 1000) * 1000);
#ifdef _WIN32
    const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
#else
    const int ready = select(socket + 1, &read_set, nullptr, nullptr, &timeout);
#endif
    if (ready > 0)
      return FD_ISSET(socket, &read_set) != 0;
    if (ready == 0)
      return false;
#ifdef _WIN32
    if (WSAGetLastError() != WSAEINTR)
      return false;
#else
    if (errno != EINTR)
      return false;
#endif
  }
}

std::string ReceiveDatagram(Socket socket, sockaddr_in* from) {
  std::array<char, 65535> buffer{};
#ifdef _WIN32
  int from_length = sizeof(*from);
#else
  socklen_t from_length = sizeof(*from);
#endif
  const int n = static_cast<int>(::recvfrom(socket, buffer.data(),
                                            static_cast<int>(buffer.size()), 0,
                                            reinterpret_cast<sockaddr*>(from), &from_length));
  if (n <= 0)
    return {};
  return std::string(buffer.data(), static_cast<size_t>(n));
}

std::string GenerateUuid() {
  unsigned char bytes[16] = {};
  if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
    for (unsigned char& byte : bytes)
      byte = static_cast<unsigned char>(std::rand() & 0xff);
  }
  // RFC 4122 version 4 layout; SADP only needs an opaque unique value.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  char text[37] = {};
  std::snprintf(text, sizeof(text),
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0],
                bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return text;
}

// ---------------------------------------------------------------------------
// Hikvision SADP

const std::vector<std::string> kDefaultSadpTargets = {"239.255.255.250:37020",
                                                      "255.255.255.255:37020"};

std::vector<SadpDevice> ParseSadpResponse(const std::string& response, const std::string& sender,
                                          bool* matched) {
  *matched = false;
  std::vector<SadpDevice> devices;
  std::vector<std::string> blocks = ExtractBlocksCI(response, "ProbeMatch");
  if (blocks.empty())
    blocks.push_back(response);
  for (const std::string& block : blocks) {
    const std::string ip_text =
        ExtractAnyTagCI(block, {"IPv4Address", "IPAddress", "IPv4", "IP"});
    const std::string mac_text = ExtractTagCI(block, "MAC");
    const std::string serial =
        ExtractAnyTagCI(block, {"DeviceSN", "DeviceSerialNo", "SerialNo", "SerialNumber", "SN"});
    const std::string device_type =
        ExtractAnyTagCI(block, {"DeviceType", "Model", "DeviceModel"});
    const std::string description =
        ExtractAnyTagCI(block, {"DeviceDescription", "Description", "DeviceName"});
    const std::string firmware =
        ExtractAnyTagCI(block, {"SoftwareVersion", "FirmwareVersion", "Version"});
    const std::string uuid = ExtractTagCI(block, "Uuid");
    const std::string http_port_text = ExtractTagCI(block, "HttpPort");
    const std::string rtsp_port_text = ExtractTagCI(block, "RtspPort");
    const bool found = !ip_text.empty() || !mac_text.empty() || !serial.empty() ||
                       !device_type.empty() || !description.empty() || !uuid.empty();
    if (!found)
      continue;
    SadpDevice device;
    device.ip = IsValidIpv4(ip_text) ? Trim(ip_text) : sender;
    if (device.ip.empty())
      continue;
    device.mac = NormaliseMac(mac_text);
    device.serial = Trim(serial);
    device.device_type = Trim(device_type);
    device.description = Trim(description);
    device.firmware = Trim(firmware);
    device.uuid = Trim(uuid);
    device.http_port = ParsePort(http_port_text, 80);
    device.rtsp_port = ParsePort(rtsp_port_text, 554);
    *matched = true;
    devices.push_back(std::move(device));
  }
  return devices;
}

void ProbeSadp(const std::vector<std::string>& configured_targets,
               const std::chrono::steady_clock::time_point& deadline, bool add_broadcast,
               std::vector<SadpDevice>* devices, ScanProtocolStats* stats,
               std::vector<std::string>* warnings) {
  std::string error;
  if (!EnsureWinsock(&error)) {
    stats->error = error;
    return;
  }
  std::vector<std::string> targets = configured_targets;
  const std::vector<LocalInterface> interfaces = LocalInterfaces();
  if (add_broadcast) {
    for (const LocalInterface& iface : interfaces) {
      if (!iface.broadcast.empty())
        targets.push_back(iface.broadcast + ":37020");
    }
  }
  std::vector<Target> parsed = BuildTargets(targets, 37020, kDefaultSadpTargets, warnings);
  if (parsed.empty()) {
    stats->error = "no valid SADP target";
    return;
  }
  const Socket socket = OpenProbeSocket(&error);
  if (socket == kInvalidSocket) {
    stats->error = error;
    return;
  }
  const std::string payload =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Probe><Uuid>" + GenerateUuid() +
      "</Uuid><Types>inquiry</Types></Probe>";
  SendTargets(socket, parsed, interfaces, payload, &stats->sent);
  if (stats->sent == 0) {
    stats->error = "cannot send the SADP probe";
    CloseSocket(socket);
    return;
  }
  std::map<std::string, bool> seen;
  while (WaitReadable(socket, deadline)) {
    sockaddr_in from{};
    const std::string response = ReceiveDatagram(socket, &from);
    if (response.empty())
      continue;
    bool matched = false;
    for (const SadpDevice& device : ParseSadpResponse(response, IpFromSockaddr(from), &matched)) {
      const std::string key = !device.mac.empty() ? "mac:" + LowerAlnum(device.mac)
                                                  : "ip:" + device.ip;
      if (!seen.emplace(key, true).second)
        continue;
      devices->push_back(device);
    }
    if (matched)
      ++stats->responses;
  }
  CloseSocket(socket);
}

// ---------------------------------------------------------------------------
// Dahua DHDiscover

const std::vector<std::string> kDefaultDahuaTargets = {"239.255.255.231:37810",
                                                       "266.0.0.100:22531"};

const json* FindJsonKey(const json& value, std::initializer_list<std::string_view> names) {
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      for (const std::string_view name : names) {
        if (IEquals(it.key(), name))
          return &it.value();
      }
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (it.value().is_object() || it.value().is_array()) {
        if (const json* found = FindJsonKey(it.value(), names))
          return found;
      }
    }
  } else if (value.is_array()) {
    for (const json& entry : value) {
      if (entry.is_object() || entry.is_array()) {
        if (const json* found = FindJsonKey(entry, names))
          return found;
      }
    }
  }
  return nullptr;
}

std::string JsonText(const json* value) {
  if (value == nullptr)
    return {};
  if (value->is_string())
    return Trim(value->get<std::string>());
  if (value->is_number_integer())
    return std::to_string(value->get<int64_t>());
  if (value->is_number_unsigned())
    return std::to_string(value->get<uint64_t>());
  if (value->is_number_float())
    return value->dump();
  if (value->is_boolean())
    return value->get<bool>() ? "true" : "false";
  return {};
}

int JsonPort(const json* value, int fallback) {
  const std::string text = JsonText(value);
  return ParsePort(text, fallback);
}

std::vector<DahuaDevice> ParseDahuaResponse(const std::string& response,
                                            const std::string& sender, bool* matched) {
  *matched = false;
  std::string text = response;
  // Some firmware prefixes the JSON with a length or protocol header.
  const size_t brace = text.find('{');
  if (brace == std::string::npos)
    return {};
  const json document = json::parse(text.substr(brace), nullptr, false);
  if (document.is_discarded() || !document.is_object())
    return {};

  const json* ip_value =
      FindJsonKey(document, {"IPv4Address", "IPAddress", "IPv4", "IP", "DeviceIP"});
  const json* mac_value = FindJsonKey(document, {"MACAddress", "MAC"});
  const json* serial_value =
      FindJsonKey(document, {"SerialNo", "SerialNumber", "DeviceSN", "SN", "Serial"});
  const json* type_value = FindJsonKey(document, {"DeviceType", "Model", "Type"});
  const json* name_value =
      FindJsonKey(document, {"Hostname", "DeviceName", "DeviceDescription", "Name", "Description"});
  const json* version_value =
      FindJsonKey(document, {"Version", "SoftwareVersion", "FirmwareVersion", "SoftVersion"});
  const json* http_value = FindJsonKey(document, {"HttpPort", "WebPort", "HTTP"});
  const json* rtsp_value = FindJsonKey(document, {"RtspPort", "RTSP"});

  const std::string ip_text = JsonText(ip_value);
  const std::string mac_text = JsonText(mac_value);
  const std::string serial = JsonText(serial_value);
  const std::string device_type = JsonText(type_value);
  const std::string name = JsonText(name_value);
  const std::string version = JsonText(version_value);
  const bool found = !ip_text.empty() || !mac_text.empty() || !serial.empty() ||
                     !device_type.empty() || !name.empty() || !version.empty();
  if (!found)
    return {};

  DahuaDevice device;
  device.ip = IsValidIpv4(ip_text) ? Trim(ip_text) : sender;
  if (device.ip.empty())
    return {};
  device.mac = NormaliseMac(mac_text);
  device.serial = serial;
  device.device_type = device_type;
  device.name = name;
  device.version = version;
  device.http_port = JsonPort(http_value, 80);
  device.rtsp_port = JsonPort(rtsp_value, 554);
  *matched = true;
  return {device};
}

void ProbeDahua(const std::vector<std::string>& configured_targets,
                const std::chrono::steady_clock::time_point& deadline,
                std::vector<DahuaDevice>* devices, ScanProtocolStats* stats,
                std::vector<std::string>* warnings) {
  std::string error;
  if (!EnsureWinsock(&error)) {
    stats->error = error;
    return;
  }
  std::vector<Target> parsed =
      BuildTargets(configured_targets, 37810, kDefaultDahuaTargets, warnings);
  if (parsed.empty()) {
    stats->error = "no valid Dahua target";
    return;
  }
  const Socket socket = OpenProbeSocket(&error);
  if (socket == kInvalidSocket) {
    stats->error = error;
    return;
  }
  const std::string payload =
      R"({"method":"DHDiscover.search","params":{"mac":"","unicast":0}})";
  SendTargets(socket, parsed, LocalInterfaces(), payload, &stats->sent);
  if (stats->sent == 0) {
    stats->error = "cannot send the Dahua probe";
    CloseSocket(socket);
    return;
  }
  std::map<std::string, bool> seen;
  while (WaitReadable(socket, deadline)) {
    sockaddr_in from{};
    const std::string response = ReceiveDatagram(socket, &from);
    if (response.empty())
      continue;
    bool matched = false;
    for (const DahuaDevice& device :
         ParseDahuaResponse(response, IpFromSockaddr(from), &matched)) {
      const std::string key = !device.mac.empty() ? "mac:" + LowerAlnum(device.mac)
                                                  : "ip:" + device.ip;
      if (!seen.emplace(key, true).second)
        continue;
      devices->push_back(device);
    }
    if (matched)
      ++stats->responses;
  }
  CloseSocket(socket);
}

} // namespace

std::string NormaliseMacAddress(const std::string& value) {
  return NormaliseMac(value);
}

ScanDiscoveryResult RunMultiProtocolDiscovery(const ScanOptions& options) {
  ScanDiscoveryResult result;
  result.window_ms = std::clamp(options.window_ms, 100, 60000);
  const auto window = std::chrono::milliseconds(result.window_ms);
  const auto deadline = std::chrono::steady_clock::now() + window;

  ScanProtocolStats onvif_stats{"onvif", 0, 0, {}};
  ScanProtocolStats sadp_stats{"sadp", 0, 0, {}};
  ScanProtocolStats dahua_stats{"dahua", 0, 0, {}};
  std::vector<std::string> sadp_warnings;
  std::vector<std::string> dahua_warnings;

  std::thread onvif_thread;
  std::thread sadp_thread;
  std::thread dahua_thread;

  if (options.onvif) {
    onvif_thread = std::thread([&] {
      std::string error;
      int sent = 0;
      int responses = 0;
      if (!DiscoverOnvif(&result.onvif_devices, result.window_ms, &error, &sent, &responses)) {
        onvif_stats.error = error;
      }
      onvif_stats.sent = sent;
      onvif_stats.responses = responses;
    });
  }
  if (options.sadp) {
    sadp_thread = std::thread([&] {
      // 255.255.255.255 only leaves through the default interface, so also
      // send to each interface's directed broadcast address.
      ProbeSadp(options.sadp_targets, deadline, true, &result.sadp_devices, &sadp_stats,
                &sadp_warnings);
    });
  }
  if (options.dahua) {
    dahua_thread = std::thread([&] {
      ProbeDahua(options.dahua_targets, deadline, &result.dahua_devices, &dahua_stats,
                 &dahua_warnings);
    });
  }

  if (onvif_thread.joinable())
    onvif_thread.join();
  if (sadp_thread.joinable())
    sadp_thread.join();
  if (dahua_thread.joinable())
    dahua_thread.join();

  if (options.onvif)
    result.stats.push_back(onvif_stats);
  if (options.sadp)
    result.stats.push_back(sadp_stats);
  if (options.dahua)
    result.stats.push_back(dahua_stats);
  for (const std::string& warning : sadp_warnings)
    result.warnings.push_back("sadp: " + warning);
  for (const std::string& warning : dahua_warnings)
    result.warnings.push_back("dahua: " + warning);
  return result;
}

} // namespace uvnvr
