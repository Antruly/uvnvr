#include "media.h"

#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#endif

namespace uvnvr {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string RedactUrl(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos)
    return url;
  const size_t at = url.find('@', scheme + 3);
  if (at == std::string::npos)
    return url;
  return url.substr(0, scheme + 3) + "***@" + url.substr(at + 1);
}

#ifdef _WIN32
std::string QuoteWindowsArg(const std::string& value) {
  if (value.find_first_of(" \t\"") == std::string::npos)
    return value;
  std::string out = "\"";
  for (char c : value) {
    if (c == '"')
      out += "\\\"";
    else
      out += c;
  }
  out += '"';
  return out;
}
#endif

std::string ResolveFfmpeg() {
  if (const char* env = std::getenv("UVNVR_FFMPEG"); env != nullptr && env[0] != '\0') {
    return env;
  }
  const std::string exe_path = platform::ExecutablePath();
  if (!exe_path.empty()) {
    const std::filesystem::path dir = std::filesystem::path(exe_path).parent_path();
#ifdef _WIN32
    const std::vector<std::string> names = {"ffmpeg.exe", "ffmpeg"};
#else
    const std::vector<std::string> names = {"ffmpeg", "ffmpeg.exe"};
#endif
    const std::vector<std::filesystem::path> dirs = {
        dir,
        dir / ".." / "bin",
        dir / ".." / "lib" / "uvnvr",
        dir / ".." / "libexec",
    };
    for (const auto& base : dirs) {
      for (const auto& name : names) {
        std::error_code ec;
        const auto candidate = (base / name).lexically_normal();
        if (std::filesystem::exists(candidate, ec) && !ec)
          return candidate.string();
      }
    }
  }
  return "ffmpeg";
}

int64_t NoiseSleep(std::atomic<bool>& stop, int64_t ms) {
  const int64_t step = 100;
  int64_t slept = 0;
  while (!stop.load() && slept < ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(step, ms - slept)));
    slept += step;
  }
  return slept;
}

#ifndef _WIN32
bool CapturePosix(const std::string& executable, const std::vector<std::string>& args,
                  std::string* output, std::string* error) {
  int fds[2];
  if (pipe(fds) != 0) {
    if (error)
      *error = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    if (error)
      *error = "fork failed";
    return false;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0)
      dup2(null_fd, STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execv(executable.c_str(), argv.data());
    _exit(127);
  }
  close(fds[1]);
  output->clear();
  char buffer[16384];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd pfd {
      fds[0], POLLIN, 0
    };
    const int ready = poll(&pfd, 1, 200);
    if (ready < 0)
      break;
    if (ready == 0)
      continue;
    const ssize_t n = read(fds[0], buffer, sizeof(buffer));
    if (n <= 0)
      break;
    output->append(buffer, static_cast<size_t>(n));
    if (output->size() > 16 * 1024 * 1024)
      break;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (output->empty()) {
    if (error)
      *error = "ffmpeg capture returned no data";
    return false;
  }
  return true;
}
#else
bool CaptureWindows(const std::string& executable, const std::vector<std::string>& args,
                    std::string* output, std::string* error) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
    if (error)
      *error = "cannot create capture pipe";
    return false;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_out =
      CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
  const bool has_null = null_out != nullptr && null_out != INVALID_HANDLE_VALUE;

  std::string command = QuoteWindowsArg(executable);
  for (const auto& arg : args)
    command += " " + QuoteWindowsArg(arg);
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_end;
  startup.hStdError = has_null ? null_out : GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL started = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write_end);
  if (has_null)
    CloseHandle(null_out);
  if (!started) {
    CloseHandle(read_end);
    if (error)
      *error = "cannot start ffmpeg (CreateProcess failed)";
    return false;
  }

  output->clear();
  const DWORD deadline = GetTickCount() + 10000;
  char buffer[16384];
  while (GetTickCount() < deadline) {
    DWORD available = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr))
      break;
    if (available == 0) {
      if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) {
        DWORD read = 0;
        while (ReadFile(read_end, buffer, sizeof(buffer), &read, nullptr) && read > 0)
          output->append(buffer, read);
        break;
      }
      continue;
    }
    DWORD read = 0;
    if (!ReadFile(read_end, buffer, sizeof(buffer), &read, nullptr) || read == 0)
      break;
    output->append(buffer, read);
    if (output->size() > 16 * 1024 * 1024)
      break;
  }
  if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
    TerminateProcess(process.hProcess, 1);
  WaitForSingleObject(process.hProcess, 2000);
  CloseHandle(read_end);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (output->empty()) {
    if (error)
      *error = "ffmpeg capture returned no data";
    return false;
  }
  return true;
}
#endif

} // namespace

