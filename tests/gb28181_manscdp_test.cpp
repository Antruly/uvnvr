// Unit tests for the GB/T 28181 MANSCDP codec (P6.2b).
//
// This binary also emits fixtures for manual verification:
//
//   gb28181_manscdp_test --emit-catalog <out.xml>
//
// The emitted document uses three local channels and is meant to be checked
// with `xmllint --noout` and by counting <Item> elements.

#include "gb28181/manscdp.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace uvnvr::gb28181;

int failures = 0;

void Expect(const std::string& name, const std::string& actual, const std::string& expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got '%s', expected '%s'\n", name.c_str(), actual.c_str(),
               expected.c_str());
  ++failures;
}

void ExpectBool(const std::string& name, bool actual, bool expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %s, expected %s\n", name.c_str(), actual ? "true" : "false",
               expected ? "true" : "false");
  ++failures;
}

void ExpectSize(const std::string& name, std::size_t actual, std::size_t expected) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "FAIL %s: got %zu, expected %zu\n", name.c_str(), actual, expected);
  ++failures;
}

void ExpectContains(const std::string& name, std::string_view haystack, std::string_view needle) {
  if (haystack.find(needle) != std::string_view::npos)
    return;
  std::fprintf(stderr, "FAIL %s: document does not contain '%.*s'\n", name.c_str(),
               static_cast<int>(needle.size()), needle.data());
  ++failures;
}

const char* kDeviceId = "34020000001180000001";
const char* kChannelId1 = "34020000001320000001";
const char* kChannelId2 = "34020000001320000002";
const char* kChannelId3 = "34020000001320000003";
const char* kGroupId1 = "34020000003100000001";
const char* kGroupId2 = "34020000003100000002";

std::vector<CatalogChannel> ThreeChannels() {
  CatalogChannel first;
  first.local_id = 1;
  first.device_id = kChannelId1;
  first.name = "前门摄像机";
  first.group_local_id = 1;
  first.manufacturer = "UvNVR";
  first.model = "IPC-1";

  CatalogChannel second;
  second.local_id = 2;
  second.device_id = kChannelId2;
  second.name = "东门 & 通道 <2>";
  second.group_local_id = 2;

  CatalogChannel third;
  third.local_id = 3;
  third.device_id = kChannelId3;
  third.name = "停车场";
  third.group_local_id = 0;

  return {first, second, third};
}

