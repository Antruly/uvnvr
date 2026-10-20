#!/usr/bin/env bash
# M1 integration smoke test: first-run setup, sessions, CSRF, RBAC data,
# audit, API tokens and persistence across a restart.
#
# Usage: tests/integration/m1_auth.sh [path-to-uvnvr] [port]
set -euo pipefail

BIN="${1:-./build/uvnvr}"
PORT="${2:-18090}"
BASE="http://127.0.0.1:${PORT}"
DATA="$(mktemp -d)"
COOKIES="$(mktemp)"
LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA}" "${COOKIES}" "${LOG}"
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

stop_server() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
  fi
}

json_field() {
  python3 -c "import json,sys; print(json.load(sys.stdin).get('$1',''))"
}

expect_status() {
  local expected="$1"
  shift
  local actual
  actual="$(curl -sS --max-time 5 -o /tmp/uvnvr-test-body.$$ -w '%{http_code}' "$@")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "expected HTTP ${expected}, got ${actual}" >&2
    cat /tmp/uvnvr-test-body.$$ >&2 || true
    rm -f /tmp/uvnvr-test-body.$$
    return 1
  fi
  rm -f /tmp/uvnvr-test-body.$$
}

echo "== M1 integration =="
start_server

setup_status="$(curl -fsS --max-time 5 "${BASE}/api/v1/setup/status")"
[[ "$(echo "${setup_status}" | json_field setupRequired)" == "True" ]]

setup_response="$(curl -fsS --max-time 10 -c "${COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"password":"Admin123!@#","confirmPassword":"Admin123!@#","timezone":"Asia/Shanghai"}' \
  "${BASE}/api/v1/setup")"
CSRF="$(echo "${setup_response}" | json_field csrfToken)"
[[ -n "${CSRF}" ]]

setup_status="$(curl -fsS --max-time 5 "${BASE}/api/v1/setup/status")"
[[ "$(echo "${setup_status}" | json_field setupRequired)" == "False" ]]

expect_status 200 -b "${COOKIES}" "${BASE}/api/v1/auth/me"
expect_status 200 -b "${COOKIES}" "${BASE}/api/v1/dashboard/summary"
expect_status 200 -b "${COOKIES}" "${BASE}/api/v1/users"
expect_status 200 -b "${COOKIES}" "${BASE}/api/v1/roles"

# Mutating requests require the CSRF header carried by the session.
expect_status 403 -b "${COOKIES}" -H 'Content-Type: application/json' \
  -d '{"username":"operator1","password":"Operator123!","roleId":2}' \
  "${BASE}/api/v1/users"

user_response="$(curl -fsS --max-time 10 -b "${COOKIES}" \
  -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
  -d '{"username":"operator1","password":"Operator123!","roleId":2}' \
  "${BASE}/api/v1/users")"
[[ "$(echo "${user_response}" | json_field username)" == "operator1" ]]

audit_response="$(curl -fsS --max-time 5 -b "${COOKIES}" "${BASE}/api/v1/audit?page=1&pageSize=10")"
[[ "$(echo "${audit_response}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["total"])')" -ge 2 ]]

token_response="$(curl -fsS --max-time 10 -b "${COOKIES}" \
  -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json' \
  -d '{"name":"integration","expiresInDays":1}' "${BASE}/api/v1/tokens")"
TOKEN="$(echo "${token_response}" | json_field token)"
[[ -n "${TOKEN}" ]]
expect_status 200 -H "Authorization: Bearer ${TOKEN}" "${BASE}/api/v1/dashboard/summary"

# The API lockout is independent of the web lockout. Lower the threshold, then
# prove that invalid bearer tokens trip it.
curl -fsS --max-time 5 -b "${COOKIES}" -X PUT   -H "X-CSRF-Token: ${CSRF}" -H 'Content-Type: application/json'   -d '{"security":{"apiMaxFailures":2}}' "${BASE}/api/v1/settings" >/dev/null
expect_status 401 -H "Authorization: Bearer invalid-token" "${BASE}/api/v1/dashboard/summary"
expect_status 401 -H "Authorization: Bearer invalid-token" "${BASE}/api/v1/dashboard/summary"
expect_status 429 -H "Authorization: Bearer invalid-token" "${BASE}/api/v1/dashboard/summary"

expect_status 200 -b "${COOKIES}" -X POST -H "X-CSRF-Token: ${CSRF}" "${BASE}/api/v1/auth/logout"
expect_status 401 -b "${COOKIES}" "${BASE}/api/v1/auth/me"

# Persistence: the same data directory must keep the admin and settings.
stop_server
start_server
login_response="$(curl -fsS --max-time 10 -c "${COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"Admin123!@#"}' "${BASE}/api/v1/auth/login")"
[[ "$(echo "${login_response}" | json_field csrfToken)" != "" ]]
expect_status 200 -b "${COOKIES}" "${BASE}/api/v1/dashboard/summary"

echo "M1 integration passed"
