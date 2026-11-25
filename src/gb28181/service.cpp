#include "gb28181/service.h"

#include "gb28181/time.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <handle/uvcpp_loop.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <uv.h>

namespace uvnvr::gb28181 {
namespace {

constexpr std::size_t kMaxSendQueueBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxLiveBufferBytes = 4 * 1024 * 1024;

int64_t MonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ToLower(std::string text) {
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  return ToLower(a) == ToLower(b);
}

std::string TrimText(std::string text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    ++begin;
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    --end;
  return text.substr(begin, end - begin);
}

// Extracts the URI from a Contact value: "<sip:a@b>;expires=60" -> "sip:a@b".
std::string ContactUri(std::string contact) {
  contact = TrimText(std::move(contact));
  const std::size_t open = contact.find('<');
  if (open != std::string::npos) {
    const std::size_t close = contact.find('>', open + 1);
    if (close != std::string::npos)
      return TrimText(contact.substr(open + 1, close - open - 1));
  }
  const std::size_t semicolon = contact.find(';');
  if (semicolon != std::string::npos)
    contact.resize(semicolon);
  return TrimText(std::move(contact));
}

// "Catalog;id=1" -> "Catalog".
std::string EventPackage(std::string value) {
  value = TrimText(std::move(value));
  const std::size_t semicolon = value.find(';');
  if (semicolon != std::string::npos)
    value.resize(semicolon);
  return TrimText(std::move(value));
}

bool ParseExpires(std::string text, std::uint32_t* expires) {
  text = TrimText(std::move(text));
  if (text.empty())
    return false;
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      parsed > 86400 * 7) {
    return false;
  }
  *expires = static_cast<std::uint32_t>(parsed);
  return true;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : text) {
    if (c == '\n') {
      if (!current.empty() && current.back() == '\r')
        current.pop_back();
      lines.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty())
    lines.push_back(std::move(current));
  return lines;
}

std::string RelativizePath(const std::string& path, const std::string& data_dir) {
  if (data_dir.empty())
    return path;
  std::string prefix = data_dir;
  if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
    prefix.push_back('/');
  if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
    return path.substr(prefix.size());
  return path;
}

std::string LocalRecordType(const std::string& record_type) {
  if (record_type == "event" || record_type == "alarm")
    return "alarm";
  if (record_type == "manual")
    return "manual";
  return "time";
}

bool ReadPlainSegment(const std::string& path, std::uint64_t offset, std::size_t size,
                      std::vector<std::uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr)
      *error = "read output is null";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr)
      *error = "cannot open " + path;
    return false;
  }
  file.seekg(static_cast<std::streamoff>(offset));
  if (!file) {
    out->clear();
    return true;
  }
  out->resize(size);
  file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
  out->resize(static_cast<std::size_t>(file.gcount()));
  return true;
}

} // namespace

struct Gb28181Service::Impl {
  Impl(Gb28181Config config_in, Gb28181Callbacks callbacks_in)
      : config(std::move(config_in)), callbacks(std::move(callbacks_in)) {}

  struct Pending {
    bool playback = false;
    std::int64_t channel_id = 0;
    std::string stream;
    PlaybackRequest request;
  };

  struct LiveSession {
    std::string call_id;
    std::int64_t channel_id = 0;
    std::string stream;
    std::uint64_t subscription = 0;
    std::unique_ptr<PsRtpPipeline> pipeline;
    std::mutex mutex;
    std::deque<std::vector<std::uint8_t>> chunks;
    std::size_t buffered = 0;
  };

  struct Outbound {
    std::string call_id;
    std::vector<std::uint8_t> ps;
    std::uint32_t timestamp_90k = 0;
    bool keyframe = false;
  };

  void Log(const std::string& text) {
    if (callbacks.log)
      callbacks.log(text);
    else
      std::fprintf(stderr, "[gb28181] %s\n", text.c_str());
  }

  std::vector<GbChannel> Channels() const {
    return callbacks.channels ? callbacks.channels() : std::vector<GbChannel>{};
  }

  std::string ChannelGbId(const GbChannel& channel) const {
    if (!channel.gb_id.empty())
      return channel.gb_id;
    if (config.center_code.size() != 8 || channel.local_id <= 0 || channel.local_id > 9999999)
      return {};
    return MakeChannelGb28181Id(config.center_code, static_cast<std::uint32_t>(channel.local_id));
  }

  bool FindChannelByGb(const std::string& gb_id, GbChannel* out) const {
    if (gb_id.empty())
      return false;
    for (const GbChannel& channel : Channels()) {
      if (ChannelGbId(channel) == gb_id) {
        if (out != nullptr)
          *out = channel;
        return true;
      }
    }
    return false;
  }

  bool FindChannelByLocal(std::int64_t local_id, GbChannel* out) const {
    for (const GbChannel& channel : Channels()) {
      if (channel.local_id == local_id) {
        if (out != nullptr)
          *out = channel;
        return true;
      }
    }
    return false;
  }

