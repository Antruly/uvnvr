#!/usr/bin/env bash
# ACC-02 integration test: object-level channel/group authorization.
#
# The binary must be built with -DUVNVR_ENABLE_CHANNEL_ACL=ON. The test proves
# that an operator only sees granted channels and that list/preview/playback/
# download/snapshot/management requests for an ungranted channel return 403,
# while group grants cover descendant groups.
#
# Usage: tests/integration/channel_acl.sh [path-to-uvnvr] [http-port]
set -euo pipefail

BIN="${1:-./build-acl/uvnvr}"
PORT="${2:-18097}"
BASE="http://127.0.0.1:${PORT}"
RTSP_PORT="$((PORT + 1))"
PRIVATE_PORT="$((PORT + 2))"
DATA="$(mktemp -d)"
ADMIN_COOKIES="$(mktemp)"
OP_COOKIES="$(mktemp)"
LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA}" "${ADMIN_COOKIES}" "${OP_COOKIES}" "${LOG}"
}
trap cleanup EXIT

wait_ready() {
  for _ in $(seq 1 80); do
    if curl -fsS --max-time 2 "${BASE}/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "server did not become ready" >&2
  cat "${LOG}" >&2
  return 1
}

json_get() {
  python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"
}

expect_status() {
  local expected="$1"
  shift
  local actual
  actual="$(curl -sS --max-time 5 -o /tmp/uvnvr-acl-body.$$ -w '%{http_code}' "$@")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "expected HTTP ${expected}, got ${actual}: $*" >&2
    cat /tmp/uvnvr-acl-body.$$ >&2 || true
    echo >&2
    rm -f /tmp/uvnvr-acl-body.$$
    return 1
  fi
  rm -f /tmp/uvnvr-acl-body.$$
}

json_request() {
  local method="$1"
  local url="$2"
  local body="$3"
  curl -fsS --max-time 10 -b "${ADMIN_COOKIES}" \
    -H "X-CSRF-Token: ${ADMIN_CSRF}" -H 'Content-Type: application/json' \
    -X "${method}" -d "${body}" "${BASE}${url}"
}

echo "== ACC-02 channel ACL integration =="
"${BIN}" --port "${PORT}" --rtsp-port "${RTSP_PORT}" \
  --private-port "${PRIVATE_PORT}" --data-dir "${DATA}" >"${LOG}" 2>&1 &
SERVER_PID=$!
wait_ready

setup_response="$(curl -fsS --max-time 10 -c "${ADMIN_COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"password":"Admin123!@#","confirmPassword":"Admin123!@#","timezone":"UTC"}' \
  "${BASE}/api/v1/setup")"
ADMIN_CSRF="$(echo "${setup_response}" | json_get "d['csrfToken']")"
[[ -n "${ADMIN_CSRF}" ]]
ADMIN_ID="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" \
  "${BASE}/api/v1/auth/me" | json_get "d['user']['id']")"

# The ACL routes only exist when the feature is compiled in. Fail loudly rather
# than silently passing the authorization assertions on an unprotected build.
expect_status 200 -b "${ADMIN_COOKIES}" "${BASE}/api/v1/users/${ADMIN_ID}/channel-grants"

create_channel() {
  local name="$1"
  json_request POST /api/v1/channels \
    "{\"name\":\"${name}\",\"protocol\":\"rtsp\",\"host\":\"127.0.0.1\",\"port\":8554,\"mainUrl\":\"rtsp://127.0.0.1:9/none\"}" \
    | json_get "d['id']"
}

ALPHA_ID="$(create_channel cam-alpha)"
BETA_ID="$(create_channel cam-beta)"
GAMMA_ID="$(create_channel cam-gamma)"
[[ "${ALPHA_ID}" == "1" && "${BETA_ID}" == "2" && "${GAMMA_ID}" == "3" ]]

# Two-level group tree: site -> floor1. A grant on "site" must cover
# channels assigned to "floor1".
SITE_ID="$(json_request POST /api/v1/channel-groups '{"name":"site"}' | json_get "d['id']")"
FLOOR_ID="$(json_request POST /api/v1/channel-groups \
  "{\"name\":\"floor1\",\"parentId\":${SITE_ID}}" | json_get "d['id']")"