std::vector<CatalogGroup> TwoGroups() {
  CatalogGroup root;
  root.local_id = 1;
  root.device_id = kGroupId1;
  root.name = "园区";

  CatalogGroup child;
  child.local_id = 2;
  child.device_id = kGroupId2;
  child.name = "东区";
  child.parent_local_id = 1;
  return {root, child};
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

void TestIdentifiers() {
  Gb28181Id parsed;
  std::string error;
  ExpectBool("id/parse-device", ParseGb28181Id(kChannelId1, &parsed, &error), true);
  Expect("id/center", parsed.center_code, "34020000");
  Expect("id/industry", parsed.industry_code, "00");
  Expect("id/type", parsed.type_code, "132");
  Expect("id/sequence", std::to_string(parsed.sequence), "1");
  Expect("id/kind-device", std::string(Gb28181IdKindName(Gb28181IdKindOf(kChannelId1))), "device");

  ExpectBool("id/parse-platform", ParseGb28181Id("34020000002000000001", &parsed, nullptr), true);
  Expect("id/kind-platform",
         std::string(Gb28181IdKindName(Gb28181IdKindOf("34020000002000000001"))), "platform");
  Expect("id/kind-group", std::string(Gb28181IdKindName(Gb28181IdKindOf(kGroupId1))),
         "business-group");
  Expect("id/kind-virtual", std::string(Gb28181IdKindName(Gb28181IdKindOf("34020000004100000001"))),
         "virtual-organization");

  Expect("id/make-channel", MakeChannelGb28181Id("34020000", 1), kChannelId1);
  Expect("id/make-group", MakeGroupGb28181Id("34020000", 1), kGroupId1);
  Expect("id/format-roundtrip", FormatGb28181Id(parsed), "34020000002000000001");

  struct InvalidId {
    const char* id;
    const char* description;
  };
  const InvalidId invalid[] = {
      {"", "empty"},
      {"3402000000132000000", "19 digits"},
      {"340200000013200000011", "21 digits"},
      {"3402000000132000000a", "non-digit"},
      {"00000000001320000001", "zero center code"},
      {"34020000001320000000", "zero sequence"},
      {"34020000005320000001", "type code starts with 5"},
  };
  for (const InvalidId& sample : invalid) {
    std::string reason;
    ExpectBool(std::string("id/invalid/") + sample.description,
               IsValidGb28181Id(sample.id, &reason), false);
    if (reason.empty()) {
      std::fprintf(stderr, "FAIL id/invalid/%s: no error message\n", sample.description);
      ++failures;
    }
  }

  Gb28181Id too_short{"3402", "00", "132", 1};
  Expect("id/format-short-center", FormatGb28181Id(too_short), "");
  Expect("id/make-zero-sequence", MakeChannelGb28181Id("34020000", 0), "");
  Expect("id/make-overflow-sequence", MakeChannelGb28181Id("34020000", 10000000), "");
  Expect("id/make-non-digit-center", MakeChannelGb28181Id("3402000z", 1), "");
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

void TestCatalogCodec() {
  CatalogResponse original;
  original.sn = 42;
  original.device_id = kDeviceId;
  for (const CatalogChannel& channel : ThreeChannels()) {
    CatalogItem item;
    item.device_id = channel.device_id;
    item.name = channel.name;
    item.manufacturer = "UvNVR";
    item.model = "IPC-1";
    item.owner = "园区";
    item.civil_code = "340200";
    item.address = "合肥";
    item.parent_id = channel.group_local_id == 0 ? kDeviceId : kGroupId1;
    item.parental = 0;
    item.status = "ON";
    original.devices.push_back(item);
  }

  const std::string xml = BuildCatalogResponse(original);
  ExpectContains("catalog/declaration", xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
  ExpectContains("catalog/cmd-type", xml, "<CmdType>Catalog</CmdType>");
  ExpectContains("catalog/sn", xml, "<SN>42</SN>");
  ExpectContains("catalog/sum", xml, "<SumNum>3</SumNum>");
  ExpectContains("catalog/num-attribute", xml, "<DeviceList Num=\"3\">");
  ExpectContains("catalog/escape-amp", xml, "东门 &amp; 通道 &lt;2&gt;");
  ExpectContains("catalog/utf8-name", xml, "前门摄像机");

  CatalogResponse parsed;
  std::string error;
  ExpectBool("catalog/parse", ParseCatalogResponse(xml, &parsed, &error), true);
  if (!error.empty())
    std::fprintf(stderr, "catalog/parse error: %s\n", error.c_str());
  Expect("catalog/roundtrip-sn", std::to_string(parsed.sn), "42");
  Expect("catalog/roundtrip-device", parsed.device_id, kDeviceId);
  ExpectSize("catalog/roundtrip-count", parsed.devices.size(), 3);
  Expect("catalog/roundtrip-sum", std::to_string(parsed.sum_num), "3");
  if (parsed.devices.size() == 3) {
    Expect("catalog/roundtrip-name", parsed.devices[0].name, "前门摄像机");
    Expect("catalog/roundtrip-escaped-name", parsed.devices[1].name, "东门 & 通道 <2>");
    Expect("catalog/roundtrip-parent", parsed.devices[2].parent_id, kDeviceId);
    Expect("catalog/roundtrip-civil", parsed.devices[0].civil_code, "340200");
  }

  CatalogResponse malformed_items;
  malformed_items.sn = 1;
  malformed_items.device_id = kDeviceId;
  malformed_items.devices.push_back(CatalogItem{}); // no DeviceID
  ExpectBool("catalog/parse-item-without-id",
             ParseCatalogResponse(BuildCatalogResponse(malformed_items), &parsed, &error), false);
}

void TestQueriesAndNotifications() {
  ManscdpQuery query;
  query.sn = 7;
  query.device_id = kDeviceId;
  const std::string catalog_query = BuildManscdpQuery(ManscdpCommand::kCatalog, query);
  ExpectContains("query/catalog", catalog_query, "<CmdType>Catalog</CmdType>");
  ExpectContains("query/catalog-sn", catalog_query, "<SN>7</SN>");
  ExpectContains("query/catalog-device", catalog_query, std::string("<DeviceID>") + kDeviceId);

  ManscdpQuery parsed_query;
  ManscdpCommand parsed_command = ManscdpCommand::kUnknown;
  std::string error;
  ExpectBool("query/parse",
             ParseManscdpQuery(catalog_query, &parsed_query, &parsed_command, &error), true);
  Expect("query/parse-command", std::string(ManscdpCommandName(parsed_command)), "Catalog");
  Expect("query/parse-sn", std::to_string(parsed_query.sn), "7");
  Expect("query/parse-device", parsed_query.device_id, kDeviceId);

  ExpectBool("query/rejects-keepalive",
             ParseManscdpQuery(BuildKeepalive(KeepaliveMessage{1, kDeviceId, "OK"}), &parsed_query,
                               &parsed_command, &error),
             false);

  ManscdpControl control;
  ExpectBool(
      "control/parse-keepalive",
      ParseManscdpControl(BuildKeepalive(KeepaliveMessage{9, kDeviceId, "OK"}), &control, &error),
      true);
  Expect("control/document", std::to_string(static_cast<int>(control.document)),
         std::to_string(static_cast<int>(ManscdpDocumentKind::kNotify)));
  Expect("control/command", std::string(ManscdpCommandName(control.command)), "Keepalive");
  Expect("control/sn", std::to_string(control.sn), "9");
}

void TestDeviceInfoAndStatus() {
  DeviceInfoResponse info_response;
  info_response.sn = 11;
  info_response.info.device_id = kDeviceId;
  info_response.info.device_name = "测试 NVR";
  info_response.info.device_type = "118";
  info_response.info.manufacturer = "UvNVR";
  info_response.info.model = "UvNVR-X1";
  info_response.info.firmware = "0.19.0";
  info_response.info.channel = 3;

  const std::string info_xml = BuildDeviceInfoResponse(info_response);
  ExpectContains("device-info/cmd", info_xml, "<CmdType>DeviceInfo</CmdType>");
  ExpectContains("device-info/channel", info_xml, "<Channel>3</Channel>");
  DeviceInfoResponse parsed_info;
  std::string error;
  ExpectBool("device-info/parse", ParseDeviceInfoResponse(info_xml, &parsed_info, &error), true);
  Expect("device-info/sn", std::to_string(parsed_info.sn), "11");
  Expect("device-info/id", parsed_info.info.device_id, kDeviceId);
  Expect("device-info/name", parsed_info.info.device_name, "测试 NVR");
  Expect("device-info/model", parsed_info.info.model, "UvNVR-X1");
  Expect("device-info/firmware", parsed_info.info.firmware, "0.19.0");
  Expect("device-info/channel-count", std::to_string(parsed_info.info.channel), "3");

  DeviceStatusResponse status_response;
  status_response.sn = 12;
  status_response.status.device_id = kDeviceId;
  status_response.status.online = "ONLINE";
  status_response.status.status = "OK";
  status_response.status.encode = "ON";
  status_response.status.record = "ON";
  status_response.status.device_time = "2026-11-25T23:00:00";

  const std::string status_xml = BuildDeviceStatusResponse(status_response);
  ExpectContains("device-status/cmd", status_xml, "<CmdType>DeviceStatus</CmdType>");
  DeviceStatusResponse parsed_status;
  ExpectBool("device-status/parse", ParseDeviceStatusResponse(status_xml, &parsed_status, &error),
             true);
  Expect("device-status/sn", std::to_string(parsed_status.sn), "12");
  Expect("device-status/online", parsed_status.status.online, "ONLINE");
  Expect("device-status/record", parsed_status.status.record, "ON");
  Expect("device-status/time", parsed_status.status.device_time, "2026-11-25T23:00:00");

  KeepaliveMessage keepalive{5, kDeviceId, "OK"};
  const std::string keepalive_xml = BuildKeepalive(keepalive);
  ExpectContains("keepalive/notify", keepalive_xml, "<Notify>");
  ExpectContains("keepalive/cmd", keepalive_xml, "<CmdType>Keepalive</CmdType>");
  KeepaliveMessage parsed_keepalive;
  ExpectBool("keepalive/parse", ParseKeepalive(keepalive_xml, &parsed_keepalive, &error), true);
  Expect("keepalive/sn", std::to_string(parsed_keepalive.sn), "5");
  Expect("keepalive/device", parsed_keepalive.device_id, kDeviceId);
  Expect("keepalive/status", parsed_keepalive.status, "OK");

  Expect("device-status/now-format", std::to_string(CurrentLocalIso8601().size()), "19");
}

// ---------------------------------------------------------------------------
// Channel mapping
// ---------------------------------------------------------------------------

void TestCatalogMapping() {
  CatalogMapConfig config;
  config.device_id = kDeviceId;
  config.manufacturer = "UvNVR";
  config.model = "UvNVR-X1";
  config.owner = "园区";
  config.address = "合肥";

  const CatalogMapResult mapped = MapChannelsToCatalog(config, TwoGroups(), ThreeChannels());
  ExpectBool("mapping/ok", mapped.ok, true);
  Expect("mapping/error", mapped.error, "");
  ExpectSize("mapping/item-count", mapped.items.size(), 5);

  if (mapped.ok && mapped.items.size() == 5) {
    Expect("mapping/group-id", mapped.items[0].device_id, kGroupId1);
    Expect("mapping/group-parental", std::to_string(mapped.items[0].parental), "1");
    Expect("mapping/group-parent", mapped.items[0].parent_id, kDeviceId);
    Expect("mapping/child-group-parent", mapped.items[1].parent_id, kGroupId1);
    Expect("mapping/channel-parent", mapped.items[2].parent_id, kGroupId1);
    Expect("mapping/channel-child-parent", mapped.items[3].parent_id, kGroupId2);
    Expect("mapping/channel-root-parent", mapped.items[4].parent_id, kDeviceId);
    Expect("mapping/channel-status", mapped.items[4].status, "ON");
    Expect("mapping/civil-code", mapped.items[2].civil_code, "340200");
  }

  // Disabled channels are skipped by default and carried as OFF when asked.
  std::vector<CatalogChannel> channels = ThreeChannels();
  channels[2].enabled = false;
  channels[2].online = false;
  const CatalogMapResult hidden = MapChannelsToCatalog(config, TwoGroups(), channels);
  ExpectBool("mapping/hidden-ok", hidden.ok, true);
  ExpectSize("mapping/hidden-count", hidden.items.size(), 4);

  config.include_disabled_channels = true;
  const CatalogMapResult visible = MapChannelsToCatalog(config, TwoGroups(), channels);
  ExpectSize("mapping/visible-count", visible.items.size(), 5);
  if (visible.items.size() == 5)
    Expect("mapping/visible-status", visible.items[4].status, "OFF");

  // Duplicate identifiers must be rejected instead of producing an ambiguous
  // catalog that a platform cannot merge.
  std::vector<CatalogChannel> duplicate = ThreeChannels();
  duplicate[1].device_id = duplicate[0].device_id;
  const CatalogMapResult duplicate_result = MapChannelsToCatalog(config, TwoGroups(), duplicate);
  ExpectBool("mapping/duplicate-fails", duplicate_result.ok, false);
  ExpectBool("mapping/duplicate-error", !duplicate_result.error.empty(), true);

  std::vector<CatalogChannel> missing_group = ThreeChannels();
  missing_group[0].group_local_id = 99;
  ExpectBool("mapping/missing-group-fails",
             MapChannelsToCatalog(config, TwoGroups(), missing_group).ok, false);

  std::vector<CatalogGroup> cycle = TwoGroups();
  cycle[0].parent_local_id = 2; // 1 -> 2 -> 1
  ExpectBool("mapping/cycle-fails", MapChannelsToCatalog(config, cycle, ThreeChannels()).ok, false);

  CatalogGroup wrong_type = TwoGroups()[0];
  wrong_type.device_id = kChannelId1; // a device ID is not a group
  ExpectBool("mapping/wrong-group-type-fails",
             MapChannelsToCatalog(config, {wrong_type}, ThreeChannels()).ok, false);
}

// ---------------------------------------------------------------------------
// MESSAGE responder
// ---------------------------------------------------------------------------

void TestResponder() {
  ManscdpDeviceConfig config;
  config.device_id = kDeviceId;
  config.device_name = "测试 NVR";
  config.firmware = "0.19.0";
  config.civil_code = "340200";
  config.owner = "园区";

  ManscdpResponder responder(config);
  responder.set_group_source([] { return TwoGroups(); });
  responder.set_channel_source([] { return ThreeChannels(); });
  responder.set_status_source([] {
    DeviceStatus status;
    status.online = "ONLINE";
    status.status = "OK";
    status.device_time = "2026-11-25T23:00:00";
    return status;
  });

  std::string response;
  std::string error;

  const std::string catalog_query =
      BuildManscdpQuery(ManscdpCommand::kCatalog, ManscdpQuery{3, kDeviceId});
  ExpectBool("responder/catalog", responder.Handle(catalog_query, &response, &error), true);
  CatalogResponse catalog;
  ExpectBool("responder/catalog-parse", ParseCatalogResponse(response, &catalog, &error), true);
  Expect("responder/catalog-sn", std::to_string(catalog.sn), "3");
  ExpectSize("responder/catalog-count", catalog.devices.size(), 5);
  Expect("responder/catalog-sum", std::to_string(catalog.sum_num), "5");

  // A catalog query for one group returns that group and its descendants:
  // group 2 plus channels 1 and 2 (channel 3 sits at the device root).
  const std::string group_query =
      BuildManscdpQuery(ManscdpCommand::kCatalog, ManscdpQuery{4, kGroupId1});
  ExpectBool("responder/group-catalog", responder.Handle(group_query, &response, &error), true);
  CatalogResponse group_catalog;
  ExpectBool("responder/group-parse", ParseCatalogResponse(response, &group_catalog, &error), true);
  ExpectSize("responder/group-count", group_catalog.devices.size(), 4);

  const std::string child_group_query =
      BuildManscdpQuery(ManscdpCommand::kCatalog, ManscdpQuery{5, kGroupId2});
  ExpectBool("responder/child-group-catalog",
             responder.Handle(child_group_query, &response, &error), true);
  CatalogResponse child_group;
  ExpectBool("responder/child-group-parse", ParseCatalogResponse(response, &child_group, &error),
             true);
  ExpectSize("responder/child-group-count", child_group.devices.size(), 2);
  if (child_group.devices.size() == 2) {
    Expect("responder/child-group-first", child_group.devices[0].device_id, kGroupId2);
    Expect("responder/child-group-second", child_group.devices[1].device_id, kChannelId2);
  }

  const std::string unknown_group_query =
      BuildManscdpQuery(ManscdpCommand::kCatalog, ManscdpQuery{6, "34020000003100000009"});
  ExpectBool("responder/unknown-group", responder.Handle(unknown_group_query, &response, &error),
             true);
  CatalogResponse unknown_group;
  ExpectBool("responder/unknown-group-parse",
             ParseCatalogResponse(response, &unknown_group, &error), true);
  ExpectSize("responder/unknown-group-count", unknown_group.devices.size(), 0);

  const std::string info_query =
      BuildManscdpQuery(ManscdpCommand::kDeviceInfo, ManscdpQuery{7, kDeviceId});
  ExpectBool("responder/device-info", responder.Handle(info_query, &response, &error), true);
  DeviceInfoResponse info;
  ExpectBool("responder/device-info-parse", ParseDeviceInfoResponse(response, &info, &error), true);
  Expect("responder/device-info-name", info.info.device_name, "测试 NVR");
  Expect("responder/device-info-channel", std::to_string(info.info.channel), "3");
  Expect("responder/device-info-result", info.info.result, "OK");

  const std::string status_query =
      BuildManscdpQuery(ManscdpCommand::kDeviceStatus, ManscdpQuery{8, kDeviceId});
  ExpectBool("responder/device-status", responder.Handle(status_query, &response, &error), true);
  DeviceStatusResponse status;
  ExpectBool("responder/device-status-parse", ParseDeviceStatusResponse(response, &status, &error),
             true);
  Expect("responder/device-status-online", status.status.online, "ONLINE");
  Expect("responder/device-status-time", status.status.device_time, "2026-11-25T23:00:00");

  response = "unchanged";
  ExpectBool(
      "responder/keepalive",
      responder.Handle(BuildKeepalive(KeepaliveMessage{9, kDeviceId, "OK"}), &response, &error),
      true);
  Expect("responder/keepalive-no-response", response, "");

  ExpectBool("responder/unsupported",
             responder.Handle("<Query><CmdType>ConfigDownload</CmdType><SN>1</SN></Query>",
                              &response, &error),
             false);
  ExpectBool("responder/unsupported-error", !error.empty(), true);
  ExpectBool("responder/malformed", responder.Handle("<Query><CmdType>Catalog", &response, &error),
             false);
}

bool WriteFile(const std::string& path, const std::string& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  return output.good();
}

int EmitCatalog(const std::string& path) {
  CatalogResponse response;
  response.sn = 1;
  response.device_id = kDeviceId;
  const std::vector<CatalogChannel> channels = ThreeChannels();
  for (const CatalogChannel& channel : channels) {
    CatalogItem item;
    item.device_id = channel.device_id;
    item.name = channel.name;
    item.manufacturer = "UvNVR";
    item.model = "IPC-1";
    item.owner = "园区";
    item.civil_code = "340200";
    item.address = "合肥";
    item.parent_id = kDeviceId;
    item.parental = 0;
    item.status = channel.online ? "ON" : "OFF";
    response.devices.push_back(item);
  }
  const std::string xml = BuildCatalogResponse(response);
  if (!WriteFile(path, xml)) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return 1;
  }
  std::printf("wrote %s (%zu channels, %zu bytes)\n", path.c_str(), response.devices.size(),
              xml.size());
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--emit-catalog")
    return EmitCatalog(argv[2]);

  TestIdentifiers();
  TestCatalogCodec();
  TestQueriesAndNotifications();
  TestDeviceInfoAndStatus();
  TestCatalogMapping();
  TestResponder();

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("gb28181 MANSCDP tests passed\n");
  return 0;
}