struct MediaService::Impl {
  struct Stream;

  struct Chunk {
    uint64_t start = 0;
    std::shared_ptr<std::vector<uint8_t>> bytes;
  };

  struct Subscriber {
    uint64_t id = 0;
    std::atomic<uvcpp::uvcpp_ws_connection*> connection{nullptr};
    std::atomic<bool> alive{true};
    std::atomic<bool> pending{false};
    std::atomic<bool> skipping{false};
  };

  struct RawSubscriber {
    uint64_t id = 0;
    RawSink sink;
  };

  struct TSDemux {
    static constexpr size_t kCapacity = 8 * 1024 * 1024;
    std::deque<Chunk> chunks;
    uint64_t first_offset = 0;
    uint64_t total_offset = 0;
    size_t buffered = 0;
    uint64_t keyframe_offset = UINT64_MAX;
    std::vector<uint8_t> pat;
    std::vector<uint8_t> pmt;
    std::vector<uint8_t> pending;
    int pmt_pid = -1;

    void Trim() {
      while (buffered > kCapacity && chunks.size() > 1) {
        const Chunk& front = chunks.front();
        buffered -= front.bytes->size();
        first_offset = front.start + front.bytes->size();
        chunks.pop_front();
      }
      if (keyframe_offset != UINT64_MAX && keyframe_offset < first_offset) {
        keyframe_offset = UINT64_MAX;
      }
    }

    void ParseTs(const uint8_t* data, size_t len) {
      pending.insert(pending.end(), data, data + len);
      size_t offset = 0;
      while (pending.size() - offset >= 188) {
        const uint8_t* p = pending.data() + offset;
        if (p[0] != 0x47) {
          ++offset;
          continue;
        }
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        const bool pusi = (p[1] & 0x40) != 0;
        const int adaptation = (p[3] >> 4) & 0x03;
        size_t payload = 4;
        if (adaptation & 0x02) {
          if (payload >= 188)
            break;
          payload += static_cast<size_t>(p[4]) + 1;
        }
        if ((adaptation & 0x01) == 0 || payload >= 188) {
          offset += 188;
          continue;
        }
        if (pid == 0 && pusi) {
          const size_t pointer = p[payload];
          const size_t section = payload + 1 + pointer;
          if (section + 12 <= 188 && p[section] == 0x00) {
            const int pmt = ((p[section + 10] & 0x1f) << 8) | p[section + 11];
            if (pmt > 0 && pmt < 0x1fff)
              pmt_pid = pmt;
            pat.assign(p, p + 188);
          }
        } else if (pid == pmt_pid && pusi) {
          pmt.assign(p, p + 188);
        }
        offset += 188;
      }
      if (offset > 0)
        pending.erase(pending.begin(), pending.begin() + offset);
      if (pending.size() > 4096)
        pending.clear();
    }

    std::vector<size_t> ScanKeyframes(uint64_t chunk_start, const uint8_t* data, size_t len) {
      std::vector<size_t> offsets;
      for (size_t i = 0; i + 4 <= len; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
          continue;
        const uint8_t nal = data[i + 3];
        const int h264_type = nal & 0x1f;
        const int h265_type = (nal >> 1) & 0x3f;
        if (h264_type != 5 && h265_type != 19 && h265_type != 20)
          continue;
        const uint64_t absolute = chunk_start + i - 3;
        const uint64_t aligned = (absolute / 188) * 188;
        if (aligned >= chunk_start && aligned - chunk_start < len) {
          offsets.push_back(static_cast<size_t>(aligned - chunk_start));
        }
        keyframe_offset = aligned;
      }
      return offsets;
    }

    void Ingest(const uint8_t* data, size_t len, std::vector<size_t>* keyframes,
                uint64_t* chunk_start) {
      *chunk_start = total_offset;
      ParseTs(data, len);
      auto bytes = std::make_shared<std::vector<uint8_t>>(data, data + len);
      Chunk chunk;
      chunk.start = total_offset;
      chunk.bytes = bytes;
      chunks.push_back(chunk);
      buffered += len;
      total_offset += len;
      *keyframes = ScanKeyframes(chunk.start, data, len);
      Trim();
    }