json_request PUT "/api/v1/channels/${BETA_ID}" "{\"groupId\":${FLOOR_ID}}" >/dev/null

# Operator account (built-in role id 2).
OP_ID="$(json_request POST /api/v1/users \
  '{"username":"op2","password":"Operator123!","roleId":2}' | json_get "d['id']")"
[[ "${OP_ID}" -gt 0 ]]

op_login="$(curl -fsS --max-time 10 -c "${OP_COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"username":"op2","password":"Operator123!"}' "${BASE}/api/v1/auth/login")"
OP_CSRF="$(echo "${op_login}" | json_get "d['csrfToken']")"
[[ -n "${OP_CSRF}" ]]

operator_channels() {
  curl -fsS --max-time 10 -b "${OP_COOKIES}" "${BASE}/api/v1/channels"
}

# Deny by default: a fresh non-admin account sees nothing.
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" \
  "${BASE}/api/v1/channels/${BETA_ID}/snapshot"
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" \
  -H 'Content-Type: application/json' -d "{\"channelId\":${BETA_ID},\"stream\":\"main\"}" \
  "${BASE}/api/v1/streams/ticket"
[[ "$(operator_channels | json_get 'len(d)')" == "0" ]]

# Grant one group: only the channel in that group is visible, and an explicit
# request for the ungrouped channel is rejected.
json_request PUT "/api/v1/users/${OP_ID}/channel-grants" \
  "{\"channelIds\":[],\"groupIds\":[${SITE_ID}]}" >/dev/null
visible="$(operator_channels)"
[[ "$(echo "${visible}" | json_get 'len(d)')" == "1" ]]
[[ "$(echo "${visible}" | json_get "d[0]['name']")" == "cam-beta" ]]
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" \
  "${BASE}/api/v1/channels/${ALPHA_ID}/snapshot"

# Add the direct channel grant: both channels are visible.
json_request PUT "/api/v1/users/${OP_ID}/channel-grants" \
  "{\"channelIds\":[${ALPHA_ID}],\"groupIds\":[${SITE_ID}]}" >/dev/null
[[ "$(operator_channels | json_get 'len(d)')" == "2" ]]

grants="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" \
  "${BASE}/api/v1/users/${OP_ID}/channel-grants")"
[[ "$(echo "${grants}" | json_get "d['channelIds']")" == "[${ALPHA_ID}]" ]]
[[ "$(echo "${grants}" | json_get "d['groupIds']")" == "[${SITE_ID}]" ]]

