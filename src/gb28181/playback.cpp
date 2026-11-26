#include "gb28181/playback.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>

namespace uvnvr::gb28181 {
namespace {

constexpr std::size_t kTsPacket = 188;
constexpr std::size_t kReadChunk = 64 * 1024;
constexpr int64_t kNoSeek = -1;

bool IsTsKeyframePacket(const std::uint8_t* packet) {
  if (packet[0] != 0x47 || (packet[1] & 0x40) == 0)
    return false;
  std::size_t payload = 4;
  const std::uint8_t adaptation_control = (packet[3] >> 4) & 0x03;
  if ((adaptation_control & 0x02) != 0)
    payload += 1 + static_cast<std::size_t>(packet[4]);
  if (!(adaptation_control & 0x01) || payload + 4 > kTsPacket)
    return false;
  const std::uint8_t* cursor = packet + payload;
  if (cursor[0] != 0 || cursor[1] != 0)
    return false;
  const std::uint8_t* nal = nullptr;
  if (cursor[2] == 1) {
    nal = cursor + 3;
  } else if (cursor[2] == 0 && cursor[3] == 1) {
    nal = cursor + 4;
  } else {
    return false;
  }
  if (nal >= packet + kTsPacket)
    return false;
  const int h264_type = nal[0] & 0x1f;
  if (h264_type == 5 || h264_type == 7 || h264_type == 8)
    return true;
  const int h265_type = (nal[0] >> 1) & 0x3f;
  return h265_type == 19 || h265_type == 20 || h265_type == 21 || h265_type == 32 ||
         h265_type == 33;
}

} // namespace

std::int64_t FindTsKeyframeOffset(const SegmentReadFn& reader, const std::string& path,
                                  std::int64_t approximate, std::int64_t file_size,
                                  std::int64_t scan_limit) {
  if (!reader || path.empty() || approximate <= 0 || file_size <= 0)
    return 0;
  std::int64_t offset = approximate - approximate % static_cast<std::int64_t>(kTsPacket);
  if (offset < 0)
    offset = 0;
  if (offset >= file_size)
    return 0;
  std::vector<std::uint8_t> window;
  std::string error;
  int64_t scanned = 0;
  while (scanned < scan_limit && offset < file_size) {
    if (!reader(path, static_cast<std::uint64_t>(offset), kReadChunk * 4, &window, &error))
      return 0;
    if (window.size() < kTsPacket)
      break;
    const std::size_t packets = window.size() / kTsPacket;
    for (std::size_t index = 0; index < packets; ++index) {
      if (IsTsKeyframePacket(window.data() + index * kTsPacket))
        return offset + static_cast<std::int64_t>(index) * static_cast<std::int64_t>(kTsPacket);
    }
    offset += static_cast<std::int64_t>(packets) * static_cast<std::int64_t>(kTsPacket);
    scanned += static_cast<std::int64_t>(packets) * static_cast<std::int64_t>(kTsPacket);
  }
  return 0;
}

struct PlaybackController::Impl {
  struct Session {
    PlaybackRequest request;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<double> speed{1.0};
    std::atomic<std::int64_t> seek_target_ms{kNoSeek};

    mutable std::mutex status_mutex;
    PlaybackStatus status;
  };

  Impl(SegmentSource source_in, SegmentReadFn reader_in, Sender sender_in,
       PlaybackControllerConfig config_in)
      : source(std::move(source_in)), reader(std::move(reader_in)), sender(std::move(sender_in)),
        config(config_in) {}

  SegmentSource source;
  SegmentReadFn reader;
  Sender sender;
  PlaybackControllerConfig config;
  FinishedFn finished;
  mutable std::mutex mutex;
  std::map<std::string, std::unique_ptr<Session>> sessions;

