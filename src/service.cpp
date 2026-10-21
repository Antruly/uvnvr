#include "service.h"

#include "platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <userenv.h>
#include <windows.h>
#include <wtsapi32.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace uvnvr::service {
namespace {

namespace fs = std::filesystem;

constexpr const char* kWindowsServiceName = "UvNVR";
// The old process needs time for web.stop() to drain connections before the
// replacement binds the same ports.
constexpr int kTransitionDelayMs = 5000;

std::string PlatformName() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

} // namespace

#ifdef _WIN32
namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty())
    return std::wstring();
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (length <= 0)
    return std::wstring();
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
  wide.resize(static_cast<size_t>(length - 1));
  return wide;
}

std::string LastErrorText(DWORD code) {
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string text = length > 0 && buffer != nullptr ? std::string(buffer, length) : std::string();
  if (buffer != nullptr)
    LocalFree(buffer);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
    text.pop_back();
  if (text.empty())
    text = "error " + std::to_string(code);
  return text;
}

// Quotes one argv element using the CommandLineToArgvW rules. The service
// binary path and configuration path contain spaces on Windows.
std::wstring QuoteArg(const std::wstring& arg) {
  if (arg.empty())
    return L"\"\"";
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring SystemBinary(const wchar_t* name) {
  wchar_t directory[MAX_PATH] = {0};
  const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
  if (length == 0)
    return name;
  return std::wstring(directory, length) + L"\\" + name;
}

int RunCommandLine(const std::wstring& command_line) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring mutable_line = command_line;
  if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exit_code == STILL_ACTIVE ? -2 : static_cast<int>(exit_code);
}

int RunSc(const std::wstring& arguments) {
  return RunCommandLine(QuoteArg(SystemBinary(L"sc.exe")) + L" " + arguments);
}

std::wstring BuildBinPath(const Config& config) {
  const std::string executable = platform::ExecutablePath();
  std::wstring bin = L"\"" + Widen(executable) + L"\" --service";
  if (!config.config_path.empty())
    bin += L" --config \"" + Widen(config.config_path) + L"\"";
  std::error_code ec;
  const fs::path data = fs::absolute(config.data_dir, ec);
  const std::string data_text = ec ? config.data_dir : data.string();
  if (!data_text.empty())
    bin += L" --data-dir \"" + Widen(data_text) + L"\"";
  return bin;
}

Config g_config;
ServiceHooks g_hooks;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
int g_exit_code = 0;

void RedirectServiceOutput() {
  std::error_code ec;
  const fs::path directory = fs::absolute(g_config.data_dir, ec) / "logs";
  fs::create_directories(directory, ec);
  if (ec)
    return;
  fs::path log_path = directory / "service.log";
  std::error_code size_ec;
  const uintmax_t size = fs::file_size(log_path, size_ec);
  if (!size_ec && size > 5 * 1024 * 1024) {
    std::error_code rename_ec;
    fs::rename(log_path, directory / "service.log.1", rename_ec);
  }
  FILE* log = _wfopen(log_path.wstring().c_str(), L"a");
  if (log == nullptr)
    return;
  _dup2(_fileno(log), _fileno(stdout));
  _dup2(_fileno(log), _fileno(stderr));
  fclose(log);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
}