# Object-level checks across every read/write path that takes a channel id.
# cam-gamma was never granted, so all of these must stop at the ACL.
expect_status 403 -b "${OP_COOKIES}" "${BASE}/api/v1/channels/${GAMMA_ID}/snapshot"
expect_status 403 -b "${OP_COOKIES}" "${BASE}/api/v1/channels/${GAMMA_ID}/encoder"
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" -X POST \
  "${BASE}/api/v1/channels/${GAMMA_ID}/reconnect"
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" -X PUT \
  -H 'Content-Type: application/json' -d '{}' "${BASE}/api/v1/channels/${GAMMA_ID}"
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" -X POST \
  -H 'Content-Type: application/json' \
  -d "{\"channelId\":${GAMMA_ID},\"stream\":\"main\"}" "${BASE}/api/v1/streams/ticket"
expect_status 403 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" -X POST \
  -H 'Content-Type: application/json' \
  -d "{\"channelId\":${GAMMA_ID},\"stream\":\"main\",\"startMs\":1,\"endMs\":2}" \
  "${BASE}/api/v1/playback/ticket"
expect_status 403 -b "${OP_COOKIES}" \
  "${BASE}/api/v1/recordings?channelId=${GAMMA_ID}"
expect_status 403 -b "${OP_COOKIES}" \
  "${BASE}/api/v1/recordings/days?channelId=${GAMMA_ID}&from=2026-01-01&to=2026-01-02"

# The granted channel still reaches business logic. There is no camera at
# 127.0.0.1:9, so a ticket (not media) is the stable assertion.
expect_status 200 -b "${OP_COOKIES}" -H "X-CSRF-Token: ${OP_CSRF}" -X POST \
  -H 'Content-Type: application/json' \
  -d "{\"channelId\":${ALPHA_ID},\"stream\":\"main\"}" "${BASE}/api/v1/streams/ticket"

# The private protocol is a second access path and must apply the same object
# ACL. This is a minimal TCP client (HELLO -> AUTH -> CHANNEL_LIST/LIVE_OPEN).
python3 - "127.0.0.1" "${PRIVATE_PORT}" "${GAMMA_ID}" <<'PY'
import json
import socket
import struct
import sys
import zlib

host, port, forbidden_id = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
MAGIC = 0x524E5655
HEADER = struct.Struct("<IBBHHHQIII")


def encode(opcode, request_id, session, payload, flags=0):
    if not isinstance(payload, (bytes, bytearray)):
        payload = json.dumps(payload).encode()
    header = bytearray(HEADER.pack(MAGIC, 1, 32, flags, opcode, 0, session,
                                   request_id, len(payload), 0))
    crc = zlib.crc32(bytes(header) + payload) & 0xFFFFFFFF
    struct.pack_into("<I", header, 28, crc)
    return bytes(header) + payload


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("connection closed")
        data += chunk
    return data


def read_frame(sock):
    raw = recv_exact(sock, HEADER.size)
    magic, version, header_len, flags, opcode, _reserved, session, request_id, length, crc = \
        HEADER.unpack(raw)
    payload = recv_exact(sock, length)
    expected = bytearray(raw)
    struct.pack_into("<I", expected, 28, 0)
    if zlib.crc32(bytes(expected) + payload) & 0xFFFFFFFF != crc:
        raise RuntimeError("bad CRC")
    if magic != MAGIC or version != 1 or header_len != 32:
        raise RuntimeError("bad frame header")
    return flags, opcode, session, request_id, payload


with socket.create_connection((host, int(port)), timeout=5) as sock:
    sock.sendall(encode(0x0001, 1, 0, {
        "protocol": 1, "client": "acl-test", "transports": ["tcp"],
        "features": [], "resumeSessionId": 0}))
    flags, opcode, _session, request_id, _payload = read_frame(sock)
    assert opcode == 0x0002 and request_id == 1, "HELLO_ACK missing"

    sock.sendall(encode(0x0003, 2, 0, {
        "username": "op2", "password": "Operator123!",
        "clientName": "acl-test", "resumeSessionId": 0}))
    flags, opcode, session, request_id, payload = read_frame(sock)
    assert opcode == 0x0004 and (flags & 0x0002) == 0, payload
    session = json.loads(payload)["sessionId"]

    sock.sendall(encode(0x0020, 3, session, b""))
    flags, opcode, _session, request_id, payload = read_frame(sock)
    channels = json.loads(payload)
    assert opcode == 0x0020 and len(channels) == 2, channels

    sock.sendall(encode(0x0030, 4, session, {
        "channelId": forbidden_id, "stream": "main", "streamId": 17}))
    flags, opcode, _session, request_id, payload = read_frame(sock)
    error = json.loads(payload)
    assert (flags & 0x0002) and error.get("status") == 403, error

    sock.sendall(encode(0x0021, 5, session, {"channelId": forbidden_id}))
    flags, opcode, _session, request_id, payload = read_frame(sock)
    error = json.loads(payload)
    assert (flags & 0x0002) and error.get("status") == 403, error

print("private protocol ACL checks passed")
PY

# Denials are audited with the permission category.
audit="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" \
  "${BASE}/api/v1/audit?category=permission&pageSize=200")"
denials="$(echo "${audit}" | python3 -c '
import json,sys
items=json.load(sys.stdin).get("items",[])
print(sum(1 for i in items if i.get("actor")=="op2" and i.get("result")=="denied"))
')"
[[ "${denials}" -ge 5 ]]

grant_audit="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" \
  "${BASE}/api/v1/audit?category=permission&pageSize=200&q=channel_grants")"
[[ "$(echo "${grant_audit}" | json_get "d['total']")" -ge 1 ]]

# Removing every grant restores deny-by-default.
json_request PUT "/api/v1/users/${OP_ID}/channel-grants" \
  '{"channelIds":[],"groupIds":[]}' >/dev/null
[[ "$(operator_channels | json_get 'len(d)')" == "0" ]]

echo "channel ACL integration passed: denials=${denials}"
