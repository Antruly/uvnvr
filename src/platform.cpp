#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace uvnvr::platform {
namespace {

uint64_t ReadUint(const std::string& path, const std::string& key) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind(key, 0) == 0) {
      std::istringstream stream(line.substr(key.size()));
      uint64_t value = 0;
      stream >> value;
      return value;
    }
  }
  return 0;
}

} // namespace

std::string ExecutablePath() {
#ifdef _WIN32
  char path[MAX_PATH] = {0};
  const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  return length == 0 ? std::string() : std::string(path, length);
#elif defined(__APPLE__)
  char path[4096] = {0};
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0)
    return {};
  return std::string(path);
#else
  char path[4096] = {0};
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  return length <= 0 ? std::string() : std::string(path, static_cast<size_t>(length));
#endif
}

bool OpenUrl(const std::string& url) {
  if (url.empty())
    return false;
#ifdef _WIN32
  const int length = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
  if (length <= 0)
    return false;
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), length);
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(result) > 32;
#else
  // Detach the handler so a launcher that stays in the foreground (some
  // xdg-open versions) never blocks the recorder.
  const pid_t pid = fork();
  if (pid != 0)
    return pid > 0;
  if (setsid() < 0) {
    // Keep going: the handler still works when setsid is restricted.
  }
  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO)
      close(null_fd);
  }
#ifdef __APPLE__
  execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
  execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
  execlp("gio", "gio", "open", url.c_str(), static_cast<char*>(nullptr));
#endif
  _exit(127);
#endif
}

std::string HostName() {
#ifdef _WIN32
  char name[256] = {0};
  DWORD size = sizeof(name);
  if (!GetComputerNameA(name, &size))
    return "unknown";
  return std::string(name, size);
#else
  char name[256] = {0};
  if (gethostname(name, sizeof(name) - 1) != 0)
    return "unknown";
  return name;
#endif
}

std::string OsDescription() {
#ifdef _WIN32
  return "Windows";
#else
  struct utsname info {};
  if (uname(&info) != 0)
    return "unknown";
  return std::string(info.sysname) + " " + info.release;
#endif
}

uint64_t TotalMemoryBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
  uint64_t value = 0;
  size_t size = sizeof(value);
  if (sysctlbyname("hw.memsize", &value, &size, nullptr, 0) == 0)
    return value;
  return 0;
#else
  const uint64_t kb = ReadUint("/proc/meminfo", "MemTotal:");
  return kb * 1024;
#endif
}

uint64_t FreeMemoryBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#elif defined(__APPLE__)
  vm_statistics64_data_t stats{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats),
                        &count) != KERN_SUCCESS) {
    return 0;
  }
  const uint64_t page = static_cast<uint64_t>(vm_page_size);
  return (static_cast<uint64_t>(stats.free_count) + static_cast<uint64_t>(stats.inactive_count)) *
         page;
#else
  const uint64_t available = ReadUint("/proc/meminfo", "MemAvailable:");
  if (available > 0)
    return available * 1024;
  return ReadUint("/proc/meminfo", "MemFree:") * 1024;
#endif
}

double UptimeSeconds() {
#ifdef _WIN32
  return static_cast<double>(GetTickCount64()) / 1000.0;
#elif defined(__APPLE__)
  struct timeval boot {};
  size_t size = sizeof(boot);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &boot, &size, nullptr, 0) != 0)
    return 0;
  const std::time_t now = std::time(nullptr);
  return static_cast<double>(now - boot.tv_sec);
#else
  std::ifstream input("/proc/uptime");
  double value = 0;
  input >> value;
  return value;
#endif
}

int CpuCount() {
  const unsigned value = std::thread::hardware_concurrency();
  return value == 0 ? 1 : static_cast<int>(value);
}

double CpuUsagePercent(uint64_t* last_total, uint64_t* last_idle) {
  uint64_t total = 0;
  uint64_t idle = 0;
#ifdef _WIN32
  FILETIME idle_time{};
  FILETIME kernel_time{};
  FILETIME user_time{};
  if (!GetSystemTimes(&idle_time, &kernel_time, &user_time))
    return 0;
  auto to_uint = [](const FILETIME& t) {
    ULARGE_INTEGER value;
    value.LowPart = t.dwLowDateTime;
    value.HighPart = t.dwHighDateTime;
    return static_cast<uint64_t>(value.QuadPart);
  };
  const uint64_t idle_ticks = to_uint(idle_time);
  const uint64_t kernel = to_uint(kernel_time);
  const uint64_t user = to_uint(user_time);
  total = kernel + user;
  idle = idle_ticks;
#elif defined(__APPLE__)
  host_cpu_load_info_data_t info{};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info),
                      &count) != KERN_SUCCESS) {
    return 0;
  }
  for (int i = 0; i < CPU_STATE_MAX; ++i)
    total += info.cpu_ticks[i];
  idle = info.cpu_ticks[CPU_STATE_IDLE];
#else
  std::ifstream input("/proc/stat");
  std::string line;
  if (!std::getline(input, line) || line.rfind("cpu ", 0) != 0)
    return 0;
  std::istringstream stream(line.substr(4));
  uint64_t values[10] = {0};
  for (uint64_t& value : values) {
    if (!(stream >> value))
      break;
    total += value;
  }
  idle = values[3] + values[4];
#endif
  double usage = 0;
  if (*last_total != 0 && total > *last_total) {
    const uint64_t delta_total = total - *last_total;
    const uint64_t delta_idle = idle >= *last_idle ? idle - *last_idle : 0;
    usage = 100.0 * (1.0 - static_cast<double>(delta_idle) / delta_total);
  } else if (total > 0) {
    usage = 100.0 * (1.0 - static_cast<double>(idle) / total);
  }
  *last_total = total;
  *last_idle = idle;
  return std::clamp(usage, 0.0, 100.0);
}

} // namespace uvnvr::platform