void ReportStatus(DWORD state, DWORD wait_hint = 0) {
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = NO_ERROR;
  g_status.dwWaitHint = wait_hint;
  g_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
  g_status.dwCheckPoint = 0;
  if (g_status_handle != nullptr)
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, void*, void*) {
  switch (control) {
  case SERVICE_CONTROL_STOP:
  case SERVICE_CONTROL_SHUTDOWN:
    ReportStatus(SERVICE_STOP_PENDING, 15000);
    if (g_hooks.request_stop)
      g_hooks.request_stop();
    return NO_ERROR;
  case SERVICE_CONTROL_INTERROGATE:
    if (g_status_handle != nullptr)
      SetServiceStatus(g_status_handle, &g_status);
    return NO_ERROR;
  default:
    return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI ServiceMainEntry(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(L"UvNVR", ControlHandler, nullptr);
  if (g_status_handle == nullptr)
    return;
  ReportStatus(SERVICE_START_PENDING, 15000);
  RedirectServiceOutput();
  g_exit_code = g_hooks.run ? g_hooks.run() : 1;
  ReportStatus(SERVICE_STOPPED);
}

bool LaunchConsole(const Config& config) {
  const std::string executable = platform::ExecutablePath();
  if (executable.empty())
    return false;
  std::wstring command = QuoteArg(Widen(executable));
  if (!config.config_path.empty())
    command += L" --config " + QuoteArg(Widen(config.config_path));
  std::error_code ec;
  const fs::path data = fs::absolute(config.data_dir, ec);
  const std::string data_text = ec ? config.data_dir : data.string();
  if (!data_text.empty())
    command += L" --data-dir " + QuoteArg(Widen(data_text));
  command += L" --open";
  const std::wstring working_storage = data_text.empty() ? std::wstring() : data.wstring();
  const wchar_t* working_directory = working_storage.empty() ? nullptr : working_storage.c_str();

  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  if (session == 0) {
    const DWORD active_session = WTSGetActiveConsoleSessionId();
    HANDLE user_token = nullptr;
    if (active_session != 0xFFFFFFFF && WTSQueryUserToken(active_session, &user_token)) {
      HANDLE primary_token = nullptr;
      if (DuplicateTokenEx(user_token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                           TokenPrimary, &primary_token)) {
        LPVOID environment = nullptr;
        CreateEnvironmentBlock(&environment, primary_token, FALSE);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION process{};
        std::wstring mutable_command = command;
        const BOOL started =
            CreateProcessAsUserW(primary_token, nullptr, mutable_command.data(), nullptr, nullptr,
                                 FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                                 environment, working_directory, &startup, &process);
        if (started) {
          CloseHandle(process.hThread);
          CloseHandle(process.hProcess);
        }
        if (environment != nullptr)
          DestroyEnvironmentBlock(environment);
        CloseHandle(primary_token);
        CloseHandle(user_token);
        if (started)
          return true;
      } else {
        CloseHandle(user_token);
      }
    }
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring mutable_command = command;
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                      nullptr, working_directory, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void SpawnHelper(const Config& config, const std::string& action) {
  const std::string executable = platform::ExecutablePath();
  if (executable.empty())
    return;
  std::wstring command = QuoteArg(Widen(executable)) + L" --service-action " + Widen(action);
  if (!config.config_path.empty())
    command += L" --config " + QuoteArg(Widen(config.config_path));
  std::error_code ec;
  const fs::path data = fs::absolute(config.data_dir, ec);
  const std::string data_text = ec ? config.data_dir : data.string();
  if (!data_text.empty())
    command += L" --data-dir " + QuoteArg(Widen(data_text));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring mutable_command = command;
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

} // namespace
#endif

void NotifyStarted() {
#ifdef _WIN32
  if (g_status_handle != nullptr)
    ReportStatus(SERVICE_RUNNING);
#endif
}

#ifndef _WIN32
namespace {

constexpr const char* kLinuxUnitName = "uvnvr.service";

struct CommandResult {
  int status = -1;
  std::string output;
};

std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                            value.back() == '\t'))
    value.pop_back();
  size_t start = 0;
  while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
    ++start;
  return value.substr(start);
}

CommandResult RunCommand(const std::vector<std::string>& argv) {
  CommandResult result;
  if (argv.empty())
    return result;
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0)
    return result;
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return result;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0)
      _exit(127);
    if (pipe_fds[1] > STDERR_FILENO)
      close(pipe_fds[1]);
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& item : argv)
      raw.push_back(const_cast<char*>(item.c_str()));
    raw.push_back(nullptr);
    execvp(raw[0], raw.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  char buffer[512];
  ssize_t count = 0;
  while ((count = read(pipe_fds[0], buffer, sizeof(buffer))) > 0)
    result.output.append(buffer, static_cast<size_t>(count));
  close(pipe_fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) == pid)
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return result;
}

bool CommandExists(const std::string& name) {
  if (name.empty())
    return false;
  if (name.find('/') != std::string::npos)
    return access(name.c_str(), X_OK) == 0;
  const char* path = getenv("PATH");
  if (path == nullptr)
    return false;
  std::stringstream stream(path);
  std::string directory;
  while (std::getline(stream, directory, ':')) {
    const std::string candidate = (directory.empty() ? "." : directory) + "/" + name;
    if (access(candidate.c_str(), X_OK) == 0)
      return true;
  }
  return false;
}