  std::vector<RecordItem> HandleRecordInfo(const RecordInfoQuery& query) {
    std::vector<RecordItem> items;
    if (!callbacks.records)
      return items;

    int64_t channel_filter = 0;
    if (query.device_id != config.device_id) {
      GbChannel channel;
      if (!FindChannelByGb(query.device_id, &channel) || !channel.enabled)
        return items;
      channel_filter = channel.local_id;
    }

    std::vector<PlaybackSegment> records =
        callbacks.records(channel_filter, std::string(), query.start_ms, query.end_ms);
    for (const PlaybackSegment& record : records) {
      if (record.end_ms <= query.start_ms || record.start_ms >= query.end_ms)
        continue;
      const std::string type = LocalRecordType(record.record_type);
      if (query.type != "all" && query.type != type)
        continue;
      GbChannel channel;
      if (!FindChannelByLocal(record.channel_id, &channel))
        continue;
      const std::string gb_id = ChannelGbId(channel);
      if (gb_id.empty())
        continue;
      RecordItem item;
      item.device_id = gb_id;
      item.name = channel.name;
      item.file_path = RelativizePath(record.path, config.data_dir);
      item.address = config.address;
      item.start_ms = std::max(record.start_ms, query.start_ms);
      item.end_ms = std::min(record.end_ms, query.end_ms);
      item.secrecy = config.secrecy;
      item.type = type;
      item.file_size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, record.size_bytes));
      items.push_back(std::move(item));
      if (items.size() >= config.max_record_items)
        break;
    }
    return items;
  }

  std::vector<CatalogChannel> CatalogChannels() {
    std::vector<CatalogChannel> out;
    for (const GbChannel& channel : Channels()) {
      const std::string gb_id = ChannelGbId(channel);
      if (gb_id.empty() || !IsValidGb28181Id(gb_id) ||
          (!channel.enabled && !config.include_disabled_channels))
        continue;
      CatalogChannel item;
      item.local_id = channel.local_id;
      item.device_id = gb_id;
      item.name = channel.name;
      item.enabled = channel.enabled;
      item.online = channel.enabled;
      item.manufacturer = channel.manufacturer.empty() ? "UvNVR" : channel.manufacturer;
      item.model = channel.model.empty() ? "UvNVR" : channel.model;
      item.civil_code = config.civil_code;
      item.address = config.address;
      out.push_back(std::move(item));
    }
    return out;
  }

  DeviceStatus DeviceStatusSnapshot() {
    DeviceStatus status;
    status.device_id = config.device_id;
    status.result = "OK";
    status.online = "ONLINE";
    status.status = "OK";
    status.encode = "ON";
    status.record = "ON";
    status.device_time = CurrentLocalIso8601();
    return status;
  }

  // --- frame queues -------------------------------------------------------

  bool EnqueueSend(const std::string& call_id, const std::uint8_t* ps, std::size_t size,
                   std::uint32_t timestamp, bool keyframe) {
    if (ps == nullptr || size == 0)
      return true;
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      if (first_frame_logged.insert(call_id).second)
        Log("first frame queued for " + call_id + " (" + std::to_string(size) + " bytes)");
    }
    Outbound frame;
    frame.call_id = call_id;
    frame.ps.assign(ps, ps + size);
    frame.timestamp_90k = timestamp;
    frame.keyframe = keyframe;
    std::lock_guard<std::mutex> lock(send_mutex);
    while (send_bytes + frame.ps.size() > kMaxSendQueueBytes && !send_queue.empty()) {
      send_bytes -= send_queue.front().ps.size();
      send_queue.pop_front();
    }
    send_bytes += frame.ps.size();
    send_queue.push_back(std::move(frame));
    return true;
  }

  void DrainSend() {
    std::deque<Outbound> frames;
    {
      std::lock_guard<std::mutex> lock(send_mutex);
      frames.swap(send_queue);
      send_bytes = 0;
    }
    if (!media)
      return;
    for (const Outbound& frame : frames) {
      std::string error;
      if (!media->SendFrame(frame.call_id, frame.ps.data(), frame.ps.size(), frame.timestamp_90k,
                            frame.keyframe, &error)) {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (send_error_logged.insert(frame.call_id).second)
          Log("send frame for " + frame.call_id + ": " + error);
      }
    }
  }

  void EnqueueLive(const std::string& call_id, const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0)
      return;
    std::shared_ptr<LiveSession> session;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      const auto it = live_sessions.find(call_id);
      if (it == live_sessions.end())
        return;
      session = it->second;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    while (session->buffered + size > kMaxLiveBufferBytes && !session->chunks.empty()) {
      session->buffered -= session->chunks.front().size();
      session->chunks.pop_front();
    }
    session->chunks.emplace_back(data, data + size);
    session->buffered += size;
  }

  void DrainLive() {
    std::vector<std::shared_ptr<LiveSession>> sessions;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      for (const auto& [id, session] : live_sessions) {
        (void)id;
        sessions.push_back(session);
      }
    }
    for (const auto& session : sessions) {
      std::deque<std::vector<std::uint8_t>> chunks;
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        chunks.swap(session->chunks);
        session->buffered = 0;
      }
      for (const std::vector<std::uint8_t>& chunk : chunks) {
        std::string error;
        if (session->pipeline && !session->pipeline->PushTs(chunk.data(), chunk.size(), &error)) {
          Log("live pipeline for " + session->call_id + ": " + error);
        }
      }
    }
  }

  // --- SIP callbacks ------------------------------------------------------

  void OnMessage(const SipMessage& message) {
    ++messages_in;
    if (message.body.empty())
      return;
    std::string response;
    std::string error;
    if (!responder) {
      Log("MANSCDP message before the responder was ready");
      return;
    }
    if (!responder->Handle(message.body, &response, &error)) {
      Log("MANSCDP request ignored: " + error);
      return;
    }
    if (response.empty())
      return;
    if (ua && !ua->SendMessage(response, std::string(kManscdpContentType), &error))
      Log("cannot send MANSCDP response: " + error);
    else
      ++messages_out;
  }

  // --- subscriptions and alarms ------------------------------------------

  struct Subscription {
    SipNotifyRequest notify;
    std::string event;
    std::uint64_t expires_at_ms = 0;
  };

  void OnSubscribe(const SipMessage& message) {
    ++messages_in;
    if (!ua)
      return;
    const std::string call_id = message.CallId();
    const std::string event = EventPackage(message.GetHeader("Event"));
    if (event.empty()) {
      ua->SendResponse(message, 400, "Bad Request", {}, {},
                       {{"Reason", "missing Event header"}}, {}, nullptr);
      Log("SUBSCRIBE without an Event header from call-id=" + call_id);
      return;
    }
    if (!EqualsIgnoreCase(event, "Catalog") && !EqualsIgnoreCase(event, "Alarm")) {
      ua->SendResponse(message, 489, "Bad Event", {}, {}, {}, SipDialogTag(call_id), nullptr);
      Log("SUBSCRIBE for unsupported event package '" + event + "' ignored");
      return;
    }

    std::uint32_t expires = 3600;
    const std::string expires_header = message.GetHeader("Expires");
    if (!expires_header.empty() && !ParseExpires(expires_header, &expires)) {
      ua->SendResponse(message, 400, "Bad Request", {}, {},
                       {{"Reason", "invalid Expires"}}, {}, nullptr);
      Log("SUBSCRIBE with invalid Expires '" + expires_header + "' ignored");
      return;
    }

    const std::string tag = SipDialogTag(call_id);
    std::string to = message.GetHeader("To");
    if (!to.empty() && to.find(";tag=") == std::string::npos)
      to += ";tag=" + tag;

    if (expires == 0) {
      ua->SendResponse(message, 200, "OK", {}, {}, {{"Expires", "0"}}, tag, nullptr);
      SipNotifyRequest terminated;
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(subscription_mutex);
        const auto it = subscriptions.find(call_id);
        if (it != subscriptions.end()) {
          terminated = it->second.notify;
          subscriptions.erase(it);
          found = true;
        }
      }
      if (found) {
        terminated.subscription_state = "terminated";
        terminated.expires_seconds = 0;
        terminated.body.clear();
        std::string error;
        if (!ua->SendNotify(terminated, &error))
          Log("cannot send terminating NOTIFY: " + error);
      }
      Log("SUBSCRIBE " + event + " unsubscribed (call-id=" + call_id + ")");
      return;
    }

    SipNotifyRequest notify;
    notify.call_id = call_id;
    notify.from = message.GetHeader("From");
    notify.to = to;
    notify.request_uri = ContactUri(message.GetHeader("Contact"));
    notify.event = event;
    notify.subscription_state = "active";
    notify.expires_seconds = expires;
    notify.content_type = std::string(kManscdpContentType);
    {
      std::lock_guard<std::mutex> lock(subscription_mutex);
      Subscription& stored = subscriptions[call_id];
      stored.notify = notify;
      stored.event = event;
      stored.expires_at_ms = MonotonicMs() + static_cast<std::uint64_t>(expires) * 1000;
    }

    const std::string expires_text = std::to_string(expires);
    ua->SendResponse(message, 200, "OK", {}, {},
                     {{"Expires", expires_text}, {"Event", event}}, tag, nullptr);
    ++messages_out;

    // Confirm the dialog immediately: Catalog carries the current catalog,
    // Alarm starts empty and is fed by ReportAlarm().
    if (EqualsIgnoreCase(event, "Catalog")) {
      notify.body = BuildCatalogNotifyBody();
    }
    std::string error;
    if (!ua->SendNotify(notify, &error))
      Log("cannot send initial NOTIFY: " + error);
    else
      ++messages_out;
    Log("SUBSCRIBE " + event + " active for " + expires_text + "s (call-id=" + call_id + ")");
  }

  void OnNotify(const SipMessage& message) {
    ++messages_in;
    Log("inbound NOTIFY (call-id=" + message.CallId() + ", " +
        std::to_string(message.body.size()) + " body bytes)");
  }

  std::string BuildCatalogNotifyBody() {
    CatalogMapResult mapped;
    if (responder)
      mapped = responder->CatalogSnapshot();
    if (!mapped.ok)
      return {};
    CatalogResponse response;
    response.sn = ++catalog_sn;
    response.device_id = config.device_id;
    response.devices = std::move(mapped.items);
    response.sum_num = static_cast<std::uint32_t>(response.devices.size());
    return BuildCatalogResponse(response);
  }

  void ExpireSubscriptions() {
    const std::uint64_t now = MonotonicMs();
    std::vector<SipNotifyRequest> expired;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex);
      for (auto it = subscriptions.begin(); it != subscriptions.end();) {
        if (it->second.expires_at_ms > now) {
          ++it;
          continue;
        }
        SipNotifyRequest notify = it->second.notify;
        notify.subscription_state = "terminated";
        notify.expires_seconds = 0;
        notify.body.clear();
        expired.push_back(std::move(notify));
        it = subscriptions.erase(it);
      }
    }
    for (const SipNotifyRequest& notify : expired) {
      std::string error;
      if (ua && !ua->SendNotify(notify, &error))
        Log("cannot send expiry NOTIFY: " + error);
      Log("subscription expired (call-id=" + notify.call_id + ")");
    }
  }

  std::string AlarmDeviceId(std::int64_t channel_id) const {
    if (channel_id > 0) {
      GbChannel channel;
      if (FindChannelByLocal(channel_id, &channel)) {
        const std::string id = ChannelGbId(channel);
        if (!id.empty())
          return id;
      }
    }
    return config.device_id;
  }

  void PublishAlarm(const GbAlarm& alarm) {
    AlarmNotify message;
    message.sn = ++alarm_sn;
    message.device_id = AlarmDeviceId(alarm.channel_id);
    message.priority = alarm.priority;
    message.method = alarm.method;
    message.alarm_type = alarm.alarm_type;
    message.alarm_type_param = alarm.alarm_type_param;
    message.description = alarm.description;
    const std::int64_t time_ms =
        alarm.time_ms > 0
            ? alarm.time_ms
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    message.time = FormatGbTime(time_ms);
    const std::string body = BuildAlarmNotify(message);

    if (ua) {
      std::string error;
      if (!ua->SendMessage(body, std::string(kManscdpContentType), &error)) {
        Log("cannot send Alarm MESSAGE: " + error);
      } else {
        ++messages_out;
      }
    }

    std::vector<SipNotifyRequest> subscribers;
    const std::uint64_t now = MonotonicMs();
    {
      std::lock_guard<std::mutex> lock(subscription_mutex);
      for (const auto& [call_id, subscription] : subscriptions) {
        (void)call_id;
        if (!EqualsIgnoreCase(subscription.event, "Alarm") ||
            subscription.expires_at_ms <= now)
          continue;
        SipNotifyRequest notify = subscription.notify;
        notify.body = body;
        notify.content_type = std::string(kManscdpContentType);
        subscribers.push_back(std::move(notify));
      }
    }
    for (const SipNotifyRequest& notify : subscribers) {
      std::string error;
      if (ua && ua->SendNotify(notify, &error))
        ++messages_out;
      else
        Log("cannot send Alarm NOTIFY: " + error);
    }
    alarms_sent.fetch_add(1);
    Log("alarm published (SN=" + std::to_string(message.sn) + ", device=" +
        message.device_id + ", subscribers=" + std::to_string(subscribers.size()) + ")");
  }

  void DrainAlarms() {
    std::deque<GbAlarm> alarms;
    {
      std::lock_guard<std::mutex> lock(alarm_mutex);
      alarms.swap(alarm_queue);
    }
    for (const GbAlarm& alarm : alarms)
      PublishAlarm(alarm);
  }

  void OnInvite(const SipMessage& message, const std::string& peer) {
    ++messages_in;
    const std::string call_id = message.CallId();
    Log("INVITE from " + peer + " call-id=" + call_id);

    const std::string cseq = message.GetHeader("CSeq");
    bool retransmission = false;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      const auto it = dialog_cseq.find(call_id);
      retransmission = it != dialog_cseq.end() && it->second == cseq;
      if (!retransmission)
        dialog_cseq[call_id] = cseq;
    }
    // A new INVITE with a different CSeq replaces an established live or
    // playback dialog; a retransmission keeps streaming untouched.
    if (!retransmission)
      StopSessionSide(call_id);

    SdpVideoSession offer;
    std::string error;
    if (!ParseSdp(message.body, &offer, &error)) {
      ua->SendResponse(message, 488, "Not Acceptable Here", {}, {}, {}, {}, nullptr);
      Log("reject INVITE: " + error);
      return;
    }

    std::string channel_id;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    bool is_playback = false;
    if (!ParsePlaybackSdp(message.body, &channel_id, &start_ms, &end_ms, &is_playback, &error)) {
      ua->SendResponse(message, 400, "Bad Request", {}, {}, {}, {}, nullptr);
      Log("reject INVITE: " + error);
      return;
    }
    if (channel_id.empty())
      channel_id = offer.origin_id;

    GbChannel channel;
    if (!FindChannelByGb(channel_id, &channel) || !channel.enabled) {
      ua->SendResponse(message, 404, "Not Found", {}, {}, {}, {}, nullptr);
      Log("INVITE for unknown or disabled channel " + channel_id);
      return;
    }

    std::string stream = config.default_stream.empty() ? "main" : config.default_stream;
    std::int64_t range_start = start_ms;
    std::int64_t range_end = end_ms;
    if (is_playback) {
      if (range_start <= 0) {
        ua->SendResponse(message, 488, "Not Acceptable Here", {}, {}, {}, {}, nullptr);
        Log("playback INVITE has no Range in u=");
        return;
      }
      if (range_end <= range_start)
        range_end = range_start + 3600LL * 1000;
      // Pick the stream that actually has an index row in this range, so a
      // sub-stream-only channel does not play silence on a main-stream request.
      if (callbacks.records) {
        const auto preferred = callbacks.records(channel.local_id, stream, range_start, range_end);
        if (preferred.empty() && !config.fallback_stream.empty() &&
            config.fallback_stream != stream) {
          const auto fallback =
              callbacks.records(channel.local_id, config.fallback_stream, range_start, range_end);
          if (!fallback.empty())
            stream = config.fallback_stream;
        }
      }
    }

    MediaInviteRequest media_request;
    media_request.call_id = call_id;
    media_request.cseq = message.GetHeader("CSeq");
    media_request.from = message.GetHeader("From");
    media_request.to = message.GetHeader("To");
    media_request.content_type = message.GetHeader("Content-Type");
    media_request.body = message.body;
    MediaInviteResponse media_response;
    if (!media || !media->OnInvite(media_request, &media_response, &error)) {
      ua->SendResponse(message, 500, "Server Internal Error", {}, {}, {}, {}, nullptr);
      Log("media INVITE failed: " + error);
      return;
    }

    if (media_response.status_code != 200) {
      ua->SendResponse(message, media_response.status_code, media_response.reason, {}, {}, {}, {},
                       nullptr);
      Log("media INVITE rejected with " + std::to_string(media_response.status_code));
      return;
    }

    ua->SendResponse(message, 200, "OK", media_response.sdp, "application/sdp", {}, {}, nullptr);
    ++messages_out;

    Pending pending;
    pending.playback = is_playback;
    pending.channel_id = channel.local_id;
    pending.stream = stream;
    pending.request.call_id = call_id;
    pending.request.channel_id = channel.local_id;
    pending.request.stream = stream;
    pending.request.start_ms = range_start;
    pending.request.end_ms = range_end;
    pending.request.speed = 1.0;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      pending_sessions[call_id] = pending;
    }
    SetState(is_playback ? "playback negotiated" : "live negotiated");
  }

  void OnAck(const SipMessage& message) {
    ++messages_in;
    const std::string call_id = message.CallId();
    std::string error;
    if (media && !media->OnAck(call_id, &error))
      Log("ACK for " + call_id + ": " + error);
    if (media)
      media->Tick(MonotonicMs(), nullptr);

    Pending pending;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      const auto it = pending_sessions.find(call_id);
      if (it == pending_sessions.end())
        return;
      pending = it->second;
      pending_sessions.erase(it);
    }

    if (pending.playback) {
      if (!playback || !playback->Start(pending.request, &error)) {
        Log("cannot start playback for " + call_id + ": " + error);
        if (media)
          media->Close(call_id, "playback start failed", true, nullptr);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        playback_sessions.insert(call_id);
      }
      Log("playback started " + call_id + " channel=" + std::to_string(pending.channel_id) +
          " stream=" + pending.stream + " range=" + std::to_string(pending.request.start_ms) +
          ".." + std::to_string(pending.request.end_ms));
      SetState("playback streaming");
      return;
    }

    auto live = std::make_shared<LiveSession>();
    live->call_id = call_id;
    live->channel_id = pending.channel_id;
    live->stream = pending.stream;
    const std::string call = call_id;
    std::string pipeline_error;
    PsRtpPipelineConfig pipeline_config;
    pipeline_config.rebase_timestamps = false;
    live->pipeline = std::make_unique<PsRtpPipeline>(
        [this, call](const std::uint8_t* ps, std::size_t size, std::uint32_t timestamp,
                     bool keyframe,
                     std::string*) { return EnqueueSend(call, ps, size, timestamp, keyframe); },
        pipeline_config);
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      live_sessions[call_id] = live;
    }
    if (!callbacks.live_open) {
      Log("no live stream provider configured");
      if (media)
        media->Close(call_id, "live stream unavailable", true, nullptr);
      return;
    }
    std::uint64_t subscription = callbacks.live_open(
        pending.channel_id, pending.stream,
        [this, call](const std::uint8_t* data, std::size_t size) { EnqueueLive(call, data, size); },
        &pipeline_error);
    if (subscription == 0) {
      Log("cannot subscribe live stream for " + call_id + ": " + pipeline_error);
      {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        live_sessions.erase(call_id);
      }
      if (media)
        media->Close(call_id, "live stream unavailable", true, nullptr);
      return;
    }
    live->subscription = subscription;
    SetState("live streaming");
  }

  void OnBye(const SipMessage& message) {
    ++messages_in;
    const std::string call_id = message.CallId();
    if (media) {
      std::string error;
      media->OnBye(call_id, &error);
    }
    // The media manager already sent RTCP BYE; do not close it twice.
    CloseSession(call_id, "BYE received", false);
  }

  void OnInfo(const SipMessage& message) {
    ++messages_in;
    const std::string call_id = message.CallId();
    PlaybackControl control;
    std::string error;
    if (!ParsePlaybackControl(message.body, &control, &error)) {
      if (ua)
        ua->SendResponse(message, 400, "Bad Request", {}, {}, {}, {}, nullptr);
      Log("INFO rejected: " + error);
      return;
    }

    bool is_playback = false;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      is_playback = playback_sessions.find(call_id) != playback_sessions.end();
    }
    if (!is_playback) {
      // A live dialog may also receive an INFO; acknowledge and ignore it
      // rather than breaking the established stream.
      if (ua)
        ua->SendResponse(message, 200, "OK", {}, {}, {}, {}, nullptr);
      ++messages_out;
      return;
    }

    if (control.has_range && playback) {
      std::int64_t target = control.range_start_ms;
      if (target <= 0)
        target = control.range_end_ms;
      if (target > 0 && !playback->Seek(call_id, target, &error))
        Log("playback seek for " + call_id + ": " + error);
    }
    if (control.has_scale) {
      if (control.scale > 0.0) {
        if (!playback->SetSpeed(call_id, control.scale, &error))
          Log("playback speed for " + call_id + ": " + error);
      } else if (control.scale == 0.0) {
        if (!playback->Pause(call_id, true, &error))
          Log("playback pause for " + call_id + ": " + error);
      } else {
        // Reverse playback is not implemented. Answering 200 would imply the
        // request took effect, so report 488 and keep the current direction.
        if (ua)
          ua->SendResponse(message, 488, "Not Acceptable Here", {}, {}, {}, {}, nullptr);
        Log("reverse playback (Scale < 0) is not supported; keeping forward playback");
        return;
      }
    }
    if (ua)
      ua->SendResponse(message, 200, "OK", {}, {}, {}, {}, nullptr);
    ++messages_out;
  }

  // Stops the media producer for a Call-ID but leaves the RTP session to the
  // caller. A re-INVITE replaces the media session in MediaSessionManager and
  // must not tear down the new one while doing so.
  void StopSessionSide(const std::string& call_id) {
    std::shared_ptr<LiveSession> live;
    bool was_playback = false;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      const auto live_it = live_sessions.find(call_id);
      if (live_it != live_sessions.end()) {
        live = live_it->second;
        live_sessions.erase(live_it);
      }
      was_playback = playback_sessions.erase(call_id) > 0;
      pending_sessions.erase(call_id);
    }
    if (live) {
      if (callbacks.live_close && live->subscription != 0)
        callbacks.live_close(live->channel_id, live->stream, live->subscription);
      live->pipeline.reset();
    }
    if (was_playback && playback)
      playback->Stop(call_id);
  }

  void CloseSession(const std::string& call_id, const std::string& reason,
                    bool close_media = true) {
    StopSessionSide(call_id);
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      dialog_cseq.erase(call_id);
    }
    if (close_media && media) {
      std::string error;
      media->Close(call_id, reason, true, &error);
    }
    Log("closed dialog " + call_id + ": " + reason);
    SetState("dialog closed");
  }

  void OnPlaybackFinished(const std::string& call_id, const std::string& reason) {
    std::lock_guard<std::mutex> lock(finished_mutex);
    finished_queue.emplace_back(call_id, reason);
  }

  void DrainFinished() {
    std::deque<std::pair<std::string, std::string>> finished;
    {
      std::lock_guard<std::mutex> lock(finished_mutex);
      finished.swap(finished_queue);
    }
    for (const auto& [call_id, reason] : finished) {
      bool known = false;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        known = playback_sessions.erase(call_id) > 0;
        pending_sessions.erase(call_id);
      }
      if (!known)
        continue;
      if (playback)
        playback->Stop(call_id);
      if (media) {
        std::string error;
        media->Close(call_id, reason, true, &error);
      }
      Log("playback finished for " + call_id + ": " + reason);
      SetState("playback finished");
    }
  }

  void SetState(const std::string& new_state) {
    std::lock_guard<std::mutex> lock(status_mutex);
    state = new_state;
  }

  void Tick() {
    if (media) {
      std::string error;
      media->PollControl(std::string(), 0, &error);
      media->Tick(MonotonicMs(), &error);
    }
    DrainLive();
    DrainSend();
    DrainFinished();
    DrainAlarms();
    ExpireSubscriptions();
  }

  static void OnTick(uv_timer_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    if (self != nullptr)
      self->Tick();
  }

  static void OnStopAsync(uv_async_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    if (self == nullptr)
      return;
    if (self->loop_handle != nullptr)
      uv_stop(self->loop_handle);
  }

  void Run(std::promise<std::string>* ready) {
    auto loop = std::make_unique<uvcpp::uvcpp_loop>();
    if (loop->init() != 0) {
      ready->set_value("cannot initialise the GB28181 event loop");
      return;
    }
    loop_ = std::move(loop);
    loop_handle = reinterpret_cast<uv_loop_t*>(loop_->get_handle());

    auto* timer = new uv_timer_t();
    if (uv_timer_init(loop_handle, timer) != 0) {
      delete timer;
      ready->set_value("cannot initialise the GB28181 timer");
      Cleanup();
      return;
    }
    tick_timer = timer;
    tick_timer->data = this;
    uv_timer_start(tick_timer, &Impl::OnTick, 20, 20);

    auto* async = new uv_async_t();
    if (uv_async_init(loop_handle, async, &Impl::OnStopAsync) != 0) {
      delete async;
      ready->set_value("cannot initialise the GB28181 stop handle");
      Cleanup();
      return;
    }
    stop_async = async;
    stop_async->data = this;

    SipUaConfig ua_config;
    ua_config.server_host = config.server_host;
    ua_config.server_port = config.server_port;
    ua_config.tcp = config.tcp;
    ua_config.username = config.username.empty() ? config.device_id : config.username;
    ua_config.password = config.password;
    ua_config.server_id = config.server_id;
    ua_config.domain = config.domain;
    ua_config.local_ip = config.local_ip;
    ua_config.bind_ip = config.bind_ip;
    ua_config.local_port = config.local_port;
    ua_config.expires_seconds = config.expires_seconds;
    ua_config.keepalive_seconds = config.keepalive_seconds;
    ua = std::make_unique<SipUserAgent>(loop_.get(), std::move(ua_config));

    SipUaCallbacks ua_callbacks;
    ua_callbacks.on_state = [this](SipRegistrationState state, const std::string& detail) {
      std::lock_guard<std::mutex> lock(status_mutex);
      registration_state = state;
      if (!detail.empty())
        state_detail = detail;
    };
    ua_callbacks.on_message = [this](const SipMessage& message) { OnMessage(message); };
    ua_callbacks.on_invite = [this](const SipMessage& message, const std::string& peer) {
      OnInvite(message, peer);
    };
    ua_callbacks.on_ack = [this](const SipMessage& message) { OnAck(message); };
    ua_callbacks.on_bye = [this](const SipMessage& message) { OnBye(message); };
    ua_callbacks.on_info = [this](const SipMessage& message) { OnInfo(message); };
    ua_callbacks.on_subscribe = [this](const SipMessage& message) { OnSubscribe(message); };
    ua_callbacks.on_notify = [this](const SipMessage& message) { OnNotify(message); };
    ua_callbacks.on_event = [this](const std::string& text) { Log(text); };
    ua->set_callbacks(std::move(ua_callbacks));

    MediaSessionConfig media_config;
    media_config.device_id = config.device_id;
    media_config.advertised_address = config.local_ip;
    media_config.bind_address = config.bind_ip.empty() ? "0.0.0.0" : config.bind_ip;
    media_config.limits.rtp_port_min = config.rtp_port_min;
    media_config.limits.rtp_port_max = config.rtp_port_max;
    media_config.limits.max_sessions = config.max_sessions;
    media_config.on_state = [this](const std::string& call_id, MediaSessionState state,
                                   const std::string& detail) {
      Log("media " + call_id + " -> " + MediaSessionStateName(state) + " " + detail);
    };
    media = std::make_unique<MediaSessionManager>(media_config);

    ManscdpDeviceConfig manscdp_config;
    manscdp_config.device_id = config.device_id;
    manscdp_config.device_name = config.device_name;
    manscdp_config.device_type = "118";
    manscdp_config.manufacturer = "UvNVR";
    manscdp_config.model = "UvNVR";
    manscdp_config.firmware = "0.19.0";
    manscdp_config.civil_code = config.civil_code;
    manscdp_config.owner = config.owner;
    manscdp_config.address = config.address;
    manscdp_config.secrecy = config.secrecy;
    responder = std::make_unique<ManscdpResponder>(manscdp_config);
    responder->set_channel_source([this] { return CatalogChannels(); });
    responder->set_group_source([] { return std::vector<CatalogGroup>{}; });
    responder->set_status_source([this] { return DeviceStatusSnapshot(); });
    responder->set_record_source(
        [this](const RecordInfoQuery& query) { return HandleRecordInfo(query); });
    responder->set_ptz_handler([this](const PtzControl& command, std::string* error) {
      GbChannel channel;
      if (!FindChannelByGb(command.device_id, &channel)) {
        if (error != nullptr)
          *error = "PTZ command for unknown channel " + command.device_id;
        return false;
      }
      if (!channel.enabled) {
        if (error != nullptr)
          *error = "PTZ command for disabled channel " + command.device_id;
        return false;
      }
      if (!callbacks.ptz_control) {
        if (error != nullptr)
          *error = "no PTZ backend is configured for channel " + command.device_id;
        return false;
      }
      std::string dispatch_error;
      if (!callbacks.ptz_control(channel.local_id, command, &dispatch_error)) {
        if (error != nullptr)
          *error = dispatch_error.empty() ? "PTZ backend rejected the command" : dispatch_error;
        return false;
      }
      Log("PTZ " + std::string(PtzActionName(command.command.action)) + " -> channel " +
          std::to_string(channel.local_id) + " (GB " + command.device_id + ")");
      return true;
    });

    SegmentReadFn reader = callbacks.read_segment ? callbacks.read_segment : ReadPlainSegment;
    playback = std::make_unique<PlaybackController>(
        [this](std::int64_t channel_id, const std::string& stream, std::int64_t start,
               std::int64_t end) {
          return callbacks.records ? callbacks.records(channel_id, stream, start, end)
                                   : std::vector<PlaybackSegment>{};
        },
        reader,
        [this](const std::string& call_id, const std::uint8_t* ps, std::size_t size,
               std::uint32_t timestamp, bool keyframe,
               std::string*) { return EnqueueSend(call_id, ps, size, timestamp, keyframe); },
        config.playback);
    playback->set_finished_callback([this](const std::string& call_id, const std::string& reason) {
      OnPlaybackFinished(call_id, reason);
    });

    std::string start_error;
    if (!ua->Start(&start_error)) {
      ready->set_value(start_error);
      Cleanup();
      return;
    }
    local_ip = ua->local_ip();
    local_port = ua->local_port();
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      state = "registered (waiting for the platform)";
    }
    running = true;
    ready->set_value({});

    loop_->run();

    running = false;
    Cleanup();
  }

  void Cleanup() {
    // Close live subscriptions first: their sink may be running on the media
    // loop and must stop before the service objects go away.
    std::map<std::string, std::shared_ptr<LiveSession>> live;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      live.swap(live_sessions);
      playback_sessions.clear();
      pending_sessions.clear();
    }
    for (const auto& [id, session] : live) {
      (void)id;
      if (callbacks.live_close && session->subscription != 0)
        callbacks.live_close(session->channel_id, session->stream, session->subscription);
      session->pipeline.reset();
    }
    {
      std::lock_guard<std::mutex> lock(subscription_mutex);
      subscriptions.clear();
    }
    {
      std::lock_guard<std::mutex> lock(alarm_mutex);
      alarm_queue.clear();
    }
    if (playback)
      playback->StopAll();
    if (media) {
      for (const std::string& call_id : media->call_ids()) {
        std::string error;
        media->Close(call_id, "service stopping", true, &error);
      }
    }
    if (ua)
      ua->Stop();

    if (loop_handle != nullptr) {
      if (tick_timer != nullptr) {
        uv_timer_stop(tick_timer);
        tick_timer->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(tick_timer),
                 [](uv_handle_t* handle) { delete reinterpret_cast<uv_timer_t*>(handle); });
        tick_timer = nullptr;
      }
      if (stop_async != nullptr) {
        stop_async->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(stop_async),
                 [](uv_handle_t* handle) { delete reinterpret_cast<uv_async_t*>(handle); });
        stop_async = nullptr;
      }
      ua.reset();
      media.reset();
      responder.reset();
      playback.reset();
      // Let the close callbacks run. uv_stop left stop_flag set, which would
      // make uv_run return immediately without draining the close queue.
      loop_handle->stop_flag = 0;
      for (int i = 0; i < 100 && uv_loop_alive(loop_handle) != 0; ++i)
        uv_run(loop_handle, UV_RUN_NOWAIT);
      loop_->loop_close();
      loop_.reset();
      loop_handle = nullptr;
    }
  }

  Gb28181Config config;
  Gb28181Callbacks callbacks;

  std::thread thread;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  std::unique_ptr<uvcpp::uvcpp_loop> loop_;
  uv_loop_t* loop_handle = nullptr;
  uv_timer_t* tick_timer = nullptr;
  uv_async_t* stop_async = nullptr;

  std::unique_ptr<SipUserAgent> ua;
  std::unique_ptr<MediaSessionManager> media;
  std::unique_ptr<ManscdpResponder> responder;
  std::unique_ptr<PlaybackController> playback;

  mutable std::mutex subscription_mutex;
  std::map<std::string, Subscription> subscriptions;
  std::uint64_t catalog_sn = 0;
  std::uint64_t alarm_sn = 0;

  mutable std::mutex alarm_mutex;
  std::deque<GbAlarm> alarm_queue;

  mutable std::mutex sessions_mutex;
  std::map<std::string, Pending> pending_sessions;
  std::map<std::string, std::shared_ptr<LiveSession>> live_sessions;
  std::set<std::string> playback_sessions;
  std::map<std::string, std::string> dialog_cseq;

  mutable std::mutex send_mutex;
  std::deque<Outbound> send_queue;
  std::size_t send_bytes = 0;

  mutable std::mutex finished_mutex;
  std::deque<std::pair<std::string, std::string>> finished_queue;

  mutable std::mutex status_mutex;
  std::string state = "stopped";
  std::string state_detail;
  SipRegistrationState registration_state = SipRegistrationState::kStopped;
  std::string local_ip;
  std::uint16_t local_port = 0;
  std::atomic<std::uint64_t> messages_in{0};
  std::atomic<std::uint64_t> messages_out{0};
  std::atomic<std::uint64_t> alarms_sent{0};
  std::set<std::string> first_frame_logged;
  std::set<std::string> send_error_logged;
};

