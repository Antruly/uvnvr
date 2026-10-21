#include "onvif_events.h"

#include "onvif.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uvnvr {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Lower(std::string value) {
  for (char& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool Truthy(const std::string& value) {
  if (value.empty())
    return true;
  const std::string lowered = Lower(value);
  return lowered == "true" || lowered == "1" || lowered == "on" || lowered == "active";
}

std::string AlertType(const std::string& topic) {
  if (topic.find("PeopleDetector") != std::string::npos ||
      topic.find("HumanDetector") != std::string::npos ||
      topic.find("ObjectDetector") != std::string::npos) {
    return "person";
  }
  if (topic.find("TamperDetector") != std::string::npos)
    return "tamper";
  if (topic.find("LineDetector") != std::string::npos ||
      topic.find("FieldDetector") != std::string::npos)
    return "intrusion";
  if (topic.find("MotionDetector") != std::string::npos ||
      topic.find("/Motion") != std::string::npos || topic.find("Motion") != std::string::npos)
    return "motion";
  return {};
}

std::string AlertMessage(const std::string& type) {
  if (type == "person")
    return "ONVIF camera reported a person";
  if (type == "tamper")
    return "ONVIF camera reported tampering";
  if (type == "intrusion")
    return "ONVIF camera reported a line/field crossing";
  return "ONVIF camera reported motion";
}

} // namespace

struct OnvifEventService::Impl {
  struct Poller : std::enable_shared_from_this<Poller> {
    OnvifEventConfig config;
    AlertFn alert;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::string event_url;
    std::string subscription;
    int64_t subscription_started_ms = 0;
    std::map<std::string, int64_t> last_alert_ms;

    void Handle(const OnvifEvent& event) {
      if (!Truthy(event.value))
        return;
      const std::string type = AlertType(event.topic);
      if (type.empty())
        return;
      const int64_t now = NowMs();
      const auto previous = last_alert_ms.find(type);
      if (previous != last_alert_ms.end() && now - previous->second < 60 * 1000)
        return;
      last_alert_ms[type] = now;
      if (alert)
        alert(config.channel_id, type, AlertMessage(type));
    }

    void Run() {
      while (!stop.load()) {
        if (event_url.empty()) {
          std::string error;
          if (!ResolveOnvifEventUrl(config.host, config.port, config.username, config.password,
                                    &event_url, &error)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
          }
        }
        if (subscription.empty()) {
          OnvifEventSubscription created;
          std::string error;
          if (!CreateOnvifEventSubscription(event_url, config.username, config.password, &created,
                                            &error)) {
            event_url.clear();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
          }
          subscription = created.address;
          subscription_started_ms = NowMs();
        }
        std::vector<OnvifEvent> events;
        std::string error;
        if (!PullOnvifEvents(subscription, config.username, config.password, 2000, &events,
                             &error)) {
          subscription.clear();
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
        for (const OnvifEvent& event : events)
          Handle(event);
        // PullPoint subscriptions expire; recreate one well before the
        // requested 120 s termination time.
        if (NowMs() - subscription_started_ms > 90 * 1000)
          subscription.clear();
        if (events.empty())
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
    }
  };

  explicit Impl(AlertFn alert_in) : alert(std::move(alert_in)) {}
  ~Impl() {
    Stop();
  }

  void Upsert(const OnvifEventConfig& config) {
    Remove(config.channel_id);
    auto poller = std::make_shared<Poller>();
    poller->config = config;
    poller->alert = alert;
    poller->thread = std::thread([poller] { poller->Run(); });
    std::lock_guard<std::mutex> lock(mutex);
    pollers[config.channel_id] = std::move(poller);
  }

  void Remove(int64_t channel_id) {
    std::shared_ptr<Poller> poller;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = pollers.find(channel_id);
      if (it == pollers.end())
        return;
      poller = it->second;
      pollers.erase(it);
    }
    poller->stop = true;
    if (poller->thread.joinable())
      poller->thread.join();
  }

  void Stop() {
    std::unordered_map<int64_t, std::shared_ptr<Poller>> active;
    {
      std::lock_guard<std::mutex> lock(mutex);
      active.swap(pollers);
    }
    for (auto& [id, poller] : active) {
      (void)id;
      poller->stop = true;
    }
    for (auto& [id, poller] : active) {
      (void)id;
      if (poller->thread.joinable())
        poller->thread.join();
    }
  }

  AlertFn alert;
  std::mutex mutex;
  std::unordered_map<int64_t, std::shared_ptr<Poller>> pollers;
};

OnvifEventService::OnvifEventService(AlertFn alert)
    : impl_(std::make_unique<Impl>(std::move(alert))) {}

OnvifEventService::~OnvifEventService() {
  impl_->Stop();
}

void OnvifEventService::Upsert(const OnvifEventConfig& config) {
  impl_->Upsert(config);
}

void OnvifEventService::Remove(int64_t channel_id) {
  impl_->Remove(channel_id);
}

void OnvifEventService::Stop() {
  impl_->Stop();
}

} // namespace uvnvr
