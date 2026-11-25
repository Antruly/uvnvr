// ONVIF PTZ (ver20) request tests against an in-process fake camera.
//
// The fake camera implements just enough of the device/media/PTZ services for
// OnvifPtzControl(): GetCapabilities (Media and PTZ), GetProfiles,
// ContinuousMove, Stop, SetPreset, GotoPreset and RemovePreset. Every SOAP
// body is recorded so the test can assert on the actual XML that the camera
// receives, not only on the return value.

#include "onvif.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void Expect(const std::string& name, bool condition, const std::string& detail = {}) {
  if (condition)
    return;
  std::fprintf(stderr, "FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ",
               detail.c_str());
  ++failures;
}

void ExpectContains(const std::string& name, const std::string& haystack,
                    const std::string& needle) {
  Expect(name, haystack.find(needle) != std::string::npos,
         "does not contain '" + needle + "'");
}

std::string StripTag(const std::string& xml, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  const std::size_t begin = xml.find(open);
  if (begin == std::string::npos)
    return {};
  const std::size_t end = xml.find(close, begin);
  if (end == std::string::npos)
    return {};
  return xml.substr(begin + open.size(), end - begin - open.size());
}

class FakeOnvifCamera {
public:
  FakeOnvifCamera() {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      return;
    socklen_t length = sizeof(address);
    getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    ::listen(socket_, 8);
    thread_ = std::thread([this] { Serve(); });
  }

  ~FakeOnvifCamera() {
    stop_.store(true);
    if (socket_ >= 0)
      ::shutdown(socket_, SHUT_RDWR);
    if (thread_.joinable())
      thread_.join();
    if (socket_ >= 0)
      ::close(socket_);
  }

  int port() const {
    return port_;
  }

  std::vector<std::string> Bodies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bodies_;
  }

private:
  void Serve() {
    while (!stop_.load()) {
      const int client = ::accept(socket_, nullptr, nullptr);
      if (client < 0) {
        if (stop_.load())
          return;
        continue;
      }
      ServeOne(client);
      ::close(client);
    }
  }

  static bool ReadRequest(int client, std::string* body) {
    std::string request;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
      const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        return false;
      request.append(buffer, static_cast<std::size_t>(n));
      if (request.size() > 1024 * 1024)
        return false;
    }
    std::size_t content_length = 0;
    const std::size_t length_pos = request.find("Content-Length:");
    if (length_pos != std::string::npos && length_pos < header_end) {
      content_length = static_cast<std::size_t>(
          std::strtoul(request.c_str() + length_pos + 15, nullptr, 10));
    }
    const std::size_t body_start = header_end + 4;
    while (request.size() < body_start + content_length) {
      const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        return false;
      request.append(buffer, static_cast<std::size_t>(n));
    }
    *body = request.substr(body_start, content_length);
    return true;
  }

  std::string Reply(const std::string& body) {
    if (body.find("GetCapabilities") != std::string::npos) {
      const std::string media = "http://127.0.0.1:" + std::to_string(port_) + "/onvif/media_service";
      const std::string ptz = "http://127.0.0.1:" + std::to_string(port_) + "/onvif/ptz_service";
      return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
             "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
             "<s:Body><tds:GetCapabilitiesResponse><tds:Capabilities>"
             "<tt:Media><tt:XAddr>" +
             media +
             "</tt:XAddr></tt:Media>"
             "<tt:PTZ><tt:XAddr>" +
             ptz +
             "</tt:XAddr></tt:PTZ>"
             "</tds:Capabilities></tds:GetCapabilitiesResponse></s:Body></s:Envelope>";
    }
    if (body.find("GetProfiles") != std::string::npos) {
      return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
             "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
             "<s:Body><trt:GetProfilesResponse>"
             "<trt:Profiles token=\"profile_main\" fixed=\"true\">"
             "<tt:Name>main</tt:Name></trt:Profiles>"
             "</trt:GetProfilesResponse></s:Body></s:Envelope>";
    }
    if (body.find("GetConfigurations") != std::string::npos) {
      return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\">"
             "<s:Body><tptz:GetConfigurationsResponse/></s:Body></s:Envelope>";
    }
    // Every PTZ operation gets a matching empty response.
    return "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>"
           "<tptz:Response xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\"/>"
           "</s:Body></s:Envelope>";
  }

  void ServeOne(int client) {
    std::string body;
    if (!ReadRequest(client, &body))
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bodies_.push_back(body);
    }
    const std::string response = Reply(body);
    const std::string http = "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\n"
                             "Content-Length: " +
                             std::to_string(response.size()) + "\r\nConnection: close\r\n\r\n" +
                             response;
    std::size_t sent = 0;
    while (sent < http.size()) {
      const ssize_t n = ::send(client, http.data() + sent, http.size() - sent, 0);
      if (n <= 0)
        return;
      sent += static_cast<std::size_t>(n);
    }
  }

  int socket_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> bodies_;
};

} // namespace