    std::vector<uint8_t> Snapshot() {
      std::vector<uint8_t> out;
      uint64_t start = keyframe_offset != UINT64_MAX ? keyframe_offset : first_offset;
      start = (start / 188) * 188;
      if (start < first_offset)
        start = first_offset;
      if (total_offset - start > 4 * 1024 * 1024)
        start = total_offset - 4 * 1024 * 1024;
      start = (start / 188) * 188;
      out.reserve(pat.size() + pmt.size() + static_cast<size_t>(total_offset - start));
      out.insert(out.end(), pat.begin(), pat.end());
      out.insert(out.end(), pmt.begin(), pmt.end());
      for (const auto& chunk : chunks) {
        const uint64_t end = chunk.start + chunk.bytes->size();
        if (end <= start)
          continue;
        const size_t from = start > chunk.start ? static_cast<size_t>(start - chunk.start) : 0;
        out.insert(out.end(), chunk.bytes->begin() + from, chunk.bytes->end());
      }
      return out;
    }
  };

  struct Stream : std::enable_shared_from_this<Stream> {
    Impl* owner = nullptr;
    int64_t channel_id = 0;
    std::string kind;
    std::string url;
    std::atomic<bool> stopping{false};
    std::atomic<int64_t> child_pid{-1};
    std::thread thread;
    StreamStatus status;
    int backoff_index = 0;
    int64_t bytes = 0;
    std::string stderr_tail;

    mutable std::mutex mutex;
    TSDemux demux;
    std::unordered_map<uint64_t, std::shared_ptr<Subscriber>> subscribers;
    std::unordered_map<uint64_t, std::shared_ptr<RawSubscriber>> raw_subscribers;
    uint64_t next_subscriber_id = 1;
    uint64_t next_raw_subscriber_id = 1;

    Stream(Impl* owner_in, int64_t channel, std::string stream_kind, std::string stream_url)
        : owner(owner_in), channel_id(channel), kind(std::move(stream_kind)),
          url(std::move(stream_url)) {
      status.state = "connecting";
    }

    ~Stream() {
      Stop();
    }

    void Publish(const StreamStatus& value) {
      status = value;
      owner->PublishStatus(channel_id, kind, status);
    }

    void SetState(const std::string& state, const std::string& error = {}) {
      StreamStatus value = status;
      value.state = state;
      value.error = error;
      value.bytes = bytes;
      value.reconnects = status.reconnects;
      value.has_keyframe = demux.keyframe_offset != UINT64_MAX;
      value.last_data_at_ms = status.last_data_at_ms;
      Publish(value);
    }

    void PostChunk(const std::shared_ptr<Subscriber>& sub, const uint8_t* data, size_t len) {
      auto copy = std::make_shared<std::vector<uint8_t>>(data, data + len);
      owner->web->post([sub, copy] {
        if (!sub->alive.load())
          return;
        auto* connection = sub->connection.load();
        if (connection == nullptr)
          return;
        sub->pending = true;
        const int rc = connection->send_binary(reinterpret_cast<const char*>(copy->data()),
                                               copy->size(), [sub](int) { sub->pending = false; });
        if (rc != 0) {
          sub->alive = false;
          connection->close();
        }
      });
    }

    void SendInitial(const std::shared_ptr<Subscriber>& sub, const std::vector<uint8_t>& data) {
      if (data.empty() || !sub->alive.load())
        return;
      auto* connection = sub->connection.load();
      if (connection == nullptr)
        return;
      sub->pending = true;
      auto copy = std::make_shared<std::vector<uint8_t>>(data);
      const int rc = connection->send_binary(reinterpret_cast<const char*>(copy->data()),
                                             copy->size(), [sub](int) { sub->pending = false; });
      if (rc != 0) {
        sub->alive = false;
        connection->close();
      }
    }

