#include "exporter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace uvnvr {
namespace {

std::string FormatSeconds(int64_t milliseconds) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3)
      << static_cast<double>(std::max<int64_t>(0, milliseconds)) / 1000.0;
  return out.str();
}

std::string LogTail(const std::string& path, size_t limit = 4096) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0)
    return {};
  const std::streamoff start = size > static_cast<std::streamoff>(limit) ? size - limit : 0;
  input.seekg(start);
  std::string text(static_cast<size_t>(size - start), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(input.gcount()));
  return text;
}

#ifdef _WIN32
std::string QuoteArg(const std::string& argument) {
  if (argument.find_first_of(" \t\"") == std::string::npos)
    return argument;
  std::string quoted = "\"";
  for (const char c : argument) {
    if (c == '"')
      quoted += "\\\"";
    else
      quoted += c;
  }
  quoted += '"';
  return quoted;
}

bool RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
                const std::string& log_path, int timeout_ms, std::string* error) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE log = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) {
    if (error != nullptr)
      *error = "cannot create the exporter log";
    return false;
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = log;
  startup.hStdError = log;
  PROCESS_INFORMATION process{};
  std::string command = QuoteArg(executable);
  for (const std::string& argument : arguments)
    command += " " + QuoteArg(argument);
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(log);
  if (!started) {
    if (error != nullptr)
      *error = "cannot start ffmpeg";
    return false;
  }
  bool ok = false;
  if (WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
    if (error != nullptr)
      *error = "ffmpeg export timed out";
  } else {
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (code == 0) {
      ok = true;
    } else if (error != nullptr) {
      std::ostringstream message;
      message << "ffmpeg exited with code " << code;
      const std::string tail = LogTail(log_path);
      if (!tail.empty())
        message << ": " << tail;
      *error = message.str();
    }
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return ok;
}
#else
bool RunProcess(const std::string& executable, const std::vector<std::string>& arguments,
                const std::string& log_path, int timeout_ms, std::string* error) {
  const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (log < 0) {
    if (error != nullptr)
      *error = "cannot create the exporter log";
    return false;
  }
  const int null_fd = open("/dev/null", O_RDONLY);
  const pid_t pid = fork();
  if (pid < 0) {
    close(log);
    if (null_fd >= 0)
      close(null_fd);
    if (error != nullptr)
      *error = "fork failed";
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    if (null_fd >= 0)
      dup2(null_fd, STDIN_FILENO);
    dup2(log, STDOUT_FILENO);
    dup2(log, STDERR_FILENO);
    close(log);
    if (null_fd >= 0)
      close(null_fd);
    std::vector<std::string> full;
    full.push_back(executable);
    full.insert(full.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(full.size() + 1);
    for (std::string& argument : full)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(log);
  if (null_fd >= 0)
    close(null_fd);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;
      if (error != nullptr) {
        std::ostringstream message;
        if (WIFEXITED(status))
          message << "ffmpeg exited with code " << WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
          message << "ffmpeg was killed by signal " << WTERMSIG(status);
        else
          message << "ffmpeg failed";
        const std::string tail = LogTail(log_path);
        if (!tail.empty())
          message << ": " << tail;
        *error = message.str();
      }
      return false;
    }
    if (result < 0)
      break;
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      if (error != nullptr)
        *error = "ffmpeg export timed out";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (error != nullptr) {
    const std::string tail = LogTail(log_path);
    *error = tail.empty() ? "ffmpeg failed" : "ffmpeg failed: " + tail;
  }
  return false;
}
#endif

} // namespace

bool ExportMp4(const ExportJob& job, std::string* error) {
  if (job.segments.empty()) {
    if (error != nullptr)
      *error = "no segments selected";
    return false;
  }
  if (job.end_ms <= job.start_ms) {
    if (error != nullptr)
      *error = "the export range is empty";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(job.work_dir, ec);
  if (ec) {
    if (error != nullptr)
      *error = "cannot create the export directory: " + ec.message();
    return false;
  }
  const std::string concat_path = (std::filesystem::path(job.work_dir) / "concat.ts").string();
  const std::string log_path = (std::filesystem::path(job.work_dir) / "ffmpeg.log").string();

  {
    std::ofstream concat(concat_path, std::ios::binary | std::ios::trunc);
    if (!concat) {
      if (error != nullptr)
        *error = "cannot create the concatenated segment file";
      return false;
    }
    std::vector<char> buffer(1024 * 1024);
    uint64_t total = 0;
    for (const std::string& path : job.segments) {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        if (error != nullptr)
          *error = "cannot open segment " + path;
        std::filesystem::remove(concat_path, ec);
        return false;
      }
      while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got <= 0)
          break;
        total += static_cast<uint64_t>(got);
        if (total > job.max_input_bytes) {
          if (error != nullptr)
            *error = "the export range is too large";
          std::filesystem::remove(concat_path, ec);
          return false;
        }
        concat.write(buffer.data(), got);
      }
    }
    concat.flush();
    if (!concat) {
      if (error != nullptr)
        *error = "cannot write the concatenated segment file";
      std::filesystem::remove(concat_path, ec);
      return false;
    }
  }

  const int64_t offset_ms = std::max<int64_t>(0, job.start_ms - job.first_segment_start_ms);
  const int64_t duration_ms = job.end_ms - job.start_ms;
  std::vector<std::string> arguments = {"-hide_banner", "-loglevel", "error", "-nostdin", "-y"};
  if (offset_ms > 0) {
    arguments.push_back("-ss");
    arguments.push_back(FormatSeconds(offset_ms));
  }
  arguments.push_back("-i");
  arguments.push_back(concat_path);
  if (duration_ms > 0) {
    arguments.push_back("-t");
    arguments.push_back(FormatSeconds(duration_ms));
  }
  for (const char* argument :
       {"-map", "0:v:0?", "-map", "0:a:0?", "-c", "copy", "-movflags", "+faststart", "-f", "mp4"}) {
    arguments.push_back(argument);
  }
  arguments.push_back(job.output_path);

  const bool ok = RunProcess(job.ffmpeg_path, arguments, log_path, 10 * 60 * 1000, error);
  std::filesystem::remove(concat_path, ec);
  if (!ok)
    return false;
  std::error_code size_error;
  const uintmax_t output_size = std::filesystem::file_size(job.output_path, size_error);
  if (size_error || output_size == 0) {
    if (error != nullptr)
      *error = "ffmpeg produced no output";
    return false;
  }
  // An empty MP4 (for example when the requested range has no keyframe) is a
  // valid container with a `moov` box but no `trak`. It is not a usable export.
  {
    std::ifstream output(job.output_path, std::ios::binary);
    std::string head(1024 * 1024, '\0');
    if (output) {
      output.read(head.data(), static_cast<std::streamsize>(head.size()));
      head.resize(static_cast<size_t>(output.gcount()));
    }
    if (head.find("trak") == std::string::npos) {
      if (error != nullptr)
        *error = "the selected range contains no exportable video";
      std::filesystem::remove(job.output_path, size_error);
      return false;
    }
  }
  return true;
}

} // namespace uvnvr