int main() {
  FakeOnvifCamera camera;
  Expect("fake camera listening", camera.port() != 0);

  uvnvr::OnvifPtzRequest request;
  request.host = "127.0.0.1";
  request.port = camera.port();

  std::string error;
  request.operation = uvnvr::OnvifPtzOperation::kContinuousMove;
  request.pan = 0.5;
  request.tilt = -0.25;
  request.zoom = 0.75;
  Expect("continuous move", uvnvr::OnvifPtzControl(request, &error), error);
  {
    const std::vector<std::string> bodies = camera.Bodies();
    bool found = false;
    for (const std::string& body : bodies) {
      if (body.find("ContinuousMove") == std::string::npos)
        continue;
      found = true;
      ExpectContains("move/profile", body, "<tptz:ProfileToken>profile_main</tptz:ProfileToken>");
      ExpectContains("move/pan", body, "x=\"0.5000\"");
      ExpectContains("move/tilt", body, "y=\"-0.2500\"");
      ExpectContains("move/zoom", body, "x=\"0.7500\"");
    }
    Expect("move/request-seen", found);
  }

  request.operation = uvnvr::OnvifPtzOperation::kStop;
  request.pan = request.tilt = request.zoom = 0.0;
  Expect("stop", uvnvr::OnvifPtzControl(request, &error), error);
  {
    const std::vector<std::string> bodies = camera.Bodies();
    bool found = false;
    for (const std::string& body : bodies) {
      if (body.find("<tptz:Stop>") == std::string::npos)
        continue;
      found = true;
      ExpectContains("stop/pan-tilt", body, "<tptz:PanTilt>true</tptz:PanTilt>");
      ExpectContains("stop/zoom", body, "<tptz:Zoom>true</tptz:Zoom>");
    }
    Expect("stop/request-seen", found);
  }

  request.operation = uvnvr::OnvifPtzOperation::kGotoPreset;
  request.preset = 7;
  Expect("goto preset", uvnvr::OnvifPtzControl(request, &error), error);
  request.operation = uvnvr::OnvifPtzOperation::kSetPreset;
  Expect("set preset", uvnvr::OnvifPtzControl(request, &error), error);
  request.operation = uvnvr::OnvifPtzOperation::kRemovePreset;
  Expect("remove preset", uvnvr::OnvifPtzControl(request, &error), error);
  {
    const std::vector<std::string> bodies = camera.Bodies();
    bool goto_seen = false;
    bool set_seen = false;
    bool remove_seen = false;
    for (const std::string& body : bodies) {
      if (body.find("<tptz:GotoPreset>") != std::string::npos) {
        goto_seen = true;
        ExpectContains("goto/preset", body, "<tptz:PresetToken>7</tptz:PresetToken>");
      }
      if (body.find("<tptz:SetPreset>") != std::string::npos) {
        set_seen = true;
        ExpectContains("set/preset", body, "<tptz:PresetToken>7</tptz:PresetToken>");
      }
      if (body.find("<tptz:RemovePreset>") != std::string::npos) {
        remove_seen = true;
        ExpectContains("remove/preset", body, "<tptz:PresetToken>7</tptz:PresetToken>");
      }
    }
    Expect("goto/request-seen", goto_seen);
    Expect("set/request-seen", set_seen);
    Expect("remove/request-seen", remove_seen);
  }

  // A preset request without a number is rejected before any HTTP call.
  request.preset = 0;
  request.operation = uvnvr::OnvifPtzOperation::kGotoPreset;
  Expect("preset/needs-number", !uvnvr::OnvifPtzControl(request, &error));

  // A closed port must fail rather than hang.
  request.port = camera.port() + 1;
  request.operation = uvnvr::OnvifPtzOperation::kStop;
  Expect("unreachable camera", !uvnvr::OnvifPtzControl(request, &error));

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ONVIF PTZ tests passed\n");
  return 0;
}
