# GB/T 28181 MANSCDP codec (P6.2b)

This stage adds the MANSCDP XML layer that the SIP user agent from P6.2a
carries: Catalog, DeviceInfo, DeviceStatus and Keepalive, the 20-digit
identifier rules, and the mapping from the local channel/group tables to
platform-visible catalog items. It does not touch the preview, recording,
ONVIF or private-protocol paths and is compiled only with
`-DUVNVR_ENABLE_GB28181=ON`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DCMAKE_PREFIX_PATH=/home/zwy/.local/uvcpp-1.9.1/usr \
  -DUVNVR_ENABLE_GB28181=ON -DUVNVR_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Targets added by this stage:

| Target | Purpose |
| --- | --- |
| `gb28181_manscdp_test` | Codec, identifier and channel-mapping unit tests, plus a fixture emitter |
| `uvnvr_gb28181` | Static library; now contains `sip.cpp` and `manscdp.cpp` |

## Identifier rules

A GB/T 28181 ID is 20 digits:

```
center code (8) + industry code (2) + type code (3) + sequence (7)
```

`ParseGb28181Id` / `IsValidGb28181Id` reject:

- anything that is not exactly 20 ASCII digits;
- an all-zero center code;
- a sequence of `0000000`;
- a type code whose first digit is not 1 (device), 2 (platform), 3 (business
  group) or 4 (virtual organization).

`MakeChannelGb28181Id(center, sequence)` defaults to type `132` (network
camera) and industry `00`; `MakeGroupGb28181Id` defaults to type `310`
(business group). Both return an empty string for invalid fields instead of
silently truncating.

## XML codec

The codec is self-contained: a small tree reader handles the declaration,
comments, CDATA, nested elements, attributes and entity references. No GPL or
LGPL XML library is introduced. Documents are written as UTF-8 with an
explicit declaration so Chinese channel names survive unchanged.

| Function | Document |
| --- | --- |
| `BuildManscdpQuery` / `ParseManscdpQuery` | `<Query><CmdType>Catalog|DeviceInfo|DeviceStatus` |
| `BuildCatalogResponse` / `ParseCatalogResponse` | `<Response>` with `<SumNum>` and `<DeviceList Num>` |
| `BuildDeviceInfoResponse` / `ParseDeviceInfoResponse` | `<Response><CmdType>DeviceInfo` |
| `BuildDeviceStatusResponse` / `ParseDeviceStatusResponse` | `<Response><CmdType>DeviceStatus` |
| `BuildKeepalive` / `ParseKeepalive` | `<Notify><CmdType>Keepalive` |

Text is escaped on write (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`) and
entity references are decoded on read, so a channel named `东门 & 通道 <2>` is
round-tripped without breaking the document.

## Channel mapping

`MapChannelsToCatalog` turns the `channels` and `channel_groups` tables into
catalog items:

- a group becomes an item with `Parental=1`;
- a group's `ParentID` is its parent group, or the NVR's own device ID for a
  root group;
- a channel's `ParentID` is its group's device ID, or the NVR device ID when it
  is not grouped;
- a disabled channel is skipped by default, or emitted with `Status=OFF` when
  `include_disabled_channels` is set;
- duplicate IDs, missing group parents, parent cycles and a group using a
  device type code are rejected with an error instead of producing an
  ambiguous catalog.

Civil code defaults to the first six digits of the NVR device ID.

## MESSAGE dispatch

`ManscdpResponder` takes an inbound MANSCDP body and produces the response
body. Its group/channel/status sources are callbacks, so the answer reflects
the current database state rather than a startup snapshot.

`SipUserAgent` now exposes the dispatch point:

- `SipUaCallbacks::on_message` is called after an inbound MESSAGE has been
  acknowledged with `200 OK`; it receives the parsed `SipMessage` (including
  the XML body).
- `SipUserAgent::SendMessage(body, content_type)` sends the MANSCDP response
  back to the platform in a new MESSAGE, reusing the current Digest challenge.

The callback runs on the event-loop thread. The application wires the two
together:

```cpp
uvnvr::gb28181::ManscdpResponder responder(config);
responder.set_group_source([&] { return LoadGroups(); });
responder.set_channel_source([&] { return LoadChannels(); });
responder.set_status_source([&] { return LoadStatus(); });

uvnvr::gb28181::SipUaCallbacks callbacks;
callbacks.on_message = [&](const uvnvr::gb28181::SipMessage& message) {
  std::string response;
  std::string error;
  if (responder.Handle(message.body, &response, &error) && !response.empty())
    ua.SendMessage(std::move(response), std::string(uvnvr::gb28181::kManscdpContentType), &error);
};
ua.set_callbacks(std::move(callbacks));
```

Limitations of this stage:

- `SendMessage` is fire-and-forget: the response MESSAGE is authenticated but
  its final 200/401 is not tracked or retransmitted. Registration and Keepalive
  keep using the tracked transaction path.
- Catalog filtering is based on the items returned by `MapChannelsToCatalog`;
  a query for an unknown device ID returns an empty catalog.
- INVITE/BYE media setup, RecordInfo, PTZ and Alarm are later stages.

## Manual evidence

```sh
build/tests/gb28181_manscdp_test --emit-catalog /tmp/catalog.xml
xmllint --noout /tmp/catalog.xml
grep -c '<Item>' /tmp/catalog.xml
```

With the built-in three-channel fixture this prints `3`, and `xmllint` exits
successfully.
