#!/usr/bin/env bash
# Stage and package a macOS build. dylibbundler must be available.
set -euo pipefail
BUILD_DIR="build"
VERSION="0.1.0"
ARCH="arm64"
OUT="dist-packages"
FFMPEG_BIN=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD_DIR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --ffmpeg) FFMPEG_BIN="$2"; shift 2 ;;
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
mkdir -p "${OUT}" "${OUT}/../.macos-stage"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
APP="${STAGE}/opt/uvnvr"
mkdir -p "${APP}/bin" "${APP}/lib" "${APP}/share/doc/uvnvr" "${STAGE}/Library/LaunchDaemons"
cmake --install "${BUILD_DIR}" --prefix "${APP}" >/dev/null
install -m 0755 "${FFMPEG_BIN}" "${APP}/bin/ffmpeg"
cp "${ROOT}/LICENSE" "${ROOT}/THIRD_PARTY_NOTICES.md" "${APP}/share/doc/uvnvr/"
cp "${ROOT}/resources/branding/uvnvr-256.png" "${APP}/share/doc/uvnvr/uvnvr.png"
cp -a "${ROOT}/frontend/dist/." "${APP}/share/uvnvr/frontend/dist/" 2>/dev/null || true
if command -v dylibbundler >/dev/null 2>&1; then
  dylibbundler -od -b -x "${APP}/bin/uvnvr" -d "${APP}/lib" -p "@loader_path/../lib" >/dev/null
else
  echo "dylibbundler is required" >&2
  exit 1
fi
install -m 0644 "${ROOT}/packaging/macos/com.antruly.uvnvr.plist" "${STAGE}/Library/LaunchDaemons/com.antruly.uvnvr.plist"
chmod +x "${ROOT}/packaging/macos/scripts/postinstall"
pkgbuild --root "${STAGE}" --identifier com.antruly.uvnvr --version "${VERSION}" \
  --install-location / --scripts "${ROOT}/packaging/macos/scripts" \
  "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.pkg" >/dev/null
hdiutil create -volname "UvNVR ${VERSION}" -srcfolder "${STAGE}" -ov -format UDZO \
  "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.dmg" >/dev/null
(cd "${STAGE}" && zip -qry "${OUT}/uvnvr-${VERSION}-macos-${ARCH}.zip" opt Library)
echo "macOS packages written to ${OUT}"
ls -lh "${OUT}"/*macos*