Gb28181Service::Gb28181Service(Gb28181Config config, Gb28181Callbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}

Gb28181Service::~Gb28181Service() {
  Stop();
}

bool Gb28181Service::Start(std::string* error) {
  std::string config_error;
  if (!IsValidGb28181Id(impl_->config.device_id, &config_error)) {
    if (error != nullptr)
      *error = "gb28181.device_id is invalid: " + config_error;
    return false;
  }
  if (impl_->config.server_host.empty()) {
    if (error != nullptr)
      *error = "gb28181.server_host is empty";
    return false;
  }
  if (impl_->thread.joinable()) {
    if (error != nullptr)
      *error = "gb28181 service is already running";
    return false;
  }

  std::promise<std::string> ready;
  std::future<std::string> future = ready.get_future();
  impl_->stop_requested.store(false);
  impl_->thread = std::thread([impl = impl_.get(), &ready] { impl->Run(&ready); });
  if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    impl_->stop_requested.store(true);
    if (impl_->stop_async != nullptr)
      uv_async_send(impl_->stop_async);
    impl_->thread.join();
    if (error != nullptr)
      *error = "timed out starting the GB28181 service";
    return false;
  }
  const std::string result = future.get();
  if (!result.empty()) {
    impl_->thread.join();
    if (error != nullptr)
      *error = result;
    return false;
  }
  return true;
}

