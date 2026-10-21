#include "openapi.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace uvnvr {
namespace {

using json = nlohmann::json;

struct Endpoint {
  const char* method;
  const char* path;
  const char* summary;
  const char* tag;
  const char* permission;
  const char* content_type;
};

const Endpoint kEndpoints[] = {
    {"get", "/healthz", "Service health and version", "System", "", "application/json"},
    {"get", "/api/v1/setup/status", "First-run setup state", "Setup", "", "application/json"},
    {"post", "/api/v1/setup", "Create the administrator password", "Setup", "", "application/json"},
    {"post", "/api/v1/auth/login", "Start a session", "Auth", "", "application/json"},
    {"get", "/api/v1/auth/me", "Current user and permissions", "Auth", "session",
     "application/json"},
    {"post", "/api/v1/auth/logout", "End the current session", "Auth", "session",
     "application/json"},

    {"get", "/api/v1/dashboard/summary", "Dashboard counters and metrics", "Dashboard",
     "dashboard.view", "application/json"},
    {"get", "/api/v1/system/info", "Minimal public system information", "System", "",
     "application/json"},
    {"get", "/api/v1/system/overview", "Full system overview", "System", "system.view",
     "application/json"},
    {"get", "/api/v1/system/config", "Effective startup configuration", "System", "system.view",
     "application/json"},
    {"get", "/api/v1/settings", "Read runtime settings", "System", "system.view",
     "application/json"},
    {"put", "/api/v1/settings", "Update runtime settings", "System", "system.manage",
     "application/json"},
    {"get", "/api/v1/system/time", "Read the recorder clock", "System", "system.view",
     "application/json"},
    {"post", "/api/v1/system/time/sync", "Apply browser time", "System", "system.manage",
     "application/json"},
    {"post", "/api/v1/system/ntp/sync", "Synchronize with NTP", "System", "system.manage",
     "application/json"},
    {"post", "/api/v1/system/email/test", "Send a test email", "System", "system.manage",
     "application/json"},
    {"put", "/api/v1/system/serial", "Change the serial number (maintenance)", "System",
     "maintenance + admin", "application/json"},
    {"put", "/api/v1/system/version", "Change the display version (maintenance)", "System",
     "maintenance + admin", "application/json"},

    {"get", "/api/v1/channels", "List channels", "Channels", "channel.view", "application/json"},
    {"get", "/api/v1/preview/channels", "Preview-only channel metadata", "Channels", "preview.view",
     "application/json"},
    {"post", "/api/v1/channels", "Create a channel", "Channels", "channel.create",
     "application/json"},
    {"put", "/api/v1/channels/{id}", "Update a channel", "Channels", "channel.manage",
     "application/json"},
    {"delete", "/api/v1/channels/{id}", "Delete a channel", "Channels", "channel.delete",
     "application/json"},
    {"post", "/api/v1/channels/detect", "Probe a camera", "Channels", "channel.manage",
     "application/json"},
    {"post", "/api/v1/channels/discover", "Scan the LAN with WS-Discovery", "Channels",
     "channel.manage", "application/json"},
    {"post", "/api/v1/channels/onvif/probe", "Probe an ONVIF device", "Channels", "channel.manage",
     "application/json"},
    {"post", "/api/v1/channels/{id}/reconnect", "Reconnect a channel", "Channels", "channel.manage",
     "application/json"},
    {"post", "/api/v1/channels/sync", "Copy channel settings to other channels", "Channels",
     "channel.manage", "application/json"},
    {"get", "/api/v1/channels/{id}/snapshot", "Capture a JPEG snapshot", "Channels", "api.snapshot",
     "image/jpeg"},
    {"get", "/api/v1/channel-groups", "List channel groups", "Channels", "session",
     "application/json"},
    {"post", "/api/v1/channel-groups", "Create a channel group", "Channels", "channel.manage",
     "application/json"},
    {"put", "/api/v1/channel-groups/{id}", "Update a channel group", "Channels", "channel.manage",
     "application/json"},
    {"delete", "/api/v1/channel-groups/{id}", "Delete a channel group", "Channels",
     "channel.manage", "application/json"},
    {"get", "/api/v1/layouts", "List the current user's layouts", "Channels", "preview.view",
     "application/json"},
    {"post", "/api/v1/layouts", "Create a preview layout", "Channels", "preview.view",
     "application/json"},
    {"put", "/api/v1/layouts/{id}", "Update a preview layout", "Channels", "preview.view",
     "application/json"},
    {"delete", "/api/v1/layouts/{id}", "Delete a preview layout", "Channels", "preview.view",
     "application/json"},

    {"get", "/api/v1/plans", "List recording plans", "Plans", "plan.view", "application/json"},
    {"put", "/api/v1/plans", "Replace a channel's plan", "Plans", "plan.manage",
     "application/json"},
    {"post", "/api/v1/plans/sync", "Copy plans to other channels", "Plans", "plan.manage",
     "application/json"},

    {"get", "/api/v1/storage/browse", "Browse storage directories", "Storage", "storage.manage",
     "application/json"},
    {"get", "/api/v1/storage/volumes", "List storage volumes", "Storage", "storage.view",
     "application/json"},
    {"post", "/api/v1/storage/volumes", "Add a storage volume", "Storage", "storage.manage",
     "application/json"},
    {"put", "/api/v1/storage/volumes/{id}", "Update a storage volume", "Storage", "storage.manage",
     "application/json"},
    {"delete", "/api/v1/storage/volumes/{id}", "Delete a storage volume", "Storage",
     "storage.manage", "application/json"},
    {"post", "/api/v1/storage/format", "Format a volume's recordings (maintenance)", "Storage",
     "maintenance + admin", "application/json"},
    {"get", "/api/v1/recordings/status", "Live recorder status", "Storage", "storage.view",
     "application/json"},
    {"get", "/api/v1/recordings", "Query recording segments", "Playback", "playback.view",
     "application/json"},
    {"delete", "/api/v1/recordings/{id}", "Delete a segment (maintenance)", "Playback",
     "maintenance + admin", "application/json"},
    {"get", "/api/v1/recordings/{id}/download", "Download a segment with Range support", "Playback",
     "playback.download", "video/mp2t"},

    {"post", "/api/v1/streams/ticket", "Create a live stream ticket", "Streaming", "preview.view",
     "application/json"},
    {"post", "/api/v1/playback/ticket", "Create a playback ticket", "Streaming", "playback.view",
     "application/json"},
    {"post", "/api/v1/playback/export", "Export a time range as MP4", "Streaming",
     "playback.download", "video/mp4"},

    {"post", "/api/v1/events/ticket", "Create a real-time events WebSocket ticket", "Alerts",
     "alert.view", "application/json"},
    {"get", "/api/v1/alerts", "List alerts", "Alerts", "alert.view", "application/json"},
    {"post", "/api/v1/alerts/{id}/ack", "Acknowledge an alert", "Alerts", "alert.manage",
     "application/json"},
    {"get", "/api/v1/alerts/{id}/snapshot", "Download an alert snapshot", "Alerts", "alert.view",
     "image/jpeg"},

    {"get", "/api/v1/users", "List users", "Users", "user.view", "application/json"},
    {"post", "/api/v1/users", "Create a user", "Users", "user.manage", "application/json"},
    {"put", "/api/v1/users/{id}", "Update a user", "Users", "user.manage", "application/json"},
    {"delete", "/api/v1/users/{id}", "Delete a user", "Users", "user.manage", "application/json"},
    {"get", "/api/v1/roles", "List roles", "Users", "user.view", "application/json"},
    {"post", "/api/v1/roles", "Create a role", "Users", "user.manage", "application/json"},
    {"put", "/api/v1/roles/{id}", "Update a role", "Users", "user.manage", "application/json"},
    {"delete", "/api/v1/roles/{id}", "Delete a role", "Users", "user.manage", "application/json"},
    {"get", "/api/v1/permissions", "List assignable permissions", "Users", "user.view",
     "application/json"},
    {"get", "/api/v1/tokens", "List API tokens", "Tokens", "api.access", "application/json"},
    {"post", "/api/v1/tokens", "Create an API token", "Tokens", "api.access", "application/json"},
    {"delete", "/api/v1/tokens/{id}", "Revoke an API token", "Tokens", "api.access",
     "application/json"},

    {"get", "/api/v1/audit", "Query the audit log", "Audit", "log.view", "application/json"},
    {"get", "/api/v1/audit/export", "Export the filtered audit log as CSV", "Audit", "log.view",
     "text/csv"},
    {"post", "/api/v1/audit/purge", "Purge audit entries (maintenance)", "Audit",
     "maintenance + admin", "application/json"},
};

bool IsPublic(const Endpoint& endpoint) {
  const std::string permission = endpoint.permission;
  return permission.empty();
}

std::string OperationId(const Endpoint& endpoint) {
  std::string id = endpoint.method;
  for (const char* p = endpoint.path; *p != '\0'; ++p) {
    if (*p == '/') {
      id += '_';
    } else if (*p == '{') {
      id += "by_";
      while (*p != '\0' && *p != '}')
        ++p;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
      id += *p;
    }
  }
  return id;
}

json ErrorResponse(const char* description) {
  return json{
      {"description", description},
      {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Error"}}}}}}}};
}

} // namespace

std::string BuildOpenApi(const std::string& version) {
  json paths = json::object();
  for (const auto& endpoint : kEndpoints) {
    json parameters = json::array();
    const std::string path = endpoint.path;
    if (path.find("{id}") != std::string::npos) {
      parameters.push_back(json{{"name", "id"},
                                {"in", "path"},
                                {"required", true},
                                {"schema", {{"type", "integer"}, {"minimum", 1}}}});
    }
    const std::string method = endpoint.method;
    if (!IsPublic(endpoint) && method != "get") {
      parameters.push_back(
          json{{"name", "X-CSRF-Token"},
               {"in", "header"},
               {"required", false},
               {"description",
                "Required for cookie-authenticated mutating requests. Obtain the token from "
                "GET /api/v1/auth/me."}});
    }
    json content;
    if (std::string(endpoint.content_type) == "application/json") {
      content = {{"application/json", {{"schema", {{"type", "object"}}}}}};
    } else {
      content = {{endpoint.content_type, {{"schema", {{"type", "string"}, {"format", "binary"}}}}}};
    }
    json operation = {
        {"summary", endpoint.summary},
        {"tags", json::array({endpoint.tag})},
        {"operationId", OperationId(endpoint)},
        {"parameters", parameters},
        {"responses",
         {{"200", {{"description", "Success"}, {"content", content}}},
          {"400", {{"$ref", "#/components/responses/BadRequest"}}},
          {"401", {{"$ref", "#/components/responses/Unauthorized"}}},
          {"403", {{"$ref", "#/components/responses/Forbidden"}}},
          {"404", {{"$ref", "#/components/responses/NotFound"}}},
          {"409", {{"$ref", "#/components/responses/Conflict"}}},
          {"500", {{"$ref", "#/components/responses/ServerError"}}}}},
    };
    if (IsPublic(endpoint))
      operation["security"] = json::array();
    if (std::string(endpoint.permission) != "")
      operation["x-permission"] = endpoint.permission;
    paths[path][method] = std::move(operation);
  }

  json spec = {
      {"openapi", "3.1.0"},
      {"info",
       {{"title", "UvNVR API"},
        {"version", version},
        {"description",
         "REST API for UvNVR. Cookie sessions require X-CSRF-Token on mutating requests; API "
         "tokens use Authorization: Bearer. WebSocket endpoints are documented in the user "
         "guide: /ws/live and /ws/playback take a single-use ticket."}}},
      {"servers", json::array({json{{"url", "/"}}})},
      {"paths", std::move(paths)},
      {"security",
       json::array({json{{"SessionCookie", json::array()}}, json{{"BearerToken", json::array()}}})},
      {"components",
       {{"securitySchemes",
         {{"SessionCookie", {{"type", "apiKey"}, {"in", "cookie"}, {"name", "uvnvr_session"}}},
          {"BearerToken", {{"type", "http"}, {"scheme", "bearer"}}}}},
        {"schemas",
         {{"Error",
           {{"type", "object"},
            {"properties", {{"error", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
            {"required", json::array({"error", "message"})}}}}},
        {"responses",
         {{"BadRequest", ErrorResponse("Invalid request or options")},
          {"Unauthorized", ErrorResponse("Authentication required or failed")},
          {"Forbidden", ErrorResponse("Missing permission, maintenance mode or administrator")},
          {"NotFound", ErrorResponse("The requested object does not exist")},
          {"Conflict", ErrorResponse("The operation conflicts with the current state")},
          {"ServerError", ErrorResponse("Internal error")}}}}},
  };
  return spec.dump(2);
}

} // namespace uvnvr
