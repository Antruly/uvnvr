#!/usr/bin/env bash
# 0.2.0 integration test: JSON startup configuration, CLI precedence and the
# read-only system configuration endpoint.
#
# Usage: tests/integration/config.sh [path-to-uvnvr] [base-port]
set -euo pipefail

BIN="${1:-./build/uvnvr}"
BASE_PORT="${2:-18200}"
HTTP_PORT="${BASE_PORT}"
OVERRIDE_PORT="$((BASE_PORT + 1))"
RTSP_PORT="$((BASE_PORT + 2))"
BASE="http://127.0.0.1:${HTTP_PORT}"
DATA="$(mktemp -d)"
CONFIG="${DATA}/uvnvr.json"
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
  local base="$1"
  for _ in $(seq 1 60); do
    if curl -fsS --max-time 2 "${base}/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "server did not become ready at ${base}" >&2
  cat "${LOG}" >&2
  return 1
}

start_server() {
  "${BIN}" "$@" >"${LOG}" 2>&1 &
  SERVER_PID=$!
}

stop_server() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
  fi
}

json_get() {
  python3 -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))' "$1"
}

echo "== configuration integration =="

cat > "${CONFIG}" <<EOF
{
  "http": { "port": ${HTTP_PORT}, "host": "127.0.0.1" },
  "rtsp": { "port": ${RTSP_PORT} },
  "dataDir": "${DATA}/data",
  "logging": { "level": "debug" }
}
EOF

start_server --config "${CONFIG}"
wait_ready "${BASE}"
grep -q "config: ${CONFIG}" "${LOG}"

setup_response="$(curl -fsS --max-time 10 -c "${COOKIES}" \
  -H 'Content-Type: application/json' \
  -d '{"password":"Admin123!@#","confirmPassword":"Admin123!@#","timezone":"UTC"}' \
  "${BASE}/api/v1/setup")"
CSRF="$(echo "${setup_response}" | json_get "d['csrfToken']")"
[[ -n "${CSRF}" ]]

config_json="$(curl -fsS --max-time 5 -b "${COOKIES}" "${BASE}/api/v1/system/config")"
[[ "$(echo "${config_json}" | json_get "d['http']['port']")" == "${HTTP_PORT}" ]]
[[ "$(echo "${config_json}" | json_get "d['rtsp']['port']")" == "${RTSP_PORT}" ]]
[[ "$(echo "${config_json}" | json_get "d['config']['loaded']")" == "True" ]]
[[ "$(echo "${config_json}" | json_get "d['dataDir']")" == "${DATA}/data" ]]

# CLI flags override the file.
stop_server
start_server --config "${CONFIG}" --port "${OVERRIDE_PORT}"
wait_ready "http://127.0.0.1:${OVERRIDE_PORT}"
effective="$("${BIN}" --config "${CONFIG}" --port "${OVERRIDE_PORT}" --print-config)"
[[ "$(echo "${effective}" | json_get "d['http']['port']")" == "${OVERRIDE_PORT}" ]]
stop_server

# Invalid configuration must be rejected before the service starts.
cat > "${CONFIG}" <<EOF
{ "http": { "port": 8080 }, "rtsp": { "port": 8080 } }
EOF
if "${BIN}" --config "${CONFIG}" >/dev/null 2>&1; then
  echo "expected a validation failure for identical ports" >&2
  exit 1
fi

if "${BIN}" --config "${DATA}/missing.json" >/dev/null 2>&1; then
  echo "expected a failure for a missing explicit config" >&2
  exit 1
fi

echo "configuration integration passed"
