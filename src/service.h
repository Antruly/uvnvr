#pragma once

#include "config.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace uvnvr::service {

// State of the platform service registration (Windows SCM or systemd).
struct Status {
  bool supported = false;
  bool installed = false;
  bool running = false;
  bool autostart = false;
  // True when the process answering this query is the service itself.
  bool running_as_service = false;
  std::string platform;
  std::string manager;
  std::string detail;
};

Status Query();
nlohmann::json StatusJson();

// Registers the service with automatic startup. It is left stopped; callers
// schedule Start() after this process exits so the ports are free.
bool Install(const Config& config, std::string* error);

// Removes the registration. When this process is the service, only the
// registration is removed (stopping ourselves would deadlock); the caller
// shuts down afterwards. An already-running console is never touched.
bool Uninstall(const Config& config, std::string* error);

// Spawns a detached helper that waits for this process to exit, then starts
// the registered service or launches an interactive console instance.
void ScheduleStart(const Config& config);
void ScheduleConsole(const Config& config);

// Entry point for `--service-action start|console` helpers.
void PerformAction(const std::string& action, const Config& config);

// Reports SERVICE_RUNNING to the SCM once the application is ready.
void NotifyStarted();

#ifdef _WIN32
struct ServiceHooks {
  std::function<int()> run;
  std::function<void()> request_stop;
};
// Blocks until the SCM stops the service. `run` performs normal startup and
// `request_stop` must make it return.
int RunWindowsService(const Config& config, const ServiceHooks& hooks);
#endif

} // namespace uvnvr::service