void Gb28181Service::Stop() {
  if (!impl_ || !impl_->thread.joinable())
    return;
  impl_->stop_requested.store(true);
  if (impl_->stop_async != nullptr)
    uv_async_send(impl_->stop_async);
  impl_->thread.join();
}

Gb28181Service::Status Gb28181Service::GetStatus() const {
  Status status;
  status.running = impl_->running.load();
  {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    status.state = impl_->state;
    status.local_ip = impl_->local_ip;
    status.local_port = impl_->local_port;
    status.registered = impl_->registration_state == SipRegistrationState::kRegistered;
  }
  status.messages_in = impl_->messages_in.load();
  status.messages_out = impl_->messages_out.load();
  status.alarms_sent = impl_->alarms_sent.load();
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    status.live_sessions = impl_->live_sessions.size();
    status.playback_sessions = impl_->playback_sessions.size();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->subscription_mutex);
    for (const auto& [call_id, subscription] : impl_->subscriptions) {
      (void)call_id;
      if (EqualsIgnoreCase(subscription.event, "Alarm"))
        ++status.alarm_subscriptions;
    }
  }
  return status;
}

bool Gb28181Service::ReportAlarm(const GbAlarm& alarm) {
  if (!impl_ || !impl_->running.load())
    return false;
  {
    std::lock_guard<std::mutex> lock(impl_->alarm_mutex);
    impl_->alarm_queue.push_back(alarm);
  }
  return true;
}

