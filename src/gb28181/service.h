#pragma once

// GB/T 28181 device-side service (P6.2d).
//
// This is the wiring between the three lower-level modules:
//
//   SipUserAgent       REGISTER/Keepalive/MESSAGE and INVITE/ACK/BYE/INFO
//   ManscdpResponder   Catalog/DeviceInfo/DeviceStatus/RecordInfo XML
//   MediaSessionManager RTP/RTCP dialogs
//   PlaybackController recorded TS -> shared PS/RTP pipeline
//
// The service owns one libuv loop thread. Live preview and playback frames are
// queued to that thread and sent through MediaSessionManager, so the SIP stack
// and the RTP sockets are never touched from a recorder or file-reader thread.
//
// Compiled only with UVNVR_ENABLE_GB28181=ON (default off). It does not change
// the existing preview/recording/ONVIF paths: App starts it only when the
// `gb28181.enabled` setting is on.

#include "gb28181/manscdp.h"
#include "gb28181/media_session.h"
#include "gb28181/playback.h"
#include "gb28181/sip.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uvnvr::gb28181 {

struct GbChannel {
  std::int64_t local_id = 0;
  std::string gb_id;
  std::string name;
  bool enabled = true;
  std::string manufacturer;
  std::string model;
};

struct Gb28181Config {
  // The NVR's own 20-digit identifier (type code 118 for an NVR).
  std::string device_id;
  std::string device_name = "UvNVR";
  // Used to generate channel identifiers when a channel has no explicit one.
  std::string center_code;
  std::string civil_code;
  std::string owner;
  std::string address;
  int secrecy = 0;

  // Platform / registrar.
  std::string server_host;
  std::uint16_t server_port = 5060;
  bool tcp = false;
  std::string username;
  std::string password;
  std::string server_id;
  std::string domain;
  std::string local_ip;
  std::string bind_ip;
  std::uint16_t local_port = 0;
  std::uint32_t expires_seconds = 3600;
  std::uint32_t keepalive_seconds = 60;

  // Media.
  std::string default_stream = "main";
  std::string fallback_stream = "sub";
  std::uint16_t rtp_port_min = 30000;
  std::uint16_t rtp_port_max = 30998;
  std::size_t max_sessions = 8;
  std::string data_dir;
  std::size_t max_record_items = 10000;
  bool include_disabled_channels = false;
  PlaybackControllerConfig playback;
};

// One alarm to publish to the platform. `channel_id` is the local channel
// primary key; 0 means the NVR itself (for example a service error). The
// service maps it to the 20-digit device/channel identifier.
struct GbAlarm {
  std::int64_t channel_id = 0;
  int priority = 2; // GB/T 28181 1..4, 1 is the most severe
  int method = 5;   // 5 = video alarm
  std::string alarm_type = "1";
  std::string alarm_type_param;
  std::string description;
  std::int64_t time_ms = 0; // 0 = now
};

struct Gb28181Callbacks {
  using ChannelSource = std::function<std::vector<GbChannel>()>;
  // Dispatches a parsed PTZ command to the camera backend (ONVIF or the
  // private protocol). It is called on the GB28181 loop thread and must not
  // block: App queues the network call on a worker and returns true when the
  // command was accepted for dispatch.
  using PtzSink = std::function<bool(std::int64_t channel_id, const PtzControl& command,
                                     std::string* error)>;
  // Implemented by App: bounded SQLite query; channel_id 0 means all channels,
  // an empty stream means all streams. Active (unindexed) segments may be
  // appended for RecordInfo; playback should return finalized rows only.
  using RecordSource =
      std::function<std::vector<PlaybackSegment>(std::int64_t channel_id, const std::string& stream,
                                                 std::int64_t start_ms, std::int64_t end_ms)>;
  // Opens a live raw-TS subscription. The sink is invoked on the media loop
  // and must not block. Returns 0 on failure.
  using LiveOpenFn = std::function<std::uint64_t(
      std::int64_t channel_id, const std::string& stream,
      std::function<void(const std::uint8_t*, std::size_t)> sink, std::string* error)>;
  using LiveCloseFn =
      std::function<void(std::int64_t channel_id, const std::string& stream, std::uint64_t id)>;
  // Optional storage reader. Empty means plain std::ifstream.
  SegmentReadFn read_segment;
  std::function<void(const std::string&)> log;

  ChannelSource channels;
  RecordSource records;
  LiveOpenFn live_open;
  LiveCloseFn live_close;
  PtzSink ptz_control;
};

class Gb28181Service {
public:
  Gb28181Service(Gb28181Config config, Gb28181Callbacks callbacks);
  ~Gb28181Service();

  Gb28181Service(const Gb28181Service&) = delete;
  Gb28181Service& operator=(const Gb28181Service&) = delete;

  // Starts the event-loop thread and registers with the platform. Returns
  // false for an invalid device identifier or an immediate transport error.
  bool Start(std::string* error = nullptr);
  void Stop();

  // Thread-safe snapshot for the system API and the integration harness.
  struct Status {
    bool running = false;
    bool registered = false;
    std::string state;
    std::string local_ip;
    std::uint16_t local_port = 0;
    std::size_t live_sessions = 0;
    std::size_t playback_sessions = 0;
    std::size_t alarm_subscriptions = 0;
    std::uint64_t alarms_sent = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t messages_out = 0;
  };
  Status GetStatus() const;

  // Publishes one alarm to the platform. Safe to call from any thread: the
  // report is queued to the GB28181 loop, sent as an Alarm MESSAGE and
  // fanned out to every active Alarm subscriber as a NOTIFY. Returns false
  // only when the service has been stopped (the queue is still accepted
  // while registration is in progress).
  bool ReportAlarm(const GbAlarm& alarm);

  // Parses a playback SDP offer. Exposed so the CLI and tests can validate the
  // `s=Playback` / `u=<channel>:<start>:<end>` convention without a socket.
  static bool ParsePlaybackSdp(const std::string& sdp, std::string* channel_id,
                               std::int64_t* start_ms, std::int64_t* end_ms, bool* is_playback,
                               std::string* error = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace uvnvr::gb28181
