#include "sdk_internal.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct uvnvr_sdk {
  std::unique_ptr<uvnvr::sdk::Client> impl;
};

namespace {

thread_local std::string g_last_error;
thread_local int g_last_code = UVNVR_OK;

void SetLastError(int code, const std::string& message) {
  g_last_code = code;
  g_last_error = message.empty() ? "SDK error" : message;
}

void ClearLastError() {
  g_last_code = UVNVR_OK;
  g_last_error.clear();
}

size_t StringBytes(const std::string& text) {
  return text.size() + 1;
}

char* PutString(char** cursor, const std::string& text) {
  char* target = *cursor;
  if (!text.empty())
    std::memcpy(target, text.data(), text.size());
  target[text.size()] = '\0';
  *cursor += text.size() + 1;
  return target;
}

bool Covered(const uvnvr_options_t* options, size_t offset, size_t length) {
  if (options->struct_size == 0)
    return true; // zero-initialised options use the full v1 layout
  return offset + length <= options->struct_size;
}

uvnvr::sdk::Options ToOptions(const uvnvr_options_t* input) {
  uvnvr::sdk::Options options;
  if (input == nullptr)
    return options;
  if (Covered(input, offsetof(uvnvr_options_t, connect_timeout_ms), sizeof(uint32_t)) &&
      input->connect_timeout_ms != 0)
    options.connect_timeout_ms = input->connect_timeout_ms;
  if (Covered(input, offsetof(uvnvr_options_t, request_timeout_ms), sizeof(uint32_t)) &&
      input->request_timeout_ms != 0)
    options.request_timeout_ms = input->request_timeout_ms;
  if (Covered(input, offsetof(uvnvr_options_t, quic_probe_timeout_ms), sizeof(uint32_t)))
    options.quic_probe_timeout_ms = input->quic_probe_timeout_ms;
  if (Covered(input, offsetof(uvnvr_options_t, prefer_quic), sizeof(int32_t)))
    options.prefer_quic = input->prefer_quic != 0;
  if (Covered(input, offsetof(uvnvr_options_t, verify_peer), sizeof(int32_t)))
    options.verify_peer = input->verify_peer != 0;
  if (Covered(input, offsetof(uvnvr_options_t, ca_file), sizeof(const char*)) &&
      input->ca_file != nullptr)
    options.ca_file = input->ca_file;
  if (Covered(input, offsetof(uvnvr_options_t, heartbeat_interval_ms), sizeof(uint32_t)) &&
      input->heartbeat_interval_ms != 0)
    options.heartbeat_interval_ms = input->heartbeat_interval_ms;
  if (Covered(input, offsetof(uvnvr_options_t, session_renew_ms), sizeof(uint32_t)) &&
      input->session_renew_ms != 0)
    options.session_renew_ms = input->session_renew_ms;
  if (Covered(input, offsetof(uvnvr_options_t, disable_tcp), sizeof(int32_t)))
    options.disable_tcp = input->disable_tcp != 0;
  return options;
}

template <typename T>
T* AllocateArray(size_t count) {
  if (count == 0)
    return nullptr;
  return static_cast<T*>(std::calloc(count, sizeof(T)));
}

} // namespace