    void Ingest(const uint8_t* data, size_t len) {
      std::vector<size_t> keyframes;
      uint64_t chunk_start = 0;
      std::vector<std::shared_ptr<Subscriber>> subscriber_list;
      std::vector<std::shared_ptr<RawSubscriber>> raw_list;
      std::vector<uint8_t> initial;
      {
        std::lock_guard<std::mutex> lock(mutex);
        demux.Ingest(data, len, &keyframes, &chunk_start);
        subscriber_list.reserve(subscribers.size());
        for (const auto& [id, sub] : subscribers) {
          (void)id;
          subscriber_list.push_back(sub);
        }
        for (const auto& [id, sub] : raw_subscribers) {
          (void)id;
          raw_list.push_back(sub);
        }
        status.has_keyframe = demux.keyframe_offset != UINT64_MAX;
      }
      for (const auto& sub : raw_list) {
        if (sub->sink)
          sub->sink(data, len);
      }
      for (const auto& sub : subscriber_list) {
        if (!sub->alive.load())
          continue;
        const bool pending = sub->pending.load();
        if (pending) {
          sub->skipping = true;
          if (!keyframes.empty()) {
            sub->skipping = false;
            PostChunk(sub, data + keyframes.front(), len - keyframes.front());
          }
          continue;
        }
        if (sub->skipping.load()) {
          if (keyframes.empty())
            continue;
          sub->skipping = false;
          PostChunk(sub, data + keyframes.front(), len - keyframes.front());
        } else {
          PostChunk(sub, data, len);
        }
      }
      (void)chunk_start;
      (void)initial;
    }

    bool AddSubscriber(uvcpp::uvcpp_ws_connection* connection, std::string* error) {
      if (connection == nullptr) {
        if (error)
          *error = "websocket connection is not ready";
        return false;
      }
      auto sub = std::make_shared<Subscriber>();
      sub->connection = connection;
      std::vector<uint8_t> initial;
      {
        std::lock_guard<std::mutex> lock(mutex);
        sub->id = next_subscriber_id++;
        subscribers[sub->id] = sub;
        initial = demux.Snapshot();
      }
      const uint64_t id = sub->id;
      std::weak_ptr<Stream> weak = shared_from_this();
      connection->on_close([weak, id](uvcpp::ws_close_code, const std::string&) {
        if (auto self = weak.lock()) {
          std::lock_guard<std::mutex> lock(self->mutex);
          auto it = self->subscribers.find(id);
          if (it != self->subscribers.end()) {
            it->second->alive = false;
            it->second->connection = nullptr;
            self->subscribers.erase(it);
          }
        }
      });
      SendInitial(sub, initial);
      return true;
    }

    uint64_t AddRawSubscriber(RawSink sink, std::vector<uint8_t>* initial) {
      auto sub = std::make_shared<RawSubscriber>();
      sub->sink = std::move(sink);
      std::lock_guard<std::mutex> lock(mutex);
      sub->id = next_raw_subscriber_id++;
      raw_subscribers[sub->id] = sub;
      *initial = demux.Snapshot();
      return sub->id;
    }

    void RemoveRawSubscriber(uint64_t id) {
      std::lock_guard<std::mutex> lock(mutex);
      raw_subscribers.erase(id);
    }

    void Start() {
      if (thread.joinable())
        return;
      stopping = false;
      thread = std::thread([this] { Run(); });
    }

    void Stop() {
      stopping = true;
      const int64_t pid = child_pid.load();
      if (pid > 0) {
#ifdef _WIN32
        // The child is killed by the job object when the service exits; an
        // explicit TerminateProcess here is handled by the process wrapper.
#else
        kill(static_cast<pid_t>(-pid), SIGTERM);
#endif
      }
      if (thread.joinable())
        thread.join();
    }

    void Run();
    void OnDataRaw(const uint8_t* data, size_t len);
    void OnErrorRaw(const uint8_t* data, size_t len);

#ifdef _WIN32
    bool SpawnWindows(HANDLE* out_read, HANDLE* err_read, PROCESS_INFORMATION* pi,
                      std::string* error);
#else
    bool SpawnPosix(int* out_fd, int* err_fd, pid_t* pid, std::string* error);
#endif
  };

  struct Channel {
    ChannelSpec spec;
    std::shared_ptr<Stream> main;
    std::shared_ptr<Stream> sub;
  };

  Impl(uvcpp::uvcpp_web_app* web_in) : web(web_in) {}
  ~Impl() {
    StopAll();
  }

  uvcpp::uvcpp_web_app* web;
  std::string ffmpeg_path;
  std::atomic<bool> gpu_enabled{false};
  bool started = false;
  mutable std::mutex mutex;
  std::unordered_map<int64_t, std::shared_ptr<Channel>> channels;
  mutable std::mutex status_mutex;
  std::unordered_map<std::string, StreamStatus> statuses;
  std::function<void(int64_t, const std::string&, const uint8_t*, size_t)> data_sink;

