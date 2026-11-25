// Unit tests for GB/T 28181 RecordInfo, playback SDP/control and the shared
// TS -> PS/RTP pipeline (GH-19).
//
// The TS fixtures are generated in-process (PAT/PMT/PES with H.264 NALs), so
// the test does not depend on ffmpeg or on the network. The controller test
// writes a temporary segment and drives pause/speed/seek through the same API
// the SIP INFO handler uses.

#include "gb28181/manscdp.h"
#include "gb28181/playback.h"
#include "gb28181/service.h"
#include "gb28181/time.h"
#include "gb28181/ts_ps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace uvnvr::gb28181;

int failures = 0;

void Expect(const std::string& name, std::string_view actual, std::string_view expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%.*s', expected '%.*s'\n", name.c_str(),
               static_cast<int>(actual.size()), actual.data(), static_cast<int>(expected.size()),
               expected.data());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectInt(const std::string& name, std::int64_t actual, std::int64_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %lld, expected %lld\n", name.c_str(),
               static_cast<long long>(actual), static_cast<long long>(expected));
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  if (haystack.find(needle) != std::string::npos)
    return;
  std::fprintf(stderr, "FAIL %s: does not contain '%s'\n", name.c_str(), needle.c_str());
  ++failures;
}

// ---------------------------------------------------------------------------
// Synthetic MPEG-TS
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> MakePacket(std::uint16_t pid, bool pusi,
                                     const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> packet(188, 0xFF);
  packet[0] = 0x47;
  packet[1] = static_cast<std::uint8_t>((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
  packet[2] = static_cast<std::uint8_t>(pid & 0xFF);
  if (payload.size() <= 183) {
    const std::size_t adaptation_length = 183 - payload.size();
    packet[3] = 0x30; // adaptation + payload
    packet[4] = static_cast<std::uint8_t>(adaptation_length);
    if (adaptation_length > 0) {
      packet[5] = 0x00;
      for (std::size_t i = 6; i < 5 + adaptation_length; ++i)
        packet[i] = 0xFF;
    }
    const std::size_t offset = 5 + adaptation_length;
    std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset));
  } else {
    packet[3] = 0x10; // payload only, 184 bytes
    std::copy(payload.begin(), payload.begin() + 184, packet.begin() + 4);
  }
  return packet;
}

std::vector<std::uint8_t> MakeSection(std::vector<std::uint8_t> section) {
  const std::size_t length = section.size() - 3 + 4;
  section[1] = static_cast<std::uint8_t>(0xB0 | ((length >> 8) & 0x0F));
  section[2] = static_cast<std::uint8_t>(length & 0xFF);
  section.insert(section.end(), 4, 0x00); // CRC (the demuxer does not verify it)
  std::vector<std::uint8_t> payload;
  payload.push_back(0x00); // pointer_field
  payload.insert(payload.end(), section.begin(), section.end());
  return payload;
}

std::vector<std::uint8_t> MakePat(std::uint16_t pmt_pid) {
  std::vector<std::uint8_t> section = {
      0x00,
      0x00,
      0x00, // table_id + placeholder length
      0x00,
      0x01, // transport_stream_id
      0xC1, // version 0, current
      0x00,
      0x00, // section / last section
      0x00,
      0x01, // program 1
      static_cast<std::uint8_t>(0xE0 | ((pmt_pid >> 8) & 0x1F)),
      static_cast<std::uint8_t>(pmt_pid & 0xFF),
  };
  return MakeSection(std::move(section));
}

std::vector<std::uint8_t> MakePmt(std::uint16_t video_pid, std::uint8_t stream_type) {
  std::vector<std::uint8_t> section = {
      0x02,
      0x00,
      0x00, // table_id + placeholder length
      0x00,
      0x01, // program_number
      0xC1, // version
      0x00,
      0x00, // section / last
      0xE1,
      0x00, // PCR_PID
      0xF0,
      0x00, // program_info_length
      stream_type,
      static_cast<std::uint8_t>(0xE0 | ((video_pid >> 8) & 0x1F)),
      static_cast<std::uint8_t>(video_pid & 0xFF),
      0xF0,
      0x00, // ES_info_length
  };
  return MakeSection(std::move(section));
}

