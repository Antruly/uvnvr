#!/usr/bin/env bash
# Verify that a GitHub release carries the release asset matrix and that
# SHA256SUMS matches the attached files.
#
#   scripts/verify-release.sh [version] [repo]
#
# Requires the GitHub CLI and an authenticated session (GH_TOKEN in CI).
set -euo pipefail

VERSION="${1:-$(cat "$(dirname "${BASH_SOURCE[0]}")/../VERSION")}"
REPO="${2:-Antruly/uvnvr}"
TAG="v${VERSION}"
DIR="$(mktemp -d)"
trap 'rm -rf "${DIR}"' EXIT

failures=0
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

echo "== downloading ${REPO} ${TAG} =="
for attempt in 1 2 3; do
  if gh release download "${TAG}" --repo "${REPO}" --dir "${DIR}" --clobber; then
    break
  fi
  if [[ "${attempt}" -eq 3 ]]; then
    echo "download failed after ${attempt} attempts" >&2
    exit 1
  fi
  sleep 3
done

expected=(
  "uvnvr-${VERSION}-windows-x64-setup.exe"
  "uvnvr-${VERSION}-windows-x64.zip"
  "uvnvr-${VERSION}-windows-arm64-setup.exe"
  "uvnvr-${VERSION}-windows-arm64.zip"
  "uvnvr_${VERSION}_amd64.deb"
  "uvnvr_${VERSION}_arm64.deb"
  "uvnvr-${VERSION}-1.x86_64.rpm"
  "uvnvr-${VERSION}-1.aarch64.rpm"
  "uvnvr-${VERSION}-linux-x64.tar.gz"
  "uvnvr-${VERSION}-linux-arm64.tar.gz"
  "uvnvr-${VERSION}-macos-x64.pkg"
  "uvnvr-${VERSION}-macos-x64.dmg"
  "uvnvr-${VERSION}-macos-x64.zip"
  "uvnvr-${VERSION}-macos-arm64.pkg"
  "uvnvr-${VERSION}-macos-arm64.dmg"
  "uvnvr-${VERSION}-macos-arm64.zip"
  "uvnvr-${VERSION}-sbom.spdx.json"
  "FFmpeg-LGPLv3.txt"
  "ffmpeg-source-46d899d7a0.tar.gz"
  "SHA256SUMS"
)

echo "== checking ${#expected[@]} expected assets =="
for name in "${expected[@]}"; do
  path="${DIR}/${name}"
  if [[ ! -f "${path}" ]]; then
    fail "missing ${name}"
  elif [[ ! -s "${path}" ]]; then
    fail "empty ${name}"
  else
    printf 'ok   %-48s %s\n' "${name}" "$(du -h "${path}" | cut -f1)"
  fi
done

if [[ -f "${DIR}/SHA256SUMS" ]]; then
  echo "== verifying checksums =="
  if (cd "${DIR}" && sha256sum -c SHA256SUMS); then
    echo "ok   SHA256SUMS"
  else
    fail "SHA256SUMS does not match the attached assets"
  fi

  if grep -q ' SHA256SUMS$' "${DIR}/SHA256SUMS"; then
    fail "SHA256SUMS contains itself"
  fi
fi

if [[ -f "${DIR}/uvnvr-${VERSION}-sbom.spdx.json" ]]; then
  if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
      "${DIR}/uvnvr-${VERSION}-sbom.spdx.json"; then
    echo "ok   SBOM is valid JSON"
  else
    fail "SBOM is not valid JSON"
  fi
fi

if (( failures > 0 )); then
  echo "release verification failed: ${failures} problem(s)" >&2
  exit 1
fi
echo "release verification passed"
