#!/usr/bin/env bash
# Stage and package a macOS build. dylibbundler must be available.
set -euo pipefail
BUILD_DIR="build"
VERSION="0.9.0"
ARCH="arm64"
OUT="dist-packages"
FFMPEG_BIN=""
SEARCH_PATHS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD_DIR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --ffmpeg) FFMPEG_BIN="$2"; shift 2 ;;
    --search-path) SEARCH_PATHS+=("$2"); shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[[ -x "${BUILD_DIR}/uvnvr" ]] || { echo "missing ${BUILD_DIR}/uvnvr" >&2; exit 1; }
[[ -n "${FFMPEG_BIN}" && -x "${FFMPEG_BIN}" ]] || { echo "missing ffmpeg" >&2; exit 1; }
if "${FFMPEG_BIN}" -buildconf 2>/dev/null | grep -q -- '--enable-gpl'; then
  echo "refusing to package a GPL FFmpeg build" >&2
  exit 1
fi
mkdir -p "${OUT}"
# The zip is created from inside the staging directory, so make --out absolute.
OUT="$(cd "${OUT}" && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
APP="${STAGE}/opt/uvnvr"
mkdir -p "${APP}/bin" "${APP}/lib" "${APP}/share/doc/uvnvr" "${STAGE}/Library/LaunchDaemons"
log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
run_bounded() {
  local seconds="$1"
  shift
  perl -e 'alarm shift; exec @ARGV' "${seconds}" "$@"
}
log "installing build"
cmake --install "${BUILD_DIR}" --prefix "${APP}" >/dev/null
install -m 0755 "${FFMPEG_BIN}" "${APP}/bin/ffmpeg"
cp "${ROOT}/LICENSE" "${ROOT}/THIRD_PARTY_NOTICES.md" "${APP}/share/doc/uvnvr/"
cp "${ROOT}/resources/branding/uvnvr-256.png" "${APP}/share/doc/uvnvr/uvnvr.png"
cp -a "${ROOT}/frontend/dist/." "${APP}/share/uvnvr/frontend/dist/" 2>/dev/null || true

# dylibbundler cannot resolve @rpath dependencies on its own; without a search
# path it prompts on stdin (and loops forever in CI). Collect the build tree's
# LC_RPATH entries plus the caller's hints and pass them as -s.
SEARCH_ARGS=()
if command -v otool >/dev/null 2>&1; then
  while IFS= read -r rpath; do
    [[ -n "${rpath}" && -d "${rpath}" ]] && SEARCH_ARGS+=("-s" "${rpath}")
  done < <(otool -l "${BUILD_DIR}/uvnvr" | awk '/LC_RPATH/{getline; getline; print $2}')
fi
for rpath in "${SEARCH_PATHS[@]+"${SEARCH_PATHS[@]}"}"; do
  [[ -d "${rpath}" ]] && SEARCH_ARGS+=("-s" "${rpath}")
done
for prefix in /usr/local /opt/homebrew; do
  [[ -d "${prefix}/lib" ]] && SEARCH_ARGS+=("-s" "${prefix}/lib")
done

log "bundling dylibs"
if command -v dylibbundler >/dev/null 2>&1; then
  run_bounded 300 dylibbundler -od -b -x "${APP}/bin/uvnvr" -d "${APP}/lib" \
    -p "@loader_path/../lib" "${SEARCH_ARGS[@]+"${SEARCH_ARGS[@]}"}" </dev/null
else
  echo "dylibbundler is required" >&2
  exit 1
fi
log "staging launchd daemon"
install -m 0644 "${ROOT}/packaging/macos/com.antruly.uvnvr.plist" "${STAGE}/Library/LaunchDaemons/com.antruly.uvnvr.plist"
chmod +x "${ROOT}/packaging/macos/scripts/postinstall"
log "creating zip"
(cd "${STAGE}" && run_bounded 300 zip -qry "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.zip" opt Library)
log "creating pkg"
run_bounded 300 pkgbuild --root "${STAGE}" --identifier com.antruly.uvnvr \
  --version "${VERSION}" --install-location / \
  --scripts "${ROOT}/packaging/macos/scripts" \
  "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.pkg" >/dev/null
# The dmg is a convenience wrapper around the pkg/zip. hdiutil can be slow or
# unavailable on headless runners, so bound it and never fail the job on it.
if command -v hdiutil >/dev/null 2>&1; then
  log "creating dmg"
  hdiutil create -volname "UvNVR ${VERSION}" -srcfolder "${STAGE}" -ov -format UDZO \
    "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.dmg" >"${STAGE}/dmg.log" 2>&1 &
  dmg_pid=$!
  for _ in $(seq 1 120); do
    if ! kill -0 "${dmg_pid}" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  if kill -0 "${dmg_pid}" 2>/dev/null; then
    log "dmg timed out after 120s; continuing without it"
    kill -9 "${dmg_pid}" 2>/dev/null || true
    sleep 1
    kill -9 "${dmg_pid}" 2>/dev/null || true
    rm -f "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.dmg"
  fi
  wait "${dmg_pid}" 2>/dev/null || true
  if [[ -s "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.dmg" ]]; then
    log "dmg created"
  else
    log "dmg not produced"
  fi
fi
echo "macOS packages written to ${OUT}"
ls -lh "${OUT}"/*macos*