  static std::string Key(int64_t channel_id, const std::string& kind) {
    return std::to_string(channel_id) + ":" + kind;
  }

  void PublishStatus(int64_t channel_id, const std::string& kind, const StreamStatus& status) {
    std::lock_guard<std::mutex> lock(status_mutex);
    statuses[Key(channel_id, kind)] = status;
  }

  void StopAll() {
    std::vector<std::shared_ptr<Channel>> copy;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& [id, channel] : channels) {
        (void)id;
        copy.push_back(channel);
      }
      channels.clear();
    }
    for (auto& channel : copy) {
      if (channel->main)
        channel->main->Stop();
      if (channel->sub)
        channel->sub->Stop();
    }
    started = false;
  }

  std::shared_ptr<Stream> FindStream(int64_t channel_id, const std::string& kind) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = channels.find(channel_id);
    if (it == channels.end())
      return nullptr;
    return kind == "sub" ? it->second->sub : it->second->main;
  }
};

void MediaService::Impl::Stream::Run() {
  static const int kDelays[] = {1000, 2000, 5000, 10000, 30000};
  while (!stopping.load()) {
    std::string error;
    int out_fd = -1;
    int err_fd = -1;
#ifdef _WIN32
    HANDLE out_read = nullptr;
    HANDLE err_read = nullptr;
    PROCESS_INFORMATION process_info{};
    if (!SpawnWindows(&out_read, &err_read, &process_info, &error)) {
      SetState("error", error);
    } else {
      child_pid = static_cast<int64_t>(process_info.dwProcessId);
      SetState("connecting");
      char buffer[65536];
      while (!stopping.load()) {
        DWORD available = 0;
        bool read_any = false;
        if (out_read != nullptr &&
            PeekNamedPipe(out_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
          DWORD got = 0;
          if (ReadFile(out_read, buffer,
                       static_cast<DWORD>(std::min<size_t>(available, sizeof(buffer))), &got,
                       nullptr) &&
              got > 0) {
            OnDataRaw(reinterpret_cast<const uint8_t*>(buffer), got);
            read_any = true;
          }
        }
        if (err_read != nullptr &&
            PeekNamedPipe(err_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
          DWORD got = 0;
          if (ReadFile(err_read, buffer,
                       static_cast<DWORD>(std::min<size_t>(available, sizeof(buffer))), &got,
                       nullptr) &&
              got > 0) {
            OnErrorRaw(reinterpret_cast<const uint8_t*>(buffer), got);
            read_any = true;
          }
        }
        if (!read_any) {
          const DWORD wait = WaitForSingleObject(process_info.hProcess, 100);
          if (wait == WAIT_OBJECT_0)
            break;
        }
      }
      if (out_read)
        CloseHandle(out_read);
      if (err_read)
        CloseHandle(err_read);
      TerminateProcess(process_info.hProcess, 0);
      WaitForSingleObject(process_info.hProcess, 2000);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      child_pid = -1;
      SetState(stopping.load() ? "disabled" : "offline", error);
    }
#else
    pid_t pid = -1;
    if (!SpawnPosix(&out_fd, &err_fd, &pid, &error)) {
      SetState("error", error);
    } else {
      child_pid = pid;
      SetState("connecting");
      struct pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
      char buffer[65536];
      while (!stopping.load()) {
        const int ready = poll(fds, 2, 200);
        if (ready < 0) {
          if (errno == EINTR)
            continue;
          error = std::strerror(errno);
          break;
        }
        bool got_data = false;
        for (auto& fd : fds) {
          if (fd.fd < 0 || (fd.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;
          const ssize_t n = read(fd.fd, buffer, sizeof(buffer));
          if (n > 0) {
            if (fd.fd == out_fd) {
              OnDataRaw(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(n));
            } else {
              OnErrorRaw(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(n));
            }
            got_data = true;
          } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            fd.fd = -1;
          }
        }
        if (!got_data) {
          int status = 0;
          const pid_t result = waitpid(pid, &status, WNOHANG);
          if (result == pid)
            break;
        }
      }
      if (out_fd >= 0)
        close(out_fd);
      if (err_fd >= 0)
        close(err_fd);
      kill(-pid, SIGTERM);
      for (int i = 0; i < 20; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      kill(-pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      child_pid = -1;
      if (!stopping.load()) {
        SetState("offline", "ffmpeg exited");
      } else {
        SetState("disabled");
      }
    }
#endif
    if (stopping.load())
      break;
    const int delay = kDelays[std::min<int>(backoff_index, 4)];
    if (backoff_index < 4)
      ++backoff_index;
    status.reconnects += 1;
    SetState("reconnecting", status.error);
    NoiseSleep(stopping, delay);
  }
  SetState("disabled");
}

#ifdef _WIN32
bool MediaService::Impl::Stream::SpawnWindows(HANDLE* out_read, HANDLE* err_read,
                                              PROCESS_INFORMATION* pi, std::string* error) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE out_write = nullptr;
  HANDLE err_write = nullptr;
  if (!CreatePipe(out_read, &out_write, &attributes, 0) ||
      !CreatePipe(err_read, &err_write, &attributes, 0)) {
    if (error)
      *error = "cannot create process pipes";
    return false;
  }
  SetHandleInformation(*out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(*err_read, HANDLE_FLAG_INHERIT, 0);

  std::string command =
      "\"" + owner->ffmpeg_path +
      "\" -hide_banner -loglevel warning -nostdin "
      "-fflags nobuffer -flags low_delay -rtsp_transport tcp -timeout 10000000 -i \"" +
      url +
      "\" -map 0:v:0? -map 0:a:0? -c copy -mpegts_flags +resend_headers "
      "-muxdelay 0 -muxpreload 0 -flush_packets 1 -f mpegts pipe:1";

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, pi);
  CloseHandle(out_write);
  CloseHandle(err_write);
  if (!ok) {
    if (error)
      *error = "cannot start ffmpeg (CreateProcess failed)";
    return false;
  }
  return true;
}
#else
bool MediaService::Impl::Stream::SpawnPosix(int* out_fd, int* err_fd, pid_t* pid,
                                            std::string* error) {
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    if (error)
      *error = std::string("cannot create pipes: ") + std::strerror(errno);
    return false;
  }
  const pid_t child = fork();
  if (child < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (error)
      *error = std::string("fork failed: ") + std::strerror(errno);
    return false;
  }
  if (child == 0) {
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    std::vector<std::string> args = {
        owner->ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "warning",
        "-nostdin",
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-rtsp_transport",
        "tcp",
        "-timeout",
        "10000000",
        "-i",
        url,
        "-map",
        "0:v:0?",
        "-map",
        "0:a:0?",
        "-c",
        "copy",
        "-mpegts_flags",
        "+resend_headers",
        "-muxdelay",
        "0",
        "-muxpreload",
        "0",
        "-flush_packets",
        "1",
        "-f",
        "mpegts",
        "pipe:1",
    };
    std::vector<char*> argv;
    for (auto& arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  *out_fd = out_pipe[0];
  *err_fd = err_pipe[0];
  *pid = child;
  return true;
}
#endif

void MediaService::Impl::Stream::OnDataRaw(const uint8_t* data, size_t len) {
  if (len == 0)
    return;
  bytes += static_cast<int64_t>(len);
  status.bytes = bytes;
  status.last_data_at_ms = NowMs();
  status.state = "online";
  status.error.clear();
  backoff_index = 0;
  Ingest(data, len);
  status.has_keyframe = demux.keyframe_offset != UINT64_MAX;
  owner->PublishStatus(channel_id, kind, status);
  if (owner->data_sink)
    owner->data_sink(channel_id, kind, data, len);
}

void MediaService::Impl::Stream::OnErrorRaw(const uint8_t* data, size_t len) {
  stderr_tail.append(reinterpret_cast<const char*>(data), len);
  if (stderr_tail.size() > 8192)
    stderr_tail.erase(0, stderr_tail.size() - 8192);
  const std::string redacted = RedactUrl(stderr_tail);
  std::fprintf(stderr, "[media %lld/%s] %s", static_cast<long long>(channel_id), kind.c_str(),
               redacted.c_str());
  if (!redacted.empty() && redacted.back() != '\n')
    std::fputc('\n', stderr);
}

MediaService::MediaService(uvcpp::uvcpp_web_app* web) : impl_(std::make_unique<Impl>(web)) {
  impl_->ffmpeg_path = ResolveFfmpeg();
}

MediaService::~MediaService() = default;

void MediaService::SetFfmpegPath(const std::string& path) {
  if (!path.empty())
    impl_->ffmpeg_path = path;
}

void MediaService::SetGpuEnabled(bool enabled) {
  impl_->gpu_enabled = enabled;
}

std::string MediaService::FfmpegPath() const {
  return impl_->ffmpeg_path;
}

bool MediaService::Available() const {
  if (impl_->ffmpeg_path.empty())
    return false;
  const bool path_like = impl_->ffmpeg_path.find('/') != std::string::npos ||
                         impl_->ffmpeg_path.find('\\') != std::string::npos;
  return path_like ? std::filesystem::exists(impl_->ffmpeg_path) : true;
}

void MediaService::Start() {
  impl_->started = true;
}

void MediaService::Stop() {
  impl_->StopAll();
}

void MediaService::Upsert(const ChannelSpec& spec) {
  std::shared_ptr<Impl::Channel> channel;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->channels.find(spec.id);
    if (it == impl_->channels.end()) {
      channel = std::make_shared<Impl::Channel>();
      impl_->channels.emplace(spec.id, channel);
      it = impl_->channels.find(spec.id);
    }
    channel = it->second;
  }
  if (!spec.enabled) {
    if (channel) {
      if (channel->main)
        channel->main->Stop();
      if (channel->sub)
        channel->sub->Stop();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->channels.erase(spec.id);
    return;
  }
  const bool main_changed = channel->spec.main_url != spec.main_url || !channel->main;
  const bool sub_changed = channel->spec.sub_url != spec.sub_url ||
                           channel->spec.sub_enabled != spec.sub_enabled || !channel->sub;
  channel->spec = spec;
  if (main_changed) {
    if (channel->main)
      channel->main->Stop();
    channel->main = std::make_shared<Impl::Stream>(impl_.get(), spec.id, "main", spec.main_url);
    channel->main->Start();
  }
  if (sub_changed) {
    if (channel->sub)
      channel->sub->Stop();
    if (spec.sub_enabled && !spec.sub_url.empty()) {
      channel->sub = std::make_shared<Impl::Stream>(impl_.get(), spec.id, "sub", spec.sub_url);
      channel->sub->Start();
    } else {
      channel->sub.reset();
    }
  }
  impl_->started = true;
}

void MediaService::Remove(int64_t channel_id) {
  std::shared_ptr<Impl::Channel> channel;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->channels.find(channel_id);
    if (it == impl_->channels.end())
      return;
    channel = it->second;
    impl_->channels.erase(it);
  }
  if (channel->main)
    channel->main->Stop();
  if (channel->sub)
    channel->sub->Stop();
}

bool MediaService::CaptureJpeg(const std::string& url, std::string* jpeg, std::string* error,
                               int width, int height, int quality) {
  const int scale_width = width > 0 ? width : -2;
  const int scale_height = height > 0 ? height : -2;
  std::vector<std::string> args = {
      "-hide_banner", "-loglevel", "error", "-nostdin", "-rtsp_transport", "tcp",
      "-timeout",     "10000000",  "-i",    url,        "-frames:v",       "1"};
  if (width > 0 || height > 0) {
    args.push_back("-vf");
    args.push_back("scale=" + std::to_string(scale_width) + ":" + std::to_string(scale_height));
  }
  args.push_back("-q:v");
  args.push_back(std::to_string(quality));
  args.push_back("-f");
  args.push_back("mjpeg");
  args.push_back("pipe:1");
  const auto run = [&](const std::vector<std::string>& attempt) {
#ifdef _WIN32
    return CaptureWindows(impl_->ffmpeg_path, attempt, jpeg, error);
#else
    return CapturePosix(impl_->ffmpeg_path, attempt, jpeg, error);
#endif
  };
  if (impl_->gpu_enabled.load()) {
    // Hardware decoding is an optimization: fall back to software if the
    // driver or the pixel format cannot produce this snapshot.
    std::vector<std::string> accelerated = args;
    const auto input = std::find(accelerated.begin(), accelerated.end(), "-i");
    accelerated.insert(input, {"-hwaccel", "auto"});
    if (run(accelerated))
      return true;
    jpeg->clear();
  }
  return run(args);
}

bool MediaService::CaptureGray(const std::string& url, int width, int height,
                               std::vector<uint8_t>* frame, std::string* error) {
  const std::string filter =
      "scale=" + std::to_string(width) + ":" + std::to_string(height) + ",format=gray";
  std::vector<std::string> args = {
      "-hide_banner", "-loglevel", "error", "-nostdin", "-rtsp_transport", "tcp",
      "-timeout",     "10000000",  "-i",    url,        "-frames:v",       "1",
      "-vf",          filter,      "-f",    "rawvideo", "pipe:1"};
  const auto run = [&](const std::vector<std::string>& attempt, std::string* output) {
#ifdef _WIN32
    return CaptureWindows(impl_->ffmpeg_path, attempt, output, error);
#else
    return CapturePosix(impl_->ffmpeg_path, attempt, output, error);
#endif
  };
  std::string raw;
  bool captured = false;
  if (impl_->gpu_enabled.load()) {
    std::vector<std::string> accelerated = args;
    const auto input = std::find(accelerated.begin(), accelerated.end(), "-i");
    accelerated.insert(input, {"-hwaccel", "auto"});
    captured = run(accelerated, &raw);
    if (!captured)
      raw.clear();
  }
  if (!captured && !run(args, &raw))
    return false;
  if (raw.size() < static_cast<size_t>(width * height)) {
    if (error)
      *error = "short raw frame from ffmpeg";
    return false;
  }
  frame->assign(raw.begin(), raw.begin() + width * height);
  return true;
}

void MediaService::SetDataSink(
    std::function<void(int64_t, const std::string&, const uint8_t*, size_t)> sink) {
  impl_->data_sink = std::move(sink);
}

void MediaService::Reconnect(int64_t channel_id) {
  std::shared_ptr<Impl::Channel> channel;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->channels.find(channel_id);
    if (it == impl_->channels.end())
      return;
    channel = it->second;
  }
  if (channel->main) {
    channel->main->Stop();
    channel->main =
        std::make_shared<Impl::Stream>(impl_.get(), channel_id, "main", channel->spec.main_url);
    channel->main->Start();
  }
  if (channel->sub) {
    channel->sub->Stop();
    channel->sub =
        std::make_shared<Impl::Stream>(impl_.get(), channel_id, "sub", channel->spec.sub_url);
    channel->sub->Start();
  }
}

uint64_t MediaService::AddRawSubscriber(int64_t channel_id, const std::string& stream, RawSink sink,
                                        std::string* error) {
  auto runtime = impl_->FindStream(channel_id, stream);
  if (!runtime) {
    if (error)
      *error = "stream is not running";
    return 0;
  }
  std::vector<uint8_t> initial;
  RawSink initial_sink = sink;
  const uint64_t id = runtime->AddRawSubscriber(std::move(sink), &initial);
  if (!initial.empty() && initial_sink)
    initial_sink(initial.data(), initial.size());
  return id;
}

void MediaService::RemoveRawSubscriber(int64_t channel_id, const std::string& stream, uint64_t id) {
  auto runtime = impl_->FindStream(channel_id, stream);
  if (runtime)
    runtime->RemoveRawSubscriber(id);
}

bool MediaService::Subscribe(int64_t channel_id, const std::string& stream,
                             uvcpp::uvcpp_ws_connection* connection, std::string* error) {
  auto runtime = impl_->FindStream(channel_id, stream);
  if (!runtime) {
    if (error)
      *error = "stream is not running";
    return false;
  }
  return runtime->AddSubscriber(connection, error);
}

StreamStatus MediaService::Status(int64_t channel_id, const std::string& stream) const {
  std::lock_guard<std::mutex> lock(impl_->status_mutex);
  const auto it = impl_->statuses.find(Impl::Key(channel_id, stream));
  return it == impl_->statuses.end() ? StreamStatus{} : it->second;
}

json MediaService::StatusJson() const {
  json result;
  result["ffmpeg"] = impl_->ffmpeg_path;
  result["available"] = Available();
  result["streams"] = json::array();
  {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    for (const auto& [key, status] : impl_->statuses) {
      result["streams"].push_back(json{{"key", key},
                                       {"state", status.state},
                                       {"error", status.error},
                                       {"bytes", status.bytes},
                                       {"reconnects", status.reconnects},
                                       {"lastDataAt", status.last_data_at_ms / 1000},
                                       {"hasKeyframe", status.has_keyframe}});
    }
  }
  return result;
}

} // namespace uvnvr
