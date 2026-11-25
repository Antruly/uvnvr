# Channel object authorization (GA/T 2155.2 ACC-02)

`src/secure/channel_acl.{h,cpp}` implements object-level authorization for
channels. It closes the horizontal-privilege gap where any account with
`channel.view` / `preview.view` / `playback.download` could reach every
channel, recording and snapshot by id.

The feature is isolated behind a build option and is **off by default**:

```sh
cmake -S . -B build -DUVNVR_ENABLE_CHANNEL_ACL=ON
```

When the option is off, no ACL table, route or check is compiled; the server
keeps the previous role-only behavior. When it is on, a non-administrator
account is denied by default until an administrator grants channels or groups.

## Model

Two tables are created by `App::Migrate` only when the option is on:

| Table | Columns | Meaning |
| --- | --- | --- |
| `acl_user_channels` | `user_id, channel_id, created_at` | direct channel grant |
| `acl_user_groups` | `user_id, group_id, created_at` | group grant |

A group grant covers every descendant group in `channel_groups` (the hierarchy
is expanded with `acl::ExpandGroupGrants`) and therefore every channel assigned
to those groups through `channel_group_members`. The built-in `admin` role and
any role carrying the `*` permission are unrestricted and need no grants.

Decision rules in `acl::Grants::Allows`:

1. unrestricted (administrator) -> allow;
2. direct `channel_id` grant -> allow;
3. channel's group is granted (including an ancestor grant) -> allow;
4. otherwise -> deny.

The service fails closed: a database error while loading grants is a `500`,
never an implicit allow.

## Management API

Both endpoints require `user.manage` and write a `permission` audit entry.

```sh
# Read the configured grants (direct values, before group expansion).
curl -b admin.cookie http://127.0.0.1:8080/api/v1/users/2/channel-grants
{"userId":2,"username":"op2","role":"operator","unrestricted":false,
 "channelIds":[1],"groupIds":[3]}

# Replace them atomically from the caller's perspective.
curl -b admin.cookie -H 'X-CSRF-Token: ...' -X PUT \
  -H 'Content-Type: application/json' \
  -d '{"channelIds":[1],"groupIds":[3]}' \
  http://127.0.0.1:8080/api/v1/users/2/channel-grants
```

`channelIds` and `groupIds` must be arrays of positive integers; every id is
checked against `channels` / `channel_groups` before the old grants are
replaced. A user can be granted channels, groups, or both.

## Enforcement points

`App::CheckChannelAccess` / `App::LoadAccessibleChannels` are the single
decision helpers. They are used by:

- **Lists**: `GET /api/v1/channels`, `GET /api/v1/preview/channels`; rows are
  filtered before serialization.
- **Preview**: `POST /api/v1/streams/ticket` and the RTSP `/live/{channel}/{stream}`
  path (an optional `RtspServer::SetChannelAuthorizer` hook, only installed
  when the option is on).
- **Playback**: `POST /api/v1/playback/ticket`, `GET /api/v1/recordings`,
  `GET /api/v1/recordings/days`.
- **Download/export**: `GET /api/v1/recordings/{id}/download`,
  `POST /api/v1/playback/export`, `DELETE /api/v1/recordings/{id}`.
- **Snapshots**: `GET /api/v1/channels/{id}/snapshot`,
  `GET /api/v1/alerts/{id}/snapshot`.
- **Management**: `PUT/DELETE /api/v1/channels/{id}`, encoder read/write,
  reconnect, per-channel AI settings and batch AI settings.
- **AI events**: `GET /api/v1/ai/events` when a `channelId` is supplied,
  and the per-event detail/snapshot endpoints (the event's `channelId` is
  resolved first).
- **Alerts**: the list is filtered (system alerts with `channelId = 0` stay
  visible) and acknowledgement is checked.
- **Private protocol** (`docs/private-protocol.md`): `CHANNEL_LIST`,
  `RECORDING_LIST`, `RECORDING_DAYS`, `LIVE_OPEN` and `DOWNLOAD_OPEN`.

A denied request returns `403 forbidden` with code `forbidden` (or the private
protocol error frame with status 403) and writes a `permission` audit row with
the account, channel, action and result `denied`. Administrators keep the
historical `404` for a missing channel; restricted accounts get `403` so the
existence of a channel is not disclosed.

## Tests

```sh
# Unit: decision helpers, deny-by-default, hierarchy expansion, cycle guard.
cmake -S . -B build-acl -DUVNVR_ENABLE_CHANNEL_ACL=ON -DUVNVR_BUILD_TESTS=ON
cmake --build build-acl -j4
ctest --test-dir build-acl --output-on-failure

# Integration: list/preview/playback/download/snapshot/RTSP/private protocol.
./tests/integration/channel_acl.sh ./build-acl/uvnvr 18097
```

## Known limits

- The dashboard counters, `/ws/status` channel-state broadcast and
  `/api/v1/status/ticket` are deployment-wide; they can still reveal the
  existence of a channel id to an authenticated account. Object content and
  per-channel APIs are protected.
- `GET /api/v1/ai/events` without a `channelId`, `GET /api/v1/ai/plates`
  and `GET /api/v1/ai/stats` are still global queries. Event detail and
  snapshot by event id are checked against the event's channel; the list
  endpoints should be narrowed in a follow-up if AI event metadata is in
  scope for a deployment.
- Grants are per user, as required by ACC-02. Role-level grants
  (`role_channel_grants`) are a possible follow-up for large installations.
