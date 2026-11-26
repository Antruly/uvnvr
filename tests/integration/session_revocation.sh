#!/usr/bin/env bash
# GH-29 / GA/T 2155.2 IDN-08 integration test.
#
# Changing an account's password or role, or disabling it, must invalidate every
# credential that was issued before the change: the old session cookie must
# return 401 on the next request and a previously minted API token must be
# rejected. Disabling is the regression case (it already deleted sessions).
#
# Usage: tests/integration/session_revocation.sh [path-to-uvnvr] [port]
set -euo pipefail

BIN="${1:-./build/uvnvr}"
PORT="${2:-18110}"
BASE="http://127.0.0.1:${PORT}"
DATA="$(mktemp -d)"
ADMIN_COOKIES="${DATA}/admin.cookies"
LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA}" "${LOG}"
}
trap cleanup EXIT

wait_ready() {
  for _ in $(seq 1 50); do
    if curl -fsS --max-time 2 "${BASE}/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "server did not become ready" >&2
  cat "${LOG}" >&2
  return 1
}

start_server() {
  "${BIN}" --port "${PORT}" --data-dir "${DATA}" >"${LOG}" 2>&1 &
  SERVER_PID=$!
  wait_ready
}

json_field() {
  python3 -c "import json,sys; print(json.load(sys.stdin).get('$1',''))"
}

expect_status() {
  local expected="$1"
  shift
  local body
  body="$(mktemp)"
  local actual
  actual="$(curl -sS --max-time 5 -o "${body}" -w '%{http_code}' "$@")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "expected HTTP ${expected}, got ${actual}" >&2
    cat "${body}" >&2 || true
    rm -f "${body}"
    return 1
  fi
  rm -f "${body}"
}

role_id() {
  # $1 = roles JSON, $2 = role name
  echo "$1" | python3 -c \
    "import json,sys; print(next(r['id'] for r in json.load(sys.stdin) if r['name']=='$2'))"
}

login_user() {
  # $1 = username, $2 = password, $3 = cookie jar; prints the CSRF token
  local response
  response="$(curl -fsS --max-time 10 -c "$3" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$1\",\"password\":\"$2\"}" \
    "${BASE}/api/v1/auth/login")"
  echo "${response}" | json_field csrfToken
}

create_user() {
  # $1 = username; prints the new user id
  local response
  response="$(curl -fsS --max-time 10 -b "${ADMIN_COOKIES}" \
    -H "X-CSRF-Token: ${ADMIN_CSRF}" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$1\",\"password\":\"Operator123!\",\"roleId\":${OPERATOR_ROLE}}" \
    "${BASE}/api/v1/users")"
  echo "${response}" | json_field id
}

update_user() {
  # $1 = user id, $2 = JSON body
  curl -fsS --max-time 10 -b "${ADMIN_COOKIES}" -X PUT \
    -H "X-CSRF-Token: ${ADMIN_CSRF}" -H 'Content-Type: application/json' \
    -d "$2" "${BASE}/api/v1/users/$1" >/dev/null
}

echo "== GH-29 session revocation =="
start_server

setup_response="$(curl -fsS --max-time 10 -c "${ADMIN_COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"password":"Admin123!@#","confirmPassword":"Admin123!@#","timezone":"Asia/Shanghai"}' \
  "${BASE}/api/v1/setup")"
ADMIN_CSRF="$(echo "${setup_response}" | json_field csrfToken)"
[[ -n "${ADMIN_CSRF}" ]]

roles="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" "${BASE}/api/v1/roles")"
OPERATOR_ROLE="$(role_id "${roles}" operator)"
VIEWER_ROLE="$(role_id "${roles}" viewer)"

PW_ID="$(create_user op-pw)"
ROLE_ID_USER="$(create_user op-role)"
DIS_ID="$(create_user op-disable)"
[[ -n "${PW_ID}" && -n "${ROLE_ID_USER}" && -n "${DIS_ID}" ]]

# --- 1. password change revokes the old cookie and API token ---------------
PW_CSRF="$(login_user op-pw 'Operator123!' "${DATA}/pw.cookies")"
[[ -n "${PW_CSRF}" ]]
expect_status 200 -b "${DATA}/pw.cookies" "${BASE}/api/v1/auth/me"

token_response="$(curl -fsS --max-time 10 -b "${DATA}/pw.cookies" \
  -H "X-CSRF-Token: ${PW_CSRF}" -H 'Content-Type: application/json' \
  -d '{"name":"gh29","expiresInDays":1}' "${BASE}/api/v1/tokens")"
OP_TOKEN="$(echo "${token_response}" | json_field token)"
[[ -n "${OP_TOKEN}" ]]
expect_status 200 -H "Authorization: Bearer ${OP_TOKEN}" "${BASE}/api/v1/dashboard/summary"

update_user "${PW_ID}" '{"password":"NewOperator123!"}'

