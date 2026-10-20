#include "ntp.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
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

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
constexpr int64_t kNtpEpochOffsetSeconds = 2208988800LL;
} // namespace

bool QueryNtpOffset(const std::string& server, int64_t* offset_ms, std::string* error) {
  if (!InitSockets(error))
    return false;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(server.c_str(), "123", &hints, &result) != 0 || result == nullptr) {
    if (error)
      *error = "cannot resolve NTP server " + server;
    return false;
  }
  Socket socket = kInvalidSocket;
  for (auto* entry = result; entry != nullptr; entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket == kInvalidSocket)
      continue;
#ifdef SO_RCVTIMEO
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
    if (::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0)
      break;
    CloseSocket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(result);
  if (socket == kInvalidSocket) {
    if (error)
      *error = "cannot connect to NTP server";
    return false;
  }
  unsigned char packet[48] = {0};
  packet[0] = 0x1b; // LI 0, VN 3, mode 3 (client)
  const int64_t send_ms = NowMs();
  if (::send(socket, reinterpret_cast<const char*>(packet), sizeof(packet), 0) !=
      static_cast<int>(sizeof(packet))) {
    CloseSocket(socket);
    if (error)
      *error = "cannot send NTP request";
    return false;
  }
  unsigned char response[48] = {0};
  const int n =
      static_cast<int>(::recv(socket, reinterpret_cast<char*>(response), sizeof(response), 0));
  CloseSocket(socket);
  if (n < 48) {
    if (error)
      *error = "NTP response is short";
    return false;
  }
  uint64_t seconds = 0;
  uint64_t fraction = 0;
  for (int i = 0; i < 4; ++i) {
    seconds = (seconds << 8) | response[40 + i];
    fraction = (fraction << 8) | response[44 + i];
  }
  const int64_t server_ms = static_cast<int64_t>(seconds - kNtpEpochOffsetSeconds) * 1000 +
                            static_cast<int64_t>((fraction * 1000) >> 32);
  *offset_ms = server_ms - send_ms;
  return true;
}

bool ApplyTimeOffset(int64_t offset_ms, std::string* error) {
  const int64_t target = NowMs() + offset_ms;
#ifdef _WIN32
  ULARGE_INTEGER value;
  value.QuadPart = static_cast<ULONGLONG>(target) * 10000ULL + 116444736000000000ULL;
  FILETIME file_time;
  file_time.dwLowDateTime = value.LowPart;
  file_time.dwHighDateTime = value.HighPart;
  SYSTEMTIME system_time;
  if (!FileTimeToSystemTime(&file_time, &system_time) || !SetSystemTime(&system_time)) {
    if (error)
      *error = "SetSystemTime failed; administrator privileges are required";
    return false;
  }
#else
  struct timeval tv {};
  tv.tv_sec = static_cast<time_t>(target / 1000);
  tv.tv_usec = static_cast<suseconds_t>((target % 1000) * 1000);
  if (settimeofday(&tv, nullptr) != 0) {
    if (error)
      *error = std::string("settimeofday failed: ") + std::strerror(errno) +
               " (CAP_SYS_TIME is required)";
    return false;
  }
#endif
  return true;
}

} // namespace uvnvr
