#pragma once

// GB/T 28181 MANSCDP XML codec and catalog mapping (P6.2b).
//
// Scope of this stage:
//   * the 20-digit GB/T 28181 identifier
//     (center code 8 + industry code 2 + type code 3 + sequence 7), with
//     generation and validation helpers;
//   * MANSCDP Catalog query/response, DeviceInfo, DeviceStatus and Keepalive
//     XML encoding and decoding;
//   * mapping the local channels and channel groups to Catalog items,
//     including group hierarchy and ParentID;
//   * a responder that turns an inbound Query/Notify body into a Response
//     body, ready to be wired to the SIP MESSAGE dispatcher.
//
// The module is deliberately self-contained: it does not depend on libuv,
// OpenSSL, nlohmann/json or an external XML library, so it can be tested
// without an event loop. It is compiled only with UVNVR_ENABLE_GB28181=ON.
//
// XML is written as UTF-8 with an explicit `encoding="UTF-8"` declaration.
// The standard text uses GB2312 in some examples; UTF-8 is accepted by the
// common platforms and avoids lossy transcoding of channel names.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gb28181 {

// Content-Type used by GB/T 28181 MESSAGE requests.
inline constexpr std::string_view kManscdpContentType = "Application/MANSCDP+xml";

// ---------------------------------------------------------------------------
// GB/T 28181 identifiers
// ---------------------------------------------------------------------------

// The standard splits the 20-digit identifier into four fields. The sequence
// is kept as a number because catalog mapping generates consecutive values;
// rendering always zero-pads it to seven digits.
struct Gb28181Id {
  std::string center_code;    // 8 digits, administrative division
  std::string industry_code;  // 2 digits, "00" for public security
  std::string type_code;      // 3 digits
  std::uint32_t sequence = 0; // 1..9999999
};

enum class Gb28181IdKind {
  kUnknown,
  kDevice,              // type code starts with 1
  kPlatform,            // type code starts with 2
  kBusinessGroup,       // type code starts with 3
  kVirtualOrganization, // type code starts with 4
};

std::string_view Gb28181IdKindName(Gb28181IdKind kind);

// Parses and validates the four fields. Validation rules:
//   * exactly 20 ASCII digits;
//   * the center code is not all zeroes;
//   * the sequence number is 1..9999999;
//   * the first digit of the type code is 1, 2, 3 or 4.
// `error` (when supplied) receives a human-readable reason on failure.
bool ParseGb28181Id(std::string_view id, Gb28181Id* parsed, std::string* error = nullptr);
bool IsValidGb28181Id(std::string_view id, std::string* error = nullptr);
Gb28181IdKind Gb28181IdKindOf(std::string_view id);

// Renders valid fields; returns an empty string when a field is out of range.
std::string FormatGb28181Id(const Gb28181Id& id);

// Convenience builders. `type_code` defaults to 132 (network camera) for a
// channel and 310 (business group) for a group; `industry_code` defaults to
// "00" (public security).
std::string MakeChannelGb28181Id(std::string_view center_code, std::uint32_t sequence,
                                 std::string_view type_code = "132",
                                 std::string_view industry_code = "00");
std::string MakeGroupGb28181Id(std::string_view center_code, std::uint32_t sequence,
                               std::string_view type_code = "310",
                               std::string_view industry_code = "00");

// ---------------------------------------------------------------------------
// MANSCDP command model
// ---------------------------------------------------------------------------

enum class ManscdpCommand {
  kUnknown,
  kCatalog,
  kDeviceInfo,
  kDeviceStatus,
  kRecordInfo,
  kKeepalive,
  kAlarm,
  kPlayback, // <Control><CmdType>Playback</CmdType> sent in a SIP INFO
};

std::string_view ManscdpCommandName(ManscdpCommand command);
ManscdpCommand ParseManscdpCommand(std::string_view name);

// Root element of a document (MANSCDP uses Query, Response and Notify).
enum class ManscdpDocumentKind { kUnknown, kQuery, kResponse, kNotify };
ManscdpDocumentKind ManscdpDocumentKindOf(std::string_view root_name);

// Minimal command envelope shared by control requests and notifications.
struct ManscdpControl {
  ManscdpCommand command = ManscdpCommand::kUnknown;
  ManscdpDocumentKind document = ManscdpDocumentKind::kUnknown;
  std::uint64_t sn = 0;
  std::string device_id;
};