std::string QuoteSystemd(const std::string& value) {
  if (value.find_first_of(" \t\n\"'\\") == std::string::npos)
    return value;
  std::string out = "\"";
  for (const char c : value) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::vector<std::string> ConsoleArguments(const Config& config) {
  const std::string executable = platform::ExecutablePath();
  if (executable.empty())
    return {};
  std::vector<std::string> args = {executable};
  if (!config.config_path.empty()) {
    args.push_back("--config");
    args.push_back(config.config_path);
  }
  std::error_code ec;
  const fs::path data = fs::absolute(config.data_dir, ec);
  args.push_back("--data-dir");
  args.push_back(ec ? config.data_dir : data.string());
  args.push_back("--open");
  return args;
}

// Double-fork so the helper is re-parented to init and survives the service
// process it was started from.
void SpawnDetached(std::vector<std::string> args) {
  if (args.empty() || args[0].empty())
    return;
  const pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    setsid();
    const pid_t second = fork();
    if (second < 0)
      _exit(127);
    if (second > 0)
      _exit(0);
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO)
        close(null_fd);
    }
    std::vector<char*> raw;
    raw.reserve(args.size() + 1);
    for (std::string& item : args)
      raw.push_back(item.data());
    raw.push_back(nullptr);
    execv(raw[0], raw.data());
    execvp(raw[0], raw.data());
    _exit(127);
  }
  waitpid(pid, nullptr, 0);
}

void SpawnHelperUnix(const Config& config, const std::string& action) {
  std::vector<std::string> args = ConsoleArguments(config);
  if (args.empty())
    return;
  args.pop_back(); // drop --open; the helper performs the transition itself
  args.insert(args.begin() + 1, {"--service-action", action});
  if (fs::exists("/run/systemd/system") && CommandExists("systemd-run")) {
    std::vector<std::string> run = {"systemd-run",
                                    "--quiet",
                                    "--collect",
                                    "--no-block",
                                    "--unit=uvnvr-" + action + "-" + std::to_string(getpid()),
                                    "--"};
    run.insert(run.end(), args.begin(), args.end());
    if (RunCommand(run).status == 0)
      return;
  }
  SpawnDetached(std::move(args));
}

void LaunchConsoleUnix(const Config& config) {
  std::vector<std::string> args = ConsoleArguments(config);
  if (args.empty())
    return;
  if (fs::exists("/run/systemd/system") && CommandExists("systemd-run")) {
    std::vector<std::string> run = {"systemd-run",
                                    "--quiet",
                                    "--collect",
                                    "--no-block",
                                    "--unit=uvnvr-console-" + std::to_string(getpid()),
                                    "--"};
    run.insert(run.end(), args.begin(), args.end());
    if (RunCommand(run).status == 0)
      return;
  }
  SpawnDetached(std::move(args));
}

} // namespace
#endif

Status Query() {
  Status status;
  status.platform = PlatformName();
#ifdef _WIN32
  status.supported = true;
  status.manager = "Windows Service Control Manager";
  status.detail = kWindowsServiceName;
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) {
    status.detail = LastErrorText(GetLastError());
    return status;
  }
  SC_HANDLE service = OpenServiceW(scm, L"UvNVR", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
  if (service == nullptr) {
    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_DOES_NOT_EXIST)
      status.detail = LastErrorText(error);
    CloseServiceHandle(scm);
    return status;
  }
  status.installed = true;
  SERVICE_STATUS_PROCESS process_status{};
  DWORD needed = 0;
  if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                           reinterpret_cast<LPBYTE>(&process_status), sizeof(process_status),
                           &needed)) {
    status.running = process_status.dwCurrentState == SERVICE_RUNNING ||
                     process_status.dwCurrentState == SERVICE_START_PENDING;
    status.running_as_service =
        status.running && process_status.dwProcessId == GetCurrentProcessId();
  }
  DWORD bytes_needed = 0;
  QueryServiceConfigW(service, nullptr, 0, &bytes_needed);
  if (bytes_needed > 0) {
    std::vector<BYTE> buffer(bytes_needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (QueryServiceConfigW(service, config, bytes_needed, &bytes_needed))
      status.autostart = config->dwStartType == SERVICE_AUTO_START;
  }
  CloseServiceHandle(service);
  CloseServiceHandle(scm);
#elif defined(__APPLE__)
  status.manager = "launchd";
  status.installed = fs::exists("/Library/LaunchDaemons/com.antruly.uvnvr.plist");
  status.detail = status.installed ? "com.antruly.uvnvr" : "not installed";
#else
  status.manager = "systemd";
  status.detail = kLinuxUnitName;
  {
    const bool unit_in_etc = fs::exists("/etc/systemd/system/uvnvr.service");
    const bool unit_in_lib = fs::exists("/lib/systemd/system/uvnvr.service") ||
                             fs::exists("/usr/lib/systemd/system/uvnvr.service");
    status.installed = unit_in_etc || unit_in_lib;
    if (unit_in_etc)
      status.detail = "/etc/systemd/system/uvnvr.service";
    else if (unit_in_lib)
      status.detail = "/lib/systemd/system/uvnvr.service";
  }
  status.supported = fs::exists("/run/systemd/system");
  if (status.supported && status.installed) {
    status.running = RunCommand({"systemctl", "is-active", "--quiet", kLinuxUnitName}).status == 0;
    status.autostart =
        RunCommand({"systemctl", "is-enabled", "--quiet", kLinuxUnitName}).status == 0;
    const std::string main_pid =
        Trim(RunCommand({"systemctl", "show", "-p", "MainPID", "--value", kLinuxUnitName}).output);
    status.running_as_service = main_pid == std::to_string(getpid());
  }
#endif
  return status;
}