void AppendPts(std::vector<std::uint8_t>* out, std::int64_t pts) {
  const std::uint64_t value = static_cast<std::uint64_t>(pts) & ((1ULL << 33) - 1);
  out->push_back(static_cast<std::uint8_t>(0x20 | (((value >> 30) & 0x07) << 1) | 0x01));
  out->push_back(static_cast<std::uint8_t>((value >> 22) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((((value >> 15) & 0x7F) << 1) | 0x01));
  out->push_back(static_cast<std::uint8_t>((value >> 7) & 0xFF));
  out->push_back(static_cast<std::uint8_t>(((value & 0x7F) << 1) | 0x01));
}

std::vector<std::uint8_t> MakePes(std::int64_t pts, const std::vector<std::uint8_t>& es) {
  std::vector<std::uint8_t> pes = {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x80, 0x05};
  AppendPts(&pes, pts);
  pes.insert(pes.end(), es.begin(), es.end());
  const std::size_t length = pes.size() - 6;
  pes[4] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
  pes[5] = static_cast<std::uint8_t>(length & 0xFF);
  return pes;
}

void AppendNal(std::vector<std::uint8_t>* out, std::initializer_list<std::uint8_t> nal) {
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  out->push_back(1);
  out->insert(out->end(), nal.begin(), nal.end());
}

std::vector<std::uint8_t> MakeEs(bool keyframe) {
  std::vector<std::uint8_t> es;
  if (keyframe) {
    AppendNal(&es, {0x67, 0x42, 0x00, 0x0A}); // SPS
    AppendNal(&es, {0x68, 0xCE, 0x3C, 0x80}); // PPS
    AppendNal(&es, {0x65, 0x88, 0x84, 0x00}); // IDR
  } else {
    AppendNal(&es, {0x41, 0x9A, 0x02, 0x03}); // non-IDR slice
  }
  return es;
}

std::vector<std::uint8_t> MakeTsSegment(std::size_t frames, std::int64_t start_pts) {
  constexpr std::uint16_t kPmtPid = 0x1000;
  constexpr std::uint16_t kVideoPid = 0x0100;
  std::vector<std::uint8_t> ts;
  auto append = [&ts](const std::vector<std::uint8_t>& packet) {
    ts.insert(ts.end(), packet.begin(), packet.end());
  };
  append(MakePacket(0, true, MakePat(kPmtPid)));
  append(MakePacket(kPmtPid, true, MakePmt(kVideoPid, kPsStreamTypeH264)));
  for (std::size_t i = 0; i < frames; ++i) {
    const bool keyframe = (i % 25) == 0;
    const std::int64_t pts = start_pts + static_cast<std::int64_t>(i) * 3000;
    append(MakePacket(kVideoPid, true, MakePes(pts, MakeEs(keyframe))));
  }
  return ts;
}

// ---------------------------------------------------------------------------
// Time / MANSCDP
// ---------------------------------------------------------------------------

void TestTime() {
  std::int64_t epoch = 0;
  std::string error;
  ExpectBool("parse gb time", ParseGbTime("2026-11-25T12:34:56", &epoch, &error), true);
  Expect("format gb time", FormatGbTime(epoch), "2026-11-25T12:34:56");
  Expect("format compact", FormatGbCompactTime(epoch), "20261125123456");
  std::int64_t compact = 0;
  ExpectBool("parse compact", ParseGbCompactTime("20261125123456", &compact, &error), true);
  ExpectInt("compact round trip", compact, epoch);
  ExpectBool("reject invalid date", ParseGbTime("2026-02-30T00:00:00", &epoch, &error), false);

  std::int64_t start = 0, end = 0;
  ExpectBool("parse range",
             ParseGbTimeRange("2026-11-25T12:00:00/2026-11-25T12:01:00", &start, &end, &error),
             true);
  Expect("range start format", FormatGbTime(start), "2026-11-25T12:00:00");
  Expect("range end format", FormatGbTime(end), "2026-11-25T12:01:00");
  std::printf("time: local and compact forms round-trip\n");
}

void TestRecordInfo() {
  RecordInfoQuery query;
  query.sn = 42;
  query.device_id = "34020000001180000001";
  query.start_ms = 0;
  query.end_ms = 0;
  std::string error;
  ParseGbTime("2026-11-25T00:00:00", &query.start_ms, &error);
  ParseGbTime("2026-11-25T23:59:59", &query.end_ms, &error);
  query.type = "all";
  const std::string xml = BuildRecordInfoQuery(query);
  ExpectContains("query cmd", xml, "<CmdType>RecordInfo</CmdType>");
  ExpectContains("query start", xml, "<StartTime>2026-11-25T00:00:00</StartTime>");

  RecordInfoQuery parsed;
  ExpectBool("parse record query", ParseRecordInfoQuery(xml, &parsed, &error), true);
  Expect("query sn", std::to_string(parsed.sn), "42");
  Expect("query type", parsed.type, "all");
  ExpectInt("query start", parsed.start_ms, query.start_ms);

  RecordInfoResponse response;
  response.sn = 42;
  response.device_id = query.device_id;
  response.name = "UvNVR";
  RecordItem item;
  item.device_id = "34020000001320000001";
  item.name = "gate";
  item.file_path = "recordings/2026/11/25/gate.ts";
  item.start_ms = query.start_ms;
  item.end_ms = query.start_ms + 60000;
  item.type = "time";
  item.file_size = 1234;
  response.items.push_back(item);
  response.sum_num = 1;
  const std::string body = BuildRecordInfoResponse(response);
  ExpectContains("response list", body, "<RecordList Num=\"1\">");
  ExpectContains("response item", body, "<FilePath>recordings/2026/11/25/gate.ts</FilePath>");

  RecordInfoResponse parsed_response;
  ExpectBool("parse record response", ParseRecordInfoResponse(body, &parsed_response, &error),
             true);
  ExpectInt("response sum", parsed_response.sum_num, 1);
  Expect("response item type", parsed_response.items.at(0).type, "time");
  ExpectInt("response item size", static_cast<std::int64_t>(parsed_response.items.at(0).file_size),
            1234);

  PlaybackControl control;
  control.sn = 7;
  control.device_id = item.device_id;
  control.scale = 2.0;
  control.has_scale = true;
  control.has_range = true;
  control.range_start_ms = query.start_ms + 30000;
  control.range_end_ms = query.end_ms;
  const std::string control_xml = BuildPlaybackControl(control);
  PlaybackControl parsed_control;
  ExpectBool("parse playback control", ParsePlaybackControl(control_xml, &parsed_control, &error),
             true);
  ExpectBool("control has scale", parsed_control.has_scale, true);
  ExpectInt("control range start", parsed_control.range_start_ms, control.range_start_ms);
  ExpectBool("control scale 2", parsed_control.scale == 2.0, true);

  PlaybackControl invalid;
  ExpectBool(
      "reject negative range",
      ParsePlaybackControl("<Control><CmdType>Playback</CmdType><SN>1</SN>"
                           "<Range>2026-11-25T12:01:00/2026-11-25T12:00:00</Range></Control>",
                           &invalid, &error),
      false);
  std::printf("RecordInfo: query/response/control XML round-trip\n");
}

void TestPlaybackSdp() {
  const std::string sdp = "v=0\r\n"
                          "o=34020000001320000001 0 0 IN IP4 192.0.2.10\r\n"
                          "s=Playback\r\n"
                          "u=34020000001320000001:20261125120000:20261125120100\r\n"
                          "c=IN IP4 192.0.2.10\r\n"
                          "t=0 0\r\n"
                          "m=video 40000 RTP/AVP 96\r\n"
                          "a=recvonly\r\n"
                          "a=rtpmap:96 PS/90000\r\n"
                          "y=01020304\r\n";
  std::string channel;
  std::int64_t start = 0, end = 0;
  bool playback = false;
  std::string error;
  ExpectBool("parse playback SDP",
             Gb28181Service::ParsePlaybackSdp(sdp, &channel, &start, &end, &playback, &error),
             true);
  ExpectBool("SDP is playback", playback, true);
  Expect("SDP channel", channel, "34020000001320000001");
  Expect("SDP start", FormatGbCompactTime(start), "20261125120000");
  Expect("SDP end", FormatGbCompactTime(end), "20261125120100");

  const std::string live = "v=0\r\no=34020000001320000001 0 0 IN IP4 192.0.2.10\r\ns=Play\r\n"
                           "c=IN IP4 192.0.2.10\r\nt=0 0\r\nm=video 40002 RTP/AVP 96\r\n"
                           "a=recvonly\r\na=rtpmap:96 PS/90000\r\n";
  ExpectBool("parse live SDP",
             Gb28181Service::ParsePlaybackSdp(live, &channel, &start, &end, &playback, &error),
             true);
  ExpectBool("SDP is live", playback, false);
  std::printf("playback SDP: s=Playback/u= range and s=Play distinguished\n");
}

// ---------------------------------------------------------------------------
// TS -> PS/RTP
// ---------------------------------------------------------------------------

void TestPipeline() {
  const std::size_t frames = 50;
  const std::vector<std::uint8_t> ts = MakeTsSegment(frames, 90000);

  TsDemuxer demuxer;
  std::vector<EsFrame> demuxed;
  std::string error;
  ExpectBool("demux TS", demuxer.Push(ts.data(), ts.size(), &demuxed, &error), true);
  ExpectBool("demux has video", demuxer.has_video(), true);
  ExpectBool("demux codec h264", demuxer.codec() == PsVideoCodec::kH264, true);
  ExpectInt("demux frame count", static_cast<std::int64_t>(demuxed.size()),
            static_cast<std::int64_t>(frames));
  ExpectInt("demux first pts", demuxed.front().pts_90k, 90000);
  ExpectBool("demux keyframe", demuxed.front().keyframe, true);
  ExpectBool("demux non-key", demuxed.at(1).keyframe, false);

  struct Sent {
    std::vector<std::uint8_t> ps;
    std::uint32_t timestamp = 0;
    bool keyframe = false;
  };
  std::vector<Sent> sent;
  PsRtpPipelineConfig config;
  config.rebase_timestamps = true;
  config.timestamp_offset_90k = 1000;
  PsRtpPipeline pipeline(
      [&sent](const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp, bool keyframe,
              std::string*) {
        Sent item;
        item.ps.assign(ps, ps + size);
        item.timestamp = timestamp;
        item.keyframe = keyframe;
        sent.push_back(std::move(item));
        return true;
      },
      config);
  ExpectBool("pipeline push TS", pipeline.PushTs(ts.data(), ts.size(), &error), true);
  ExpectInt("pipeline frame count", static_cast<std::int64_t>(sent.size()),
            static_cast<std::int64_t>(frames));
  ExpectInt("pipeline first timestamp", sent.front().timestamp, 1000);
  ExpectInt("pipeline second timestamp", sent.at(1).timestamp, 4000);
  ExpectBool("pipeline keyframe", sent.front().keyframe, true);

  PsPackInfo info;
  ExpectBool("parse PS pack",
             ParsePsPack(sent.front().ps.data(), sent.front().ps.size(), &info, &error), true);
  ExpectBool("PS system header", info.has_system_header, true);
  ExpectBool("PS PSM", info.has_psm, true);
  ExpectInt("PS stream type", info.psm_stream_type, kPsStreamTypeH264);
  ExpectBool("PS PTS", info.has_pts, true);
  ExpectInt("PS PES pts", info.pts, 1000);

  // A live pipeline keeps the source timeline.
  std::uint32_t live_timestamp = 0;
  PsRtpPipeline live([&live_timestamp](const std::uint8_t*, std::size_t, std::uint32_t timestamp,
                                       bool, std::string*) {
    live_timestamp = timestamp;
    return true;
  });
  const std::vector<std::uint8_t> short_ts = MakeTsSegment(1, 777000);
  ExpectBool("live pipeline", live.PushTs(short_ts.data(), short_ts.size(), &error), true);
  ExpectInt("live timestamp unchanged", live_timestamp, 777000);
  std::printf("pipeline: %zu frames TS -> PS; rebased and live timestamps verified\n", sent.size());
}

// ---------------------------------------------------------------------------
// Playback controller: pause / speed / seek
// ---------------------------------------------------------------------------

struct Collector {
  std::mutex mutex;
  std::condition_variable cv;
  std::size_t frames = 0;
  std::vector<std::uint32_t> timestamps;
};

void TestPlaybackController() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "uvnvr-gb28181-playback-test";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path path = dir / "segment.ts";
  const std::size_t frames = 240;
  const std::vector<std::uint8_t> ts = MakeTsSegment(frames, 0);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(ts.data()), static_cast<std::streamsize>(ts.size()));
  }
  const int64_t start_ms = 1700000000000LL;
  PlaybackSegment segment;
  segment.channel_id = 1;
  segment.stream = "main";
  segment.path = path.string();
  segment.start_ms = start_ms;
  segment.end_ms = start_ms + static_cast<int64_t>(frames) * 40;
  segment.size_bytes = static_cast<int64_t>(ts.size());
  segment.codec = "h264";
  segment.record_type = "timed";

  auto collector = std::make_shared<Collector>();
  SegmentSource source = [segment](int64_t, const std::string&, int64_t start, int64_t end) {
    std::vector<PlaybackSegment> out;
    if (segment.start_ms < end && segment.end_ms > start)
      out.push_back(segment);
    return out;
  };
  SegmentReadFn reader = [](const std::string& file_path, std::uint64_t offset, std::size_t size,
                            std::vector<std::uint8_t>* out, std::string* error) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
      if (error != nullptr)
        *error = "cannot open";
      return false;
    }
    file.seekg(static_cast<std::streamoff>(offset));
    out->resize(size);
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
    out->resize(static_cast<std::size_t>(file.gcount()));
    return true;
  };
  PlaybackControllerConfig config;
  config.startup_burst_ms = 0;
  PlaybackController controller(
      source, reader,
      [collector](const std::string&, const std::uint8_t*, std::size_t, std::uint32_t timestamp,
                  bool, std::string*) {
        std::lock_guard<std::mutex> lock(collector->mutex);
        collector->timestamps.push_back(timestamp);
        ++collector->frames;
        collector->cv.notify_all();
        return true;
      },
      config);
  bool finished = false;
  controller.set_finished_callback(
      [&finished](const std::string&, const std::string&) { finished = true; });

  PlaybackRequest request;
  request.call_id = "test-call";
  request.channel_id = 1;
  request.stream = "main";
  request.start_ms = start_ms;
  request.end_ms = segment.end_ms;
  request.speed = 4.0;
  std::string error;
  ExpectBool("controller starts", controller.Start(request, &error), true);

  auto wait_frames = [collector](std::size_t target) {
    std::unique_lock<std::mutex> lock(collector->mutex);
    return collector->cv.wait_for(lock, std::chrono::seconds(5),
                                  [collector, target] { return collector->frames >= target; });
  };
  ExpectBool("controller sends initial frames", wait_frames(8), true);

  ExpectBool("pause", controller.Pause("test-call", true, &error), true);
  std::size_t paused_at = 0;
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    paused_at = collector->frames;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::size_t after_pause = 0;
  {
    std::lock_guard<std::mutex> lock(collector->mutex);
    after_pause = collector->frames;
  }
  // At most one frame may already have been queued when the pause landed.
  ExpectBool("no frames while paused", after_pause <= paused_at + 1, true);

  ExpectBool("resume", controller.Pause("test-call", false, &error), true);
  ExpectBool("frames resume", wait_frames(after_pause + 4), true);

  ExpectBool("set speed", controller.SetSpeed("test-call", 2.0, &error), true);
  PlaybackStatus snapshot;
  ExpectBool("snapshot", controller.Snapshot("test-call", &snapshot), true);
  ExpectBool("snapshot speed", snapshot.speed == 2.0, true);
  ExpectBool("snapshot active", snapshot.active, true);

  const std::size_t before_seek = [collector] {
    std::lock_guard<std::mutex> lock(collector->mutex);
    return collector->frames;
  }();
  ExpectBool("seek", controller.Seek("test-call", start_ms + 4000, &error), true);
  ExpectBool("frames after seek", wait_frames(before_seek + 4), true);
  ExpectBool("seek snapshot position",
             controller.Snapshot("test-call", &snapshot) && snapshot.position_ms >= start_ms + 3000,
             true);

  controller.Stop("test-call");
  (void)finished;
  fs::remove_all(dir, ec);
  std::printf("playback: pause/resume, 2x speed and seek exercised on %zu frames\n", frames);
}

} // namespace

int main() {
  TestTime();
  TestRecordInfo();
  TestPlaybackSdp();
  TestPipeline();
  TestPlaybackController();

  if (failures == 0) {
    std::printf("gb28181_record_playback_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "gb28181_record_playback_test: %d checks failed\n", failures);
  return 1;
}
