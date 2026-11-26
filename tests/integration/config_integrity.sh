#!/usr/bin/env bash
# Configuration integrity integration test: digest/signature manifests, the
# warn/enforce startup policy, unknown-key warnings and the signed-manifest
# downgrade guard.
#
# Requires a binary built with -DUVNVR_ENABLE_CONFIG_INTEGRITY=ON and the
# openssl CLI (OpenSSL 3) for the SM2 key pair.
#
# Usage: tests/integration/config_integrity.sh [path-to-uvnvr] [base-port]
set -euo pipefail

BIN="${1:-./build/uvnvr}"
BASE_PORT="${2:-19400}"
HTTP_PORT="${BASE_PORT}"
RTSP_PORT="$((BASE_PORT + 1))"
PRIVATE_PORT="$((BASE_PORT + 2))"
DATA="$(mktemp -d)"
CONFIG="${DATA}/uvnvr.json"
MANIFEST="${CONFIG}.sm3"
PRIVATE_KEY="${DATA}/config-signing-key.pem"
PUBLIC_KEY="${DATA}/config-signing-key.pub.pem"
LOG="${DATA}/server.log"
SERVER_PID=""

fail() {
  echo "config integrity integration FAILED: $*" >&2
  exit 1
}

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA}"
}
trap cleanup EXIT

write_config() {
  local http_port="$1"
  cat >"${CONFIG}" <<EOF
{
  "http": { "port": ${http_port}, "host": "127.0.0.1" },
  "rtsp": { "port": ${RTSP_PORT} },
  "private": { "port": ${PRIVATE_PORT} },
  "dataDir": "${DATA}/data",
  "logging": { "level": "info" }
}
EOF
}

expect_failure() {
  local name="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    fail "${name}: command unexpectedly succeeded"
  fi
}

echo "== configuration integrity integration =="

mkdir -p "${DATA}/data"
write_config "${HTTP_PORT}"

if ! "${BIN}" --config "${CONFIG}" --write-config-digest >/dev/null 2>&1; then
  echo "SKIP: ${BIN} was built without UVNVR_ENABLE_CONFIG_INTEGRITY" >&2
  exit 77
fi

# 1. A manifest generated from the current bytes verifies.
"${BIN}" --config "${CONFIG}" --verify-config | grep -q '^OK ' ||
  fail "fresh manifest did not verify"
grep -q '^uvnvr-config-integrity-v1$' "${MANIFEST}" || fail "manifest header missing"

# 2. Editing the file is detected and reported with both digests.
sed -i.bak "s/\"port\": ${HTTP_PORT}/\"port\": $((HTTP_PORT + 10))/" "${CONFIG}"
if "${BIN}" --config "${CONFIG}" --verify-config >"${DATA}/verify.out" 2>&1; then
  fail "tampered configuration still verified"
fi
grep -q '^FAIL ' "${DATA}/verify.out" || fail "tampered verification did not print FAIL"
grep -q 'expected sm3' "${DATA}/verify.out" || fail "tampered verification lacks digests"

# 3. warn (default) starts and logs the mismatch; enforce refuses to start.
start_warn() {
  "${BIN}" --config "${CONFIG}" --config-integrity warn --write-config-digest >/dev/null 2>&1 || true
  sed -i "s/\"port\": $((HTTP_PORT + 10))/\"port\": $((HTTP_PORT + 11))/" "${CONFIG}"
  "${BIN}" --config "${CONFIG}" --config-integrity warn >"${LOG}" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 50); do
    if grep -q 'configuration warning: config integrity:' "${LOG}"; then
      kill "${SERVER_PID}" 2>/dev/null || true
      wait "${SERVER_PID}" 2>/dev/null || true
      SERVER_PID=""
      return 0
    fi
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
      cat "${LOG}" >&2
      SERVER_PID=""
      return 1
    fi
    sleep 0.1
  done
  kill "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
  SERVER_PID=""
  cat "${LOG}" >&2
  return 1
}
start_warn || fail "warn mode did not log the integrity warning"

expect_failure "enforce mode" \
  "${BIN}" --config "${CONFIG}" --config-integrity enforce

# 4. Unknown keys are reported, known keys are not.
write_config "${HTTP_PORT}"
"${BIN}" --config "${CONFIG}" --write-config-digest >/dev/null
sed -i 's/"logging": { "level": "info" }/"logging": { "level": "info", "levl": "debug" }, "maintenence": true/' "${CONFIG}"
"${BIN}" --config "${CONFIG}" --write-config-digest >"${DATA}/unknown.out" 2>&1
grep -q 'unknown key "maintenence"' "${DATA}/unknown.out" || fail "root unknown key not reported"
grep -q 'unknown key "logging.levl"' "${DATA}/unknown.out" || fail "nested unknown key not reported"

# 5. Signed manifests: correct key verifies, a changed file or forged signature
#    does not, and digest-only manifests are refused when a key is configured.
write_config "${HTTP_PORT}"
openssl genpkey -algorithm SM2 -out "${PRIVATE_KEY}" 2>/dev/null ||
  fail "cannot generate an SM2 key (OpenSSL 3 is required)"
openssl pkey -in "${PRIVATE_KEY}" -pubout -out "${PUBLIC_KEY}" 2>/dev/null ||
  fail "cannot export the SM2 public key"

"${BIN}" --config "${CONFIG}" --sign-config "${PRIVATE_KEY}" >/dev/null
grep -q '^signature ' "${MANIFEST}" || fail "signed manifest has no signature"
"${BIN}" --config "${CONFIG}" --config-public-key "${PUBLIC_KEY}" --verify-config |
  grep -q 'SM2 signature verified' || fail "signed manifest did not verify"

sed -i.bak "s/\"port\": ${HTTP_PORT}/\"port\": $((HTTP_PORT + 12))/" "${CONFIG}"
expect_failure "signed tamper" \
  "${BIN}" --config "${CONFIG}" --config-public-key "${PUBLIC_KEY}" --verify-config
sed -i.bak "s/\"port\": $((HTTP_PORT + 12))/\"port\": ${HTTP_PORT}/" "${CONFIG}"

sed -i 's/^signature 30/signature 31/' "${MANIFEST}"
expect_failure "forged signature" \
  "${BIN}" --config "${CONFIG}" --config-public-key "${PUBLIC_KEY}" --verify-config

"${BIN}" --config "${CONFIG}" --sign-config "${PRIVATE_KEY}" >/dev/null
"${BIN}" --config "${CONFIG}" --write-config-digest >/dev/null
expect_failure "digest-only downgrade" \
  "${BIN}" --config "${CONFIG}" --config-public-key "${PUBLIC_KEY}" --verify-config

# 6. A signed configuration starts under enforce with the public key.
"${BIN}" --config "${CONFIG}" --sign-config "${PRIVATE_KEY}" >/dev/null
timeout 2 "${BIN}" --config "${CONFIG}" --config-public-key "${PUBLIC_KEY}" \
  --config-integrity enforce >/dev/null 2>"${DATA}/enforce-ok.err" || status=$?
status="${status:-0}"
[[ "${status}" == "124" ]] || {
  cat "${DATA}/enforce-ok.err" >&2
  fail "signed configuration did not start under enforce (status ${status})"
}
if grep -q 'config integrity' "${DATA}/enforce-ok.err"; then
  fail "signed configuration logged an integrity error under enforce"
fi

echo "configuration integrity integration passed"