// Parses a Query/Response/Notify document and fills CmdType, SN and DeviceID.
// This is the entry point used by the MESSAGE dispatcher; it fails for
// malformed XML and for messages without a CmdType.
bool ParseManscdpControl(std::string_view xml, ManscdpControl* control,
                         std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

struct ManscdpQuery {
  std::uint64_t sn = 0;
  std::string device_id;
};

// Builds <Query><CmdType>...</CmdType><SN>...</SN><DeviceID>...</DeviceID>.
// DeviceID is omitted when the string is empty.
std::string BuildManscdpQuery(ManscdpCommand command, const ManscdpQuery& query);

// Parses a <Query> document. `command` (optional) receives the CmdType; the
// parser rejects Keepalive and Alarm, which are not query commands.
bool ParseManscdpQuery(std::string_view xml, ManscdpQuery* query, ManscdpCommand* command = nullptr,
                       std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

struct CatalogItem {
  std::string device_id;
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string owner;
  std::string civil_code;
  std::string address;
  std::string parent_id;
  std::string status = "ON"; // ON / OFF
  int parental = 0;          // 0 leaf, 1 has children
  int safety_way = 0;
  int register_way = 1;
  int secrecy = 0;
  // Optional in the standard; omitted from the XML when empty.
  std::string longitude;
  std::string latitude;
};

struct CatalogResponse {
  std::uint64_t sn = 0;
  std::string device_id;
  std::uint32_t sum_num = 0;
  std::vector<CatalogItem> devices;
};

std::string BuildCatalogResponse(const CatalogResponse& response);
bool ParseCatalogResponse(std::string_view xml, CatalogResponse* response,
                          std::string* error = nullptr);

// ---------------------------------------------------------------------------
// DeviceInfo
// ---------------------------------------------------------------------------

struct DeviceInfo {
  std::string device_id;
  std::string result = "OK";
  std::string device_name;
  std::string device_type = "118"; // 118 = network video recorder
  std::string manufacturer = "UvNVR";
  std::string model = "UvNVR";
  std::string firmware;
  std::uint32_t channel = 0;
};

struct DeviceInfoResponse {
  std::uint64_t sn = 0;
  DeviceInfo info;
};

std::string BuildDeviceInfoResponse(const DeviceInfoResponse& response);
bool ParseDeviceInfoResponse(std::string_view xml, DeviceInfoResponse* response,
                             std::string* error = nullptr);

// ---------------------------------------------------------------------------
// DeviceStatus
// ---------------------------------------------------------------------------

struct DeviceStatus {
  std::string device_id;
  std::string result = "OK";
  std::string online = "ONLINE"; // ONLINE / OFFLINE
  std::string status = "OK";     // OK / ERROR
  std::string encode = "ON";     // ON / OFF
  std::string record = "ON";     // ON / OFF
  std::string device_time;       // local time, "YYYY-MM-DDTHH:MM:SS"
};

struct DeviceStatusResponse {
  std::uint64_t sn = 0;
  DeviceStatus status;
};

std::string BuildDeviceStatusResponse(const DeviceStatusResponse& response);
bool ParseDeviceStatusResponse(std::string_view xml, DeviceStatusResponse* response,
                               std::string* error = nullptr);

// ---------------------------------------------------------------------------
// RecordInfo (录像查询)
// ---------------------------------------------------------------------------

// RecordInfo asks the device for the recording index of one device/channel in
// a time range. `type` is one of "all", "time", "alarm" or "manual" (an empty
// value is treated as "all").
struct RecordInfoQuery {
  std::uint64_t sn = 0;
  std::string device_id;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::string file_path;
  std::string address;
  int secrecy = 0;
  std::string type = "all";
};

// One recording in a RecordInfo response. `type` uses the MANSCDP spelling
// ("time", "alarm", "manual"), not the local database spelling
// ("timed"/"event").
struct RecordItem {
  std::string device_id;
  std::string name;
  std::string file_path;
  std::string address;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  int secrecy = 0;
  std::string type = "time";
  std::uint64_t file_size = 0;
};

struct RecordInfoResponse {
  std::uint64_t sn = 0;
  std::string device_id;
  std::string name;
  std::uint32_t sum_num = 0;
  std::vector<RecordItem> items;
};

std::string BuildRecordInfoQuery(const RecordInfoQuery& query);
bool ParseRecordInfoQuery(std::string_view xml, RecordInfoQuery* query,
                          std::string* error = nullptr);
std::string BuildRecordInfoResponse(const RecordInfoResponse& response);
bool ParseRecordInfoResponse(std::string_view xml, RecordInfoResponse* response,
                             std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Playback control (倍速 / 暂停 / 拖拽)
// ---------------------------------------------------------------------------

// The platform sends this document inside a SIP INFO to change an established
// playback dialog. `scale` follows GB/T 28181: 0 pauses, positive values are
// the forward rate (1.0 is real time). A negative scale would be reverse
// playback; this implementation rejects it instead of silently playing
// forward. An empty `range` leaves the current position alone.
struct PlaybackControl {
  std::uint64_t sn = 0;
  std::string device_id;
  double scale = 1.0;
  bool has_scale = false;
  std::int64_t range_start_ms = 0;
  std::int64_t range_end_ms = 0;
  bool has_range = false;
};

std::string BuildPlaybackControl(const PlaybackControl& control);
bool ParsePlaybackControl(std::string_view xml, PlaybackControl* control,
                          std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Keepalive
// ---------------------------------------------------------------------------

struct KeepaliveMessage {
  std::uint64_t sn = 0;
  std::string device_id;
  std::string status = "OK";
};

// Keepalive is a <Notify> document sent by the device to the platform.
std::string BuildKeepalive(const KeepaliveMessage& message);
bool ParseKeepalive(std::string_view xml, KeepaliveMessage* message, std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Local channel / group mapping
// ---------------------------------------------------------------------------

// A local channel as stored in the service database. `local_id` is the
// integer primary key; `device_id` is the 20-digit GB identifier assigned to
// that channel.
struct CatalogChannel {
  std::int64_t local_id = 0;
  std::string device_id;
  std::string name;
  bool enabled = true;
  bool online = true;
  std::int64_t group_local_id = 0;
  // Optional per-channel overrides; empty values fall back to the map config.
  std::string manufacturer;
  std::string model;
  std::string owner;
  std::string civil_code;
  std::string address;
};

// A local channel group (channel_groups table). Groups are emitted as catalog
// items with Parental=1 so that platforms can display the tree.
struct CatalogGroup {
  std::int64_t local_id = 0;
  std::string device_id;
  std::string name;
  std::int64_t parent_local_id = 0;
};

struct CatalogMapConfig {
  // The NVR's own 20-digit identifier. It is the ParentID of root groups and
  // of channels that are not assigned to a group.
  std::string device_id;
  std::string manufacturer = "UvNVR";
  std::string model = "UvNVR";
  std::string owner;
  // Defaults to the first six digits of `device_id` when empty.
  std::string civil_code;
  std::string address;
  int secrecy = 0;
  bool include_disabled_channels = false;
  // Optional name for the device itself; catalog items use their own name.
  std::string device_name;
};

struct CatalogMapResult {
  bool ok = false;
  std::string error;
  std::vector<CatalogItem> items;
};

// Validates the identifiers, checks group parents for cycles and builds
// catalog items in a stable order: groups first (input order), then channels
// (input order). Disabled channels are skipped unless configured otherwise.
CatalogMapResult MapChannelsToCatalog(const CatalogMapConfig& config,
                                      const std::vector<CatalogGroup>& groups,
                                      const std::vector<CatalogChannel>& channels);

// ---------------------------------------------------------------------------
// MANSCDP responder
// ---------------------------------------------------------------------------

// Static description of the NVR as seen by the platform.
struct ManscdpDeviceConfig {
  std::string device_id;
  std::string device_name;
  std::string device_type = "118";
  std::string manufacturer = "UvNVR";
  std::string model = "UvNVR";
  std::string firmware;
  std::string civil_code;
  std::string owner;
  std::string address;
  int secrecy = 0;
  bool include_disabled_channels = false;
};

// Turns an inbound MANSCDP control body into a response body. The sources are
// callbacks so the responder always sees the current database state; they may
// be omitted, in which case the catalog is empty and the device status is a
// default "ONLINE/OK" snapshot.
class ManscdpResponder {
public:
  using GroupSource = std::function<std::vector<CatalogGroup>()>;
  using ChannelSource = std::function<std::vector<CatalogChannel>()>;
  using StatusSource = std::function<DeviceStatus()>;
  // Receives a parsed RecordInfo query and returns the matching index rows.
  // It is called on the SIP event-loop thread, so it must not block for long;
  // the service implementation performs a bounded SQLite query.
  using RecordSource = std::function<std::vector<RecordItem>(const RecordInfoQuery&)>;

  explicit ManscdpResponder(ManscdpDeviceConfig config);

  void set_group_source(GroupSource source);
  void set_channel_source(ChannelSource source);
  void set_status_source(StatusSource source);
  void set_record_source(RecordSource source);

  // Returns true when `body` is a recognised MANSCDP command. `response_body`
  // is empty for Keepalive notifications, which the device does not answer.
  // Returns false with an `error` for malformed XML or unsupported commands.
  bool Handle(std::string_view body, std::string* response_body,
              std::string* error = nullptr) const;

  const ManscdpDeviceConfig& config() const {
    return config_;
  }

private:
  CatalogMapResult BuildCatalog() const;
  std::vector<CatalogItem> FilterCatalog(const std::vector<CatalogItem>& items,
                                         std::string_view requested_id) const;
  DeviceInfo BuildDeviceInfo() const;
  DeviceStatus BuildDeviceStatus() const;

  bool BuildRecordInfo(const RecordInfoQuery& query, std::string* response_body,
                       std::string* error) const;

  ManscdpDeviceConfig config_;
  GroupSource group_source_;
  ChannelSource channel_source_;
  StatusSource status_source_;
  RecordSource record_source_;
};

// Current local time in the DeviceStatus form. Exposed for tests and for
// callers that want to fill DeviceStatus themselves.
std::string CurrentLocalIso8601();

} // namespace uvnvr::gb28181