expect_status 401 -b "${DATA}/pw.cookies" "${BASE}/api/v1/auth/me"
expect_status 401 -H "Authorization: Bearer ${OP_TOKEN}" "${BASE}/api/v1/dashboard/summary"
# The old password no longer logs in; the new one does.
expect_status 401 -H 'Content-Type: application/json' \
  -d '{"username":"op-pw","password":"Operator123!"}' "${BASE}/api/v1/auth/login"
NEW_PW_CSRF="$(login_user op-pw 'NewOperator123!' "${DATA}/pw-new.cookies")"
[[ -n "${NEW_PW_CSRF}" ]]

# --- 2. role change revokes the old cookie ---------------------------------
ROLE_CSRF="$(login_user op-role 'Operator123!' "${DATA}/role.cookies")"
[[ -n "${ROLE_CSRF}" ]]
expect_status 200 -b "${DATA}/role.cookies" "${BASE}/api/v1/auth/me"

update_user "${ROLE_ID_USER}" "{\"roleId\":${VIEWER_ROLE}}"

expect_status 401 -b "${DATA}/role.cookies" "${BASE}/api/v1/auth/me"
# The password is unchanged, so a fresh login still works and sees the new role.
ROLE_NEW_CSRF="$(login_user op-role 'Operator123!' "${DATA}/role-new.cookies")"
[[ -n "${ROLE_NEW_CSRF}" ]]
expect_status 200 -b "${DATA}/role-new.cookies" "${BASE}/api/v1/auth/me"
new_role="$(curl -fsS -b "${DATA}/role-new.cookies" "${BASE}/api/v1/auth/me" |
  python3 -c 'import json,sys; print(json.load(sys.stdin)["user"]["role"])')"
[[ "${new_role}" == "viewer" ]]

# --- 3. disable revokes the old cookie (regression) ------------------------
DIS_CSRF="$(login_user op-disable 'Operator123!' "${DATA}/disable.cookies")"
[[ -n "${DIS_CSRF}" ]]
expect_status 200 -b "${DATA}/disable.cookies" "${BASE}/api/v1/auth/me"

update_user "${DIS_ID}" '{"enabled":false}'

expect_status 401 -b "${DATA}/disable.cookies" "${BASE}/api/v1/auth/me"
# A disabled account cannot obtain a replacement session either.
expect_status 403 -H 'Content-Type: application/json' \
  -d '{"username":"op-disable","password":"Operator123!"}' "${BASE}/api/v1/auth/login"

# --- 4. audit trail records the revocation --------------------------------
AUDIT_JSON="$(curl -fsS --max-time 5 -b "${ADMIN_COOKIES}" \
  "${BASE}/api/v1/audit?category=user&pageSize=200")"
PW_ID="${PW_ID}" ROLE_ID_USER="${ROLE_ID_USER}" DIS_ID="${DIS_ID}" AUDIT_JSON="${AUDIT_JSON}" \
  python3 - <<'PY'
import json
import os

items = json.loads(os.environ["AUDIT_JSON"])["items"]
want = {
    int(os.environ["PW_ID"]): "password",
    int(os.environ["ROLE_ID_USER"]): "role",
    int(os.environ["DIS_ID"]): "disable",
}
evidence = {}
for item in items:
    if item.get("category") != "user" or item.get("action") != "update":
        continue
    if item.get("result") != "success":
        print("unexpected failed user update:", item, file=__import__("sys").stderr)
        raise SystemExit(1)
    try:
        object_id = int(item.get("objectId"))
    except (TypeError, ValueError):
        continue
    if object_id not in want:
        continue
    detail = json.loads(item.get("detail") or "{}")
    if object_id in evidence:
        continue  # keep the first matching update for each account
    evidence[object_id] = detail

def require(condition, message):
    if not condition:
        raise SystemExit("audit check failed: " + message)

require(int(os.environ["PW_ID"]) in evidence, "missing password-change audit entry")
password = evidence[int(os.environ["PW_ID"])]
require(password.get("passwordChanged") is True, "passwordChanged must be true")
require(password.get("revokedSessions", 0) >= 1, "password change must count revoked sessions")
require(password.get("revokedApiTokens", 0) >= 1, "password change must count revoked tokens")

require(int(os.environ["ROLE_ID_USER"]) in evidence, "missing role-change audit entry")
role = evidence[int(os.environ["ROLE_ID_USER"])]
require(role.get("passwordChanged") is False, "role change must not claim a password change")
require(role.get("revokedSessions", 0) >= 1, "role change must count revoked sessions")

require(int(os.environ["DIS_ID"]) in evidence, "missing disable audit entry")
disable = evidence[int(os.environ["DIS_ID"])]
require(disable.get("enabled") is False, "disable audit must record enabled=false")
require(disable.get("revokedSessions", 0) >= 1, "disable must count revoked sessions")

print("audit_evidence=" + json.dumps({str(k): v for k, v in evidence.items()}, sort_keys=True))
PY

echo "GH-29 session revocation integration passed"