extern "C" {

const char* UVNVR_CALL uvnvr_version(void) {
  return "UvNVR SDK " UVNVR_SDK_VERSION " (protocol 1)";
}

void UVNVR_CALL uvnvr_options_init(uvnvr_options_t* options) {
  if (options == nullptr)
    return;
  std::memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(uvnvr_options_t);
  options->connect_timeout_ms = 5000;
  options->request_timeout_ms = 10000;
  options->quic_probe_timeout_ms = 400;
  options->prefer_quic = 1;
  options->verify_peer = 0;
  options->ca_file = nullptr;
  options->heartbeat_interval_ms = 15000;
  options->session_renew_ms = 600000;
  options->disable_tcp = 0;
}

uvnvr_sdk_t* UVNVR_CALL uvnvr_create(const uvnvr_options_t* options) {
  const uvnvr::sdk::Options converted = ToOptions(options);
  if (!converted.prefer_quic && converted.disable_tcp) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT,
                 "prefer_quic is disabled and TCP fallback is disabled");
    return nullptr;
  }
  auto* sdk = new (std::nothrow) uvnvr_sdk();
  if (sdk == nullptr) {
    SetLastError(UVNVR_ERR_NO_MEMORY, "out of memory");
    return nullptr;
  }
  try {
    sdk->impl = std::make_unique<uvnvr::sdk::Client>(converted);
  } catch (const std::exception& error) {
    delete sdk;
    SetLastError(UVNVR_ERR_NO_MEMORY, error.what());
    return nullptr;
  }
  sdk->impl->SetOwner(sdk);
  ClearLastError();
  return sdk;
}

void UVNVR_CALL uvnvr_destroy(uvnvr_sdk_t* sdk) {
  delete sdk;
}

int UVNVR_CALL uvnvr_connect(uvnvr_sdk_t* sdk, const char* host, uint16_t port,
                             int timeout_ms) {
  if (sdk == nullptr || host == nullptr || port == 0) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "host and port are required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  std::string error;
  const int rc = sdk->impl->Connect(host, port, timeout_ms, &error);
  if (rc != UVNVR_OK)
    SetLastError(rc, error);
  else
    ClearLastError();
  return rc;
}