  void Run(Session* session);
  bool SendFrame(Session* session, const std::uint8_t* ps, std::size_t size,
                 std::uint32_t timestamp_90k, bool keyframe, std::string* error);
};

bool PlaybackController::Impl::SendFrame(Session* session, const std::uint8_t* ps, std::size_t size,
                                         std::uint32_t timestamp_90k, bool keyframe,
                                         std::string* error) {
  if (!sender)
    return false;
  if (!sender(session->request.call_id, ps, size, timestamp_90k, keyframe, error))
    return false;
  std::lock_guard<std::mutex> lock(session->status_mutex);
  ++session->status.frames_sent;
  session->status.bytes_sent += size;
  if (keyframe)
    ++session->status.keyframes_sent;
  return true;
}

void PlaybackController::Impl::Run(Session* session) {
  const PlaybackRequest& request = session->request;
  const bool bounded = request.end_ms > request.start_ms;
  std::int64_t cursor_ms = request.start_ms;
  std::int64_t seek_generation = 0;
  std::map<std::string, std::int64_t> read_offsets;
  int idle_polls = 0;

  auto set_finished = [this, session](const char* reason) {
    {
      std::lock_guard<std::mutex> lock(session->status_mutex);
      session->status.active = false;
      session->status.finished = true;
      session->status.paused = false;
    }
    session->stop.store(true);
    if (finished)
      finished(session->request.call_id, reason);
  };

  while (!session->stop.load()) {
    const std::int64_t seek = session->seek_target_ms.exchange(kNoSeek);
    if (seek != kNoSeek) {
      cursor_ms = std::max(request.start_ms, seek);
      read_offsets.clear();
      ++seek_generation;
      idle_polls = 0;
    }

    std::vector<PlaybackSegment> segments;
    if (source)
      segments = source(request.channel_id, request.stream, cursor_ms,
                        bounded ? request.end_ms : (1LL << 60));
    if (segments.empty()) {
      if (bounded && cursor_ms >= request.end_ms)
        break;
      if (++idle_polls > 25) { // ~5 s without a new segment
        set_finished("no more recordings");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    idle_polls = 0;

    TsDemuxer demuxer;
    PsRtpPipelineConfig pipeline_config;
    pipeline_config.rebase_timestamps = true;
    pipeline_config.timestamp_offset_90k = 0;
    PsRtpPipeline pipeline(
        [this, session](const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp,
                        bool keyframe, std::string* error) {
          return SendFrame(session, ps, size, timestamp, keyframe, error);
        },
        pipeline_config);

    bool sent_any = false;
    bool seek_break = false;
    std::int64_t first_pts = 0;
    bool have_first_pts = false;
    double media_ms = 0.0;
    double anchor_media_ms = 0.0;
    auto anchor_wall = std::chrono::steady_clock::now();
    double last_speed = session->speed.load();
    bool was_paused = false;
    bool burst_pending = config.startup_burst_ms > 0;
    std::string error;

    for (const PlaybackSegment& segment : segments) {
      if (session->stop.load() || seek_break)
        break;
      if (segment.end_ms <= cursor_ms)
        continue;
      if (bounded && segment.start_ms >= request.end_ms)
        break;
      const std::string key = segment.stream + ":" + std::to_string(segment.start_ms);
      std::int64_t offset = 0;
      const auto resumed = read_offsets.find(key);
      if (resumed != read_offsets.end()) {
        offset = resumed->second;
      } else if (cursor_ms > segment.start_ms && segment.size_bytes > 0) {
        const std::int64_t duration = std::max<std::int64_t>(1, segment.end_ms - segment.start_ms);
        const double fraction = std::clamp(static_cast<double>(cursor_ms - segment.start_ms) /
                                               static_cast<double>(duration),
                                           0.0, 1.0);
        const std::int64_t approximate =
            static_cast<std::int64_t>(fraction * static_cast<double>(segment.size_bytes));
        offset = FindTsKeyframeOffset(reader, segment.path, approximate, segment.size_bytes,
                                      config.keyframe_scan_bytes);
        if (offset < 0)
          offset = 0;
      }
      std::int64_t segment_media_base = media_ms;
      const std::int64_t segment_position_base =
          cursor_ms > segment.start_ms ? cursor_ms : segment.start_ms;
      for (;;) {
        if (session->stop.load())
          return;
        const std::int64_t seek_now = session->seek_target_ms.load();
        if (seek_now != kNoSeek) {
          seek_break = true;
          break;
        }
        const bool paused = session->paused.load();
        if (paused) {
          if (!was_paused) {
            anchor_wall = std::chrono::steady_clock::now();
            anchor_media_ms = media_ms;
            was_paused = true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        if (was_paused) {
          anchor_wall = std::chrono::steady_clock::now();
          anchor_media_ms = media_ms;
          was_paused = false;
        }

        std::vector<std::uint8_t> chunk;
        std::string read_error;
        if (!reader || !reader(segment.path, static_cast<std::uint64_t>(offset), kReadChunk, &chunk,
                               &read_error)) {
          break;
        }
        if (chunk.empty())
          break;
        offset += static_cast<std::int64_t>(chunk.size());
        read_offsets[key] = offset;

        std::vector<EsFrame> frames;
        std::string demux_error;
        if (!demuxer.Push(chunk.data(), chunk.size(), &frames, &demux_error))
          break;
        for (const EsFrame& frame : frames) {
          if (session->stop.load())
            return;
          if (session->seek_target_ms.load() != kNoSeek) {
            seek_break = true;
            break;
          }
          // Pause can land after the chunk was demuxed; check before every
          // access unit so a pause does not leak the rest of the batch.
          if (session->paused.load()) {
            was_paused = true;
            while (!session->stop.load() && session->paused.load()) {
              if (session->seek_target_ms.load() != kNoSeek)
                break;
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (session->stop.load())
              return;
            if (session->seek_target_ms.load() != kNoSeek) {
              seek_break = true;
              break;
            }
            anchor_wall = std::chrono::steady_clock::now();
            anchor_media_ms = media_ms;
            was_paused = false;
          }
          const double speed_now = session->speed.load();
          if (speed_now != last_speed) {
            anchor_wall = std::chrono::steady_clock::now();
            anchor_media_ms = media_ms;
            last_speed = speed_now;
          }
          if (!have_first_pts && frame.has_pts) {
            first_pts = frame.pts_90k;
            have_first_pts = true;
          }
          if (frame.has_pts && have_first_pts) {
            const double frame_media_ms = static_cast<double>(frame.pts_90k - first_pts) / 90.0;
            // A timestamp discontinuity (segment rotation without a continuous
            // clock) restarts the pacing anchor instead of jumping backward.
            if (frame_media_ms + 1000.0 < media_ms) {
              first_pts = frame.pts_90k;
              anchor_wall = std::chrono::steady_clock::now();
              anchor_media_ms = media_ms;
            } else {
              media_ms = frame_media_ms;
            }
          } else {
            media_ms += 40.0;
          }

          if (!pipeline.PushFrame(frame, &error)) {
            set_finished(error.empty() ? "PS/RTP send failed" : error.c_str());
            return;
          }
          sent_any = true;

          const double rate = std::max(0.01, speed_now);
          const double burst = burst_pending ? static_cast<double>(config.startup_burst_ms) : 0.0;
          const double paced_ms = std::max(0.0, media_ms - anchor_media_ms - burst);
          burst_pending = false;
          const auto target =
              anchor_wall +
              std::chrono::microseconds(static_cast<std::int64_t>(paced_ms * 1000.0 / rate));
          if (target > std::chrono::steady_clock::now())
            std::this_thread::sleep_until(target);

          const std::int64_t position_ms =
              segment_position_base + static_cast<std::int64_t>(media_ms - segment_media_base);
          {
            std::lock_guard<std::mutex> lock(session->status_mutex);
            session->status.position_ms = position_ms;
          }
          if (bounded && position_ms >= request.end_ms) {
            cursor_ms = request.end_ms;
            seek_break = true;
            break;
          }
        }
        if (seek_break)
          break;
      }
      if (seek_break)
        break;
      cursor_ms = std::max(cursor_ms, segment.end_ms);
    }

    if (!sent_any && bounded && cursor_ms >= request.end_ms)
      break;
    if (bounded && cursor_ms >= request.end_ms)
      break;
    if (!bounded && !session->stop.load())
      continue;
    (void)seek_generation;
  }

  {
    std::lock_guard<std::mutex> lock(session->status_mutex);
    session->status.active = false;
    session->status.finished = true;
  }
  if (finished)
    finished(session->request.call_id, "playback range complete");
}

PlaybackController::PlaybackController(SegmentSource source, SegmentReadFn reader, Sender sender,
                                       PlaybackControllerConfig config)
    : impl_(std::make_unique<Impl>(std::move(source), std::move(reader), std::move(sender),
                                   config)) {}

PlaybackController::~PlaybackController() {
  StopAll();
}

void PlaybackController::set_finished_callback(FinishedFn callback) {
  impl_->finished = std::move(callback);
}

bool PlaybackController::Start(const PlaybackRequest& request, std::string* error) {
  if (request.call_id.empty()) {
    if (error != nullptr)
      *error = "playback request has no Call-ID";
    return false;
  }
  if (request.end_ms <= request.start_ms) {
    if (error != nullptr)
      *error = "playback range is empty";
    return false;
  }
  if (request.speed <= 0.0 || request.speed < impl_->config.min_speed ||
      request.speed > impl_->config.max_speed) {
    if (error != nullptr)
      *error = "playback speed is outside the supported range";
    return false;
  }
  auto session = std::make_unique<Impl::Session>();
  session->request = request;
  session->status.call_id = request.call_id;
  session->status.channel_id = request.channel_id;
  session->status.stream = request.stream;
  session->status.start_ms = request.start_ms;
  session->status.end_ms = request.end_ms;
  session->status.speed = request.speed;
  session->status.active = true;
  session->speed.store(request.speed);

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->sessions.find(request.call_id) != impl_->sessions.end()) {
    if (error != nullptr)
      *error = "a playback session already exists for this Call-ID";
    return false;
  }
  Impl::Session* raw = session.get();
  impl_->sessions.emplace(request.call_id, std::move(session));
  raw->thread = std::thread([impl = impl_.get(), raw] { impl->Run(raw); });
  return true;
}

bool PlaybackController::SetSpeed(const std::string& call_id, double speed, std::string* error) {
  if (speed <= 0.0 || speed < impl_->config.min_speed || speed > impl_->config.max_speed) {
    if (error != nullptr)
      *error = "playback speed is outside the supported range";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->sessions.find(call_id);
  if (it == impl_->sessions.end()) {
    if (error != nullptr)
      *error = "no playback session for this Call-ID";
    return false;
  }
  it->second->speed.store(speed);
  std::lock_guard<std::mutex> status_lock(it->second->status_mutex);
  it->second->status.speed = speed;
  it->second->status.paused = false;
  it->second->paused.store(false);
  return true;
}

bool PlaybackController::Pause(const std::string& call_id, bool paused, std::string* error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->sessions.find(call_id);
  if (it == impl_->sessions.end()) {
    if (error != nullptr)
      *error = "no playback session for this Call-ID";
    return false;
  }
  it->second->paused.store(paused);
  std::lock_guard<std::mutex> status_lock(it->second->status_mutex);
  it->second->status.paused = paused;
  return true;
}

bool PlaybackController::Seek(const std::string& call_id, std::int64_t position_ms,
                              std::string* error) {
  if (position_ms < 0) {
    if (error != nullptr)
      *error = "seek position is negative";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->sessions.find(call_id);
  if (it == impl_->sessions.end()) {
    if (error != nullptr)
      *error = "no playback session for this Call-ID";
    return false;
  }
  it->second->seek_target_ms.store(position_ms);
  std::lock_guard<std::mutex> status_lock(it->second->status_mutex);
  it->second->status.position_ms = position_ms;
  return true;
}

void PlaybackController::Stop(const std::string& call_id) {
  std::unique_ptr<Impl::Session> session;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->sessions.find(call_id);
    if (it == impl_->sessions.end())
      return;
    session = std::move(it->second);
    impl_->sessions.erase(it);
  }
  session->stop.store(true);
  if (session->thread.joinable())
    session->thread.join();
}

void PlaybackController::StopAll() {
  std::map<std::string, std::unique_ptr<Impl::Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    sessions.swap(impl_->sessions);
  }
  for (auto& [id, session] : sessions) {
    (void)id;
    session->stop.store(true);
  }
  for (auto& [id, session] : sessions) {
    (void)id;
    if (session->thread.joinable())
      session->thread.join();
  }
}

bool PlaybackController::Snapshot(const std::string& call_id, PlaybackStatus* status) const {
  if (status == nullptr)
    return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->sessions.find(call_id);
  if (it == impl_->sessions.end())
    return false;
  std::lock_guard<std::mutex> status_lock(it->second->status_mutex);
  *status = it->second->status;
  return true;
}

std::vector<std::string> PlaybackController::call_ids() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::string> ids;
  ids.reserve(impl_->sessions.size());
  for (const auto& [id, session] : impl_->sessions) {
    (void)session;
    ids.push_back(id);
  }
  return ids;
}

std::size_t PlaybackController::size() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->sessions.size();
}

} // namespace uvnvr::gb28181