bool Gb28181Service::ParsePlaybackSdp(const std::string& sdp, std::string* channel_id,
                                      std::int64_t* start_ms, std::int64_t* end_ms,
                                      bool* is_playback, std::string* error) {
  bool playback = false;
  std::string uri;
  for (const std::string& line : SplitLines(sdp)) {
    if (line.size() < 2 || line[1] != '=')
      continue;
    const std::string value = line.substr(2);
    if (line[0] == 's' && EqualsIgnoreCase(value, "Playback"))
      playback = true;
    else if (line[0] == 's' && EqualsIgnoreCase(value, "Download"))
      playback = false;
    else if (line[0] == 'u')
      uri = value;
  }
  if (!uri.empty()) {
    playback = true;
    const std::size_t first = uri.find(':');
    const std::size_t second =
        first == std::string::npos ? std::string::npos : uri.find(':', first + 1);
    if (first == std::string::npos) {
      if (error != nullptr)
        *error = "playback u= must contain <channel>:<start>:<end>";
      return false;
    }
    if (channel_id != nullptr)
      *channel_id = uri.substr(0, first);
    if (second == std::string::npos) {
      if (error != nullptr)
        *error = "playback u= has no start/end";
      return false;
    }
    std::int64_t start = 0;
    std::string time_error;
    if (!ParseGbCompactTime(uri.substr(first + 1, second - first - 1), &start, &time_error)) {
      if (error != nullptr)
        *error = "playback u= start: " + time_error;
      return false;
    }
    std::int64_t end = start + 3600LL * 1000;
    const std::string end_text = uri.substr(second + 1);
    if (!end_text.empty() && !ParseGbCompactTime(end_text, &end, &time_error)) {
      if (error != nullptr)
        *error = "playback u= end: " + time_error;
      return false;
    }
    if (start_ms != nullptr)
      *start_ms = start;
    if (end_ms != nullptr)
      *end_ms = end;
  }
  if (is_playback != nullptr)
    *is_playback = playback;
  return true;
}

} // namespace uvnvr::gb28181