int UVNVR_CALL uvnvr_login(uvnvr_sdk_t* sdk, const char* user, const char* pass) {
  if (sdk == nullptr || user == nullptr || pass == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "user and pass are required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  std::string error;
  const int rc = sdk->impl->Login(user, pass, &error);
  if (rc != UVNVR_OK)
    SetLastError(rc, error);
  else
    ClearLastError();
  return rc;
}

void UVNVR_CALL uvnvr_logout(uvnvr_sdk_t* sdk) {
  if (sdk != nullptr)
    sdk->impl->Logout();
}

int UVNVR_CALL uvnvr_get_channel_list(uvnvr_sdk_t* sdk, uvnvr_channel_t** out,
                                      size_t* count) {
  if (sdk == nullptr || out == nullptr || count == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "out and count are required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  *out = nullptr;
  *count = 0;
  std::vector<uvnvr::sdk::Channel> channels;
  std::string error;
  const int rc = sdk->impl->GetChannels(&channels, &error);
  if (rc != UVNVR_OK) {
    SetLastError(rc, error);
    return rc;
  }

  size_t bytes = 0;
  for (const auto& channel : channels) {
    bytes += StringBytes(channel.name) + StringBytes(channel.protocol) +
             StringBytes(channel.host) + StringBytes(channel.main_state) +
             StringBytes(channel.main_error) + StringBytes(channel.sub_state) +
             StringBytes(channel.sub_error);
  }
  const size_t total = channels.size() * sizeof(uvnvr_channel_t) + bytes;
  auto* array = static_cast<uvnvr_channel_t*>(std::calloc(1, total == 0 ? 1 : total));
  if (array == nullptr) {
    SetLastError(UVNVR_ERR_NO_MEMORY, "out of memory");
    return UVNVR_ERR_NO_MEMORY;
  }
  char* cursor = reinterpret_cast<char*>(array + channels.size());
  for (size_t i = 0; i < channels.size(); ++i) {
    const auto& channel = channels[i];
    uvnvr_channel_t& item = array[i];
    item.struct_size = sizeof(uvnvr_channel_t);
    item.id = channel.id;
    item.group_id = channel.group_id;
    item.port = channel.port;
    item.enabled = channel.enabled ? 1 : 0;
    item.sub_enabled = channel.sub_enabled ? 1 : 0;
    item.online = channel.online ? 1 : 0;
    item.has_keyframe = channel.has_keyframe ? 1 : 0;
    item.name = PutString(&cursor, channel.name);
    item.protocol = PutString(&cursor, channel.protocol);
    item.host = PutString(&cursor, channel.host);
    item.main_state = PutString(&cursor, channel.main_state);
    item.main_error = PutString(&cursor, channel.main_error);
    item.sub_state = PutString(&cursor, channel.sub_state);
    item.sub_error = PutString(&cursor, channel.sub_error);
  }
  *out = array;
  *count = channels.size();
  ClearLastError();
  return UVNVR_OK;
}

void UVNVR_CALL uvnvr_free_channels(uvnvr_channel_t* channels, size_t count) {
  (void)count;
  std::free(channels);
}

int UVNVR_CALL uvnvr_get_recording_days(uvnvr_sdk_t* sdk, int channel_id,
                                        const char* from, const char* to,
                                        char*** days, size_t* count) {
  if (sdk == nullptr || from == nullptr || to == nullptr || days == nullptr ||
      count == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "channelId, from, to, days and count are required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  *days = nullptr;
  *count = 0;
  std::vector<std::string> values;
  std::string error;
  const int rc = sdk->impl->GetDays(channel_id, from, to, &values, &error);
  if (rc != UVNVR_OK) {
    SetLastError(rc, error);
    return rc;
  }
  char** array = static_cast<char**>(std::calloc(values.size() == 0 ? 1 : values.size(),
                                                 sizeof(char*)));
  if (array == nullptr) {
    SetLastError(UVNVR_ERR_NO_MEMORY, "out of memory");
    return UVNVR_ERR_NO_MEMORY;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    array[i] = static_cast<char*>(std::malloc(values[i].size() + 1));
    if (array[i] == nullptr) {
      uvnvr_free_strings(array, i);
      SetLastError(UVNVR_ERR_NO_MEMORY, "out of memory");
      return UVNVR_ERR_NO_MEMORY;
    }
    std::memcpy(array[i], values[i].data(), values[i].size());
    array[i][values[i].size()] = '\0';
  }
  *days = array;
  *count = values.size();
  ClearLastError();
  return UVNVR_OK;
}

void UVNVR_CALL uvnvr_free_strings(char** strings, size_t count) {
  if (strings == nullptr)
    return;
  for (size_t i = 0; i < count; ++i)
    std::free(strings[i]);
  std::free(strings);
}

int UVNVR_CALL uvnvr_get_recordings(uvnvr_sdk_t* sdk, int channel_id,
                                    const char* stream, int64_t from_ms,
                                    int64_t to_ms, uvnvr_recording_t** out,
                                    size_t* count) {
  if (sdk == nullptr || out == nullptr || count == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "out and count are required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  *out = nullptr;
  *count = 0;
  std::vector<uvnvr::sdk::Recording> recordings;
  std::string error;
  const int rc = sdk->impl->GetRecordings(channel_id, stream ? stream : "", from_ms, to_ms,
                                          &recordings, &error);
  if (rc != UVNVR_OK) {
    SetLastError(rc, error);
    return rc;
  }
  size_t bytes = 0;
  for (const auto& recording : recordings) {
    bytes += StringBytes(recording.stream) + StringBytes(recording.codec) +
             StringBytes(recording.record_type) + StringBytes(recording.path);
  }
  const size_t total = recordings.size() * sizeof(uvnvr_recording_t) + bytes;
  auto* array = static_cast<uvnvr_recording_t*>(std::calloc(1, total == 0 ? 1 : total));
  if (array == nullptr) {
    SetLastError(UVNVR_ERR_NO_MEMORY, "out of memory");
    return UVNVR_ERR_NO_MEMORY;
  }
  char* cursor = reinterpret_cast<char*>(array + recordings.size());
  for (size_t i = 0; i < recordings.size(); ++i) {
    const auto& recording = recordings[i];
    uvnvr_recording_t& item = array[i];
    item.struct_size = sizeof(uvnvr_recording_t);
    item.id = recording.id;
    item.channel_id = recording.channel_id;
    item.start_ms = recording.start_ms;
    item.end_ms = recording.end_ms;
    item.duration_ms = recording.duration_ms;
    item.size_bytes = recording.size_bytes;
    item.active = recording.active ? 1 : 0;
    item.stream = PutString(&cursor, recording.stream);
    item.codec = PutString(&cursor, recording.codec);
    item.record_type = PutString(&cursor, recording.record_type);
    item.path = PutString(&cursor, recording.path);
  }
  *out = array;
  *count = recordings.size();
  ClearLastError();
  return UVNVR_OK;
}

void UVNVR_CALL uvnvr_free_recordings(uvnvr_recording_t* recordings, size_t count) {
  (void)count;
  std::free(recordings);
}

int UVNVR_CALL uvnvr_live_open(uvnvr_sdk_t* sdk, int channel_id, int stream,
                               uvnvr_frame_cb cb, void* user) {
  if (sdk == nullptr || cb == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "callback is required");
    return -UVNVR_ERR_INVALID_ARGUMENT;
  }
  std::string error;
  auto internal = [sdk, cb, user](int handle, const uint8_t* data, size_t size, int codec,
                                  uint32_t flags, int64_t pts) {
    uvnvr_frame_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(uvnvr_frame_t);
    frame.codec = codec;
    frame.flags = flags;
    frame.pts_ms = pts;
    frame.data = data;
    frame.size = size;
    cb(sdk, handle, &frame, user);
  };
  const int handle =
      sdk->impl->LiveOpen(channel_id, stream, std::move(internal), &error);
  if (handle < 0) {
    SetLastError(-handle, error);
    return handle;
  }
  ClearLastError();
  return handle;
}

int UVNVR_CALL uvnvr_live_close(uvnvr_sdk_t* sdk, int handle) {
  if (sdk == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "sdk is required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  const int rc = sdk->impl->LiveClose(handle);
  if (rc != UVNVR_OK)
    SetLastError(rc, "invalid live handle");
  else
    ClearLastError();
  return rc;
}

int UVNVR_CALL uvnvr_download_recording(uvnvr_sdk_t* sdk, int64_t recording_id,
                                        const char* path,
                                        uvnvr_progress_cb progress, void* user) {
  if (sdk == nullptr || path == nullptr) {
    SetLastError(UVNVR_ERR_INVALID_ARGUMENT, "path is required");
    return UVNVR_ERR_INVALID_ARGUMENT;
  }
  uvnvr::sdk::ProgressCallback internal;
  if (progress != nullptr) {
    internal = [sdk, progress, user](int64_t received, int64_t total) {
      progress(sdk, received, total, user);
    };
  }
  std::string error;
  const int rc = sdk->impl->Download(recording_id, path, std::move(internal), &error);
  if (rc != UVNVR_OK)
    SetLastError(rc, error);
  else
    ClearLastError();
  return rc;
}

const char* UVNVR_CALL uvnvr_last_error(uvnvr_sdk_t* sdk) {
  (void)sdk;
  return g_last_error.c_str();
}

int UVNVR_CALL uvnvr_last_error_code(uvnvr_sdk_t* sdk) {
  (void)sdk;
  return g_last_code;
}

uvnvr_transport_t UVNVR_CALL uvnvr_transport(uvnvr_sdk_t* sdk) {
  return sdk == nullptr ? UVNVR_TRANSPORT_UNKNOWN : sdk->impl->Transport();
}

const char* UVNVR_CALL uvnvr_transport_name(uvnvr_sdk_t* sdk) {
  switch (uvnvr_transport(sdk)) {
  case UVNVR_TRANSPORT_QUIC:
    return "quic";
  case UVNVR_TRANSPORT_TCP:
    return "tcp";
  default:
    return "unknown";
  }
}

void UVNVR_CALL uvnvr_cancel(uvnvr_sdk_t* sdk) {
  if (sdk != nullptr)
    sdk->impl->Cancel();
}

} // extern "C"