nlohmann::json StatusJson() {
  const Status status = Query();
  return nlohmann::json{
      {"platform", status.platform},   {"manager", status.manager},
      {"detail", status.detail},       {"supported", status.supported},
      {"installed", status.installed}, {"running", status.running},
      {"autostart", status.autostart}, {"runningAsService", status.running_as_service}};
}

bool Install(const Config& config, std::string* error) {
#ifdef _WIN32
  const std::string executable = platform::ExecutablePath();
  if (executable.empty()) {
    *error = "cannot resolve the UvNVR executable path";
    return false;
  }
  const std::wstring bin_path = BuildBinPath(config);
  const Status current = Query();
  int result = -1;
  if (current.installed) {
    result = RunSc(L"config UvNVR binPath= " + QuoteArg(bin_path) + L" start= auto");
  } else {
    result = RunSc(L"create UvNVR binPath= " + QuoteArg(bin_path) +
                   L" start= auto obj= LocalSystem DisplayName= \"UvNVR\"");
    if (result == 1073) // ERROR_SERVICE_EXISTS
      result = RunSc(L"config UvNVR binPath= " + QuoteArg(bin_path) + L" start= auto");
  }
  if (result != 0) {
    *error = result == 5 ? "administrator rights are required to register the service"
                         : "sc.exe failed with exit code " + std::to_string(result);
    return false;
  }
  RunSc(L"description UvNVR \"UvNVR software network video recorder\"");
  RunSc(L"failure UvNVR reset= 86400 actions= restart/5000/restart/5000/restart/5000");
  return true;
#elif defined(__APPLE__)
  (void)config;
  *error = "service registration is managed by the macOS package (launchd); "
           "use the installer or launchctl";
  return false;
#else
  if (geteuid() != 0) {
    *error = "root privileges are required to register the systemd service; "
             "run the web UI as root or use sudo";
    return false;
  }
  if (!fs::exists("/run/systemd/system")) {
    *error = "systemd is not running on this host";
    return false;
  }
  const std::string executable = platform::ExecutablePath();
  if (executable.empty()) {
    *error = "cannot resolve the UvNVR executable path";
    return false;
  }
  std::error_code ec;
  const fs::path data = fs::absolute(config.data_dir, ec);
  const std::string data_text = ec ? config.data_dir : data.string();
  fs::create_directories(data_text, ec);
  if (ec) {
    *error = "cannot create the data directory " + data_text + ": " + ec.message();
    return false;
  }

  std::string exec_start = QuoteSystemd(executable) + " --service";
  if (!config.config_path.empty())
    exec_start += " --config " + QuoteSystemd(config.config_path);
  exec_start += " --data-dir " + QuoteSystemd(data_text);

  // The in-app registration keeps the service in the foreground as root,
  // matching the Windows LocalSystem service and leaving the web UI able to
  // manage it later. Distribution packages ship their own unit that runs as
  // the dedicated `uvnvr` user.
  std::ostringstream unit;
  unit << "[Unit]\n"
       << "Description=UvNVR software network video recorder\n"
       << "After=network-online.target\n"
       << "Wants=network-online.target\n\n"
       << "[Service]\n"
       << "Type=simple\n"
       << "ExecStart=" << exec_start << "\n"
       << "WorkingDirectory=" << QuoteSystemd(data_text) << "\n"
       << "AmbientCapabilities=CAP_NET_BIND_SERVICE\n"
       << "CapabilityBoundingSet=CAP_NET_BIND_SERVICE\n"
       << "NoNewPrivileges=true\n"
       << "Restart=on-failure\n"
       << "RestartSec=5\n"
       << "StandardOutput=journal\n"
       << "StandardError=journal\n\n"
       << "[Install]\n"
       << "WantedBy=multi-user.target\n";

  const fs::path unit_path = "/etc/systemd/system/uvnvr.service";
  {
    std::ofstream output(unit_path, std::ios::trunc);
    if (!output) {
      *error = "cannot write " + unit_path.string();
      return false;
    }
    output << unit.str();
    if (!output) {
      *error = "cannot write " + unit_path.string();
      return false;
    }
  }

  const CommandResult reload = RunCommand({"systemctl", "daemon-reload"});
  if (reload.status != 0) {
    *error = "systemctl daemon-reload failed: " + Trim(reload.output);
    return false;
  }
  const CommandResult enable = RunCommand({"systemctl", "enable", "uvnvr.service"});
  if (enable.status != 0) {
    *error = "systemctl enable failed: " + Trim(enable.output);
    return false;
  }
  return true;
#endif
}

