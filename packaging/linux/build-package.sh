#!/usr/bin/env bash
# Stage a self-contained Linux package tree and build deb/tar.gz (and rpm when
# rpmbuild is available). Run from the repository root after building.
#
#   packaging/linux/build-package.sh \
#     --build build --libuvcpp /path/to/libuvcpp-1.8.1-linux-x64 \
#     --ffmpeg /path/to/static/ffmpeg --arch x64 --version 0.1.0 --out dist-packages
set -euo pipefail

BUILD_DIR="build"
LIBUVCPP_ROOT=""
FFMPEG_BIN=""
ARCH="x64"
VERSION="0.1.0"
OUT="dist-packages"
STAGE=""

usage() {
  echo "usage: $0 --build DIR --libuvcpp DIR --ffmpeg FILE --arch x64|arm64 --version VER --out DIR" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD_DIR="$2"; shift 2 ;;
    --libuvcpp) LIBUVCPP_ROOT="$2"; shift 2 ;;
    --ffmpeg) FFMPEG_BIN="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

[[ -x "${BUILD_DIR}/uvnvr" ]] || { echo "missing ${BUILD_DIR}/uvnvr" >&2; exit 1; }
[[ -n "${FFMPEG_BIN}" && -x "${FFMPEG_BIN}" ]] || { echo "missing static ffmpeg" >&2; exit 1; }
if "${FFMPEG_BIN}" -buildconf 2>/dev/null | grep -q -- '--enable-gpl'; then
  echo "refusing to package a GPL FFmpeg build" >&2
  exit 1
fi

case "${ARCH}" in
  x64) DEB_ARCH="amd64"; RPM_ARCH="x86_64" ;;
  arm64) DEB_ARCH="arm64"; RPM_ARCH="aarch64" ;;
  *) echo "unsupported arch ${ARCH}" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

APP="${STAGE}/opt/uvnvr"
mkdir -p "${APP}/bin" "${APP}/lib" "${APP}/share/uvnvr/frontend/dist" \
         "${APP}/share/doc/uvnvr" "${STAGE}/lib/systemd/system" \
         "${STAGE}/DEBIAN"

cmake --install "${BUILD_DIR}" --prefix "${APP}" >/dev/null
install -m 0755 "${FFMPEG_BIN}" "${APP}/bin/ffmpeg"
if [[ ! -f "${APP}/share/uvnvr/frontend/dist/index.html" ]]; then
  cp -a "${ROOT}/frontend/dist/." "${APP}/share/uvnvr/frontend/dist/"
fi
install -m 0644 "${ROOT}/LICENSE" "${APP}/share/doc/uvnvr/LICENSE"
install -m 0644 "${ROOT}/THIRD_PARTY_NOTICES.md" "${APP}/share/doc/uvnvr/THIRD_PARTY_NOTICES.md"
install -m 0644 "${ROOT}/resources/branding/uvnvr-256.png" "${APP}/share/doc/uvnvr/uvnvr.png"

# Bundle every non-glibc shared library the binary needs. The application is
# linked with RPATH $ORIGIN/../lib, so the loader finds these copies.
copy_deps() {
  local file="$1"
  ldd "${file}" 2>/dev/null | awk '/=> \// {print $3}' | while read -r lib; do
    [[ -n "${lib}" ]] || continue
    local base
    base="$(basename "${lib}")"
    case "${base}" in
      libc.so.*|libm.so.*|libdl.so.*|librt.so.*|libpthread.so.*|libmvec.so.*|ld-linux*) continue ;;
    esac
    if [[ ! -e "${APP}/lib/${base}" ]]; then
      cp -L "${lib}" "${APP}/lib/${base}"
    fi
  done
}
copy_deps "${BUILD_DIR}/uvnvr"
if [[ -n "${LIBUVCPP_ROOT}" ]]; then
  cp -L "${LIBUVCPP_ROOT}/lib/libuvcpp.so" "${APP}/lib/libuvcpp.so"
fi

install -m 0644 "${ROOT}/packaging/linux/uvnvr.service" "${STAGE}/lib/systemd/system/uvnvr.service"

cat > "${STAGE}/DEBIAN/control" <<EOF
Package: uvnvr
Version: ${VERSION}
Section: video
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: Antruly <noreply@github.com>
Description: UvNVR software network video recorder
 ONVIF/RTSP camera recorder with live preview, MPEG-TS recording,
 playback, RTSP output and a Vue 3 web console.
EOF
install -m 0755 "${ROOT}/packaging/linux/postinst" "${STAGE}/DEBIAN/postinst"
install -m 0755 "${ROOT}/packaging/linux/prerm" "${STAGE}/DEBIAN/prerm"
install -m 0755 "${ROOT}/packaging/linux/postrm" "${STAGE}/DEBIAN/postrm"

dpkg-deb --build --root-owner-group "${STAGE}" "${OUT}/uvnvr_${VERSION}_${DEB_ARCH}.deb" >/dev/null

tar -C "${STAGE}" -czf "${OUT}/uvnvr-${VERSION}-linux-${ARCH}.tar.gz" opt lib

if command -v rpmbuild >/dev/null 2>&1; then
  rpm_top="$(mktemp -d)"
  mkdir -p "${rpm_top}/SOURCES" "${rpm_top}/SPECS" "${rpm_top}/BUILD" \
           "${rpm_top}/RPMS" "${rpm_top}/SRPMS" "${rpm_top}/BUILDROOT"
  tar -C "${STAGE}" -cf "${rpm_top}/SOURCES/uvnvr-tree.tar" opt lib
  cat > "${rpm_top}/SPECS/uvnvr.spec" <<EOF
Name: uvnvr
Version: ${VERSION}
Release: 1
Summary: UvNVR software network video recorder
License: MIT
BuildArch: ${RPM_ARCH}
AutoReqProv: no
Source0: uvnvr-tree.tar
%description
ONVIF/RTSP camera recorder with live preview, MPEG-TS recording, playback,
RTSP output and a Vue 3 web console.
%prep
%build
%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -C %{buildroot} -xf %{SOURCE0}
%pre
getent group uvnvr >/dev/null || groupadd -r uvnvr
getent passwd uvnvr >/dev/null || useradd -r -g uvnvr -d /var/lib/uvnvr -s /sbin/nologin uvnvr
%post
mkdir -p /var/lib/uvnvr
chown -R uvnvr:uvnvr /var/lib/uvnvr
systemctl daemon-reload >/dev/null 2>&1 || true
systemctl enable uvnvr >/dev/null 2>&1 || true
%preun
if [ \$1 -eq 0 ]; then systemctl stop uvnvr >/dev/null 2>&1 || true; systemctl disable uvnvr >/dev/null 2>&1 || true; fi
%postun
systemctl daemon-reload >/dev/null 2>&1 || true
%files
/opt/uvnvr
/lib/systemd/system/uvnvr.service
EOF
  rpmbuild -bb --define "_topdir ${rpm_top}" "${rpm_top}/SPECS/uvnvr.spec" >/dev/null
  cp "${rpm_top}/RPMS/${RPM_ARCH}/"*.rpm "${OUT}/"
  rm -rf "${rpm_top}"
else
  echo "rpmbuild not found; skipped rpm" >&2
fi

echo "packages written to ${OUT}"
ls -lh "${OUT}"