bool Uninstall(const Config& config, std::string* error) {
  (void)config;
#ifdef _WIN32
  const Status current = Query();
  if (!current.installed)
    return true;
  if (!current.running_as_service) {
    RunSc(L"stop UvNVR");
    for (int attempt = 0; attempt < 20 && Query().running; ++attempt)
      Sleep(500);
  }
  const int result = RunSc(L"delete UvNVR");
  if (result != 0 && result != 1060 && result != 1072) {
    *error = result == 5 ? "administrator rights are required to remove the service"
                         : "sc.exe failed with exit code " + std::to_string(result);
    return false;
  }
  for (int attempt = 0; attempt < 10 && Query().installed; ++attempt)
    Sleep(300);
  return true;
#elif defined(__APPLE__)
  *error = "service registration is managed by the macOS package (launchd); "
           "use launchctl to remove it";
  return false;
#else
  if (!fs::exists("/run/systemd/system")) {
    *error = "systemd is not running on this host";
    return false;
  }
  if (!Query().running_as_service)
    RunCommand({"systemctl", "stop", "uvnvr.service"});
  RunCommand({"systemctl", "disable", "uvnvr.service"});
  std::error_code ec;
  fs::remove("/etc/systemd/system/uvnvr.service", ec);
  const CommandResult reload = RunCommand({"systemctl", "daemon-reload"});
  if (reload.status != 0) {
    *error = "systemctl daemon-reload failed: " + Trim(reload.output);
    return false;
  }
  return true;
#endif
}

void ScheduleStart(const Config& config) {
  const std::string executable = platform::ExecutablePath();
  if (executable.empty())
    return;
#ifdef _WIN32
  SpawnHelper(config, "start");
#else
  SpawnHelperUnix(config, "start");
#endif
}

void ScheduleConsole(const Config& config) {
  const std::string executable = platform::ExecutablePath();
  if (executable.empty())
    return;
#ifdef _WIN32
  SpawnHelper(config, "console");
#else
  SpawnHelperUnix(config, "console");
#endif
}

void PerformAction(const std::string& action, const Config& config) {
  std::this_thread::sleep_for(std::chrono::milliseconds(kTransitionDelayMs));
#ifdef _WIN32
  if (action == "start") {
    RunSc(L"stop UvNVR");
    Sleep(1200);
    for (int attempt = 0; attempt < 6; ++attempt) {
      if (RunSc(L"start UvNVR") == 0)
        break;
      Sleep(1200);
    }
  } else if (action == "console") {
    LaunchConsole(config);
  }
#else
  if (action == "start") {
    if (RunCommand({"systemctl", "restart", "uvnvr.service"}).status != 0)
      RunCommand({"systemctl", "start", "uvnvr.service"});
  } else if (action == "console") {
    LaunchConsoleUnix(config);
  }
#endif
}

#ifdef _WIN32
int RunWindowsService(const Config& config, const ServiceHooks& hooks) {
  g_config = config;
  g_hooks = hooks;
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L"UvNVR"), ServiceMainEntry},
                                  {nullptr, nullptr}};
  if (!StartServiceCtrlDispatcherW(table)) {
    std::fprintf(stderr, "UvNVR was not started by the Service Control Manager\n");
    return 1;
  }
  return g_exit_code;
}
#endif

} // namespace uvnvr::service
