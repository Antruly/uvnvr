#!/usr/bin/env bash
# Package the UvNVR private-protocol C SDK for one platform.
#   build-sdk-package.sh --build build --out dist-packages --version 0.19.0 \
#                        --platform windows-x64 --uvcpp /path/to/libuvcpp [--zip]
set -euo pipefail

build_dir=build
out_dir=dist-packages
version=0.0.0
platform=linux-x64
uvcpp=""
use_zip=0

while [ $# -gt 0 ]; do
  case "$1" in
    --build) build_dir=$2; shift 2 ;;
    --out) out_dir=$2; shift 2 ;;
    --version) version=$2; shift 2 ;;
    --platform) platform=$2; shift 2 ;;
    --uvcpp) uvcpp=$2; shift 2 ;;
    --zip) use_zip=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$platform" in
  windows-*) use_zip=1 ;;
esac

mkdir -p "$out_dir"
out_abs=$(cd "$out_dir" && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
root="$stage/uvnvr-sdk-$version-$platform"
mkdir -p "$root/include" "$root/lib" "$root/samples" "$root/docs"

cp sdk/include/uvnvr_sdk.h "$root/include/"
if [ -f docs/private-protocol.md ]; then cp docs/private-protocol.md "$root/docs/"; fi
if [ -f docs/configuration.md ]; then cp docs/configuration.md "$root/docs/"; fi

case "$platform" in
  windows-*)
    cp "$build_dir"/libuvnvr_sdk.dll "$root/lib/"
    [ -f "$build_dir"/libuvnvr_sdk.dll.a ] && cp "$build_dir"/libuvnvr_sdk.dll.a "$root/lib/"
    cp "$build_dir"/uvnvr_sample_basic.exe "$build_dir"/uvnvr_sample_live.exe \
       "$build_dir"/uvnvr_sample_download.exe "$root/samples/"
    if [ -n "$uvcpp" ] && [ -f "$uvcpp/bin/libuvcpp.dll" ]; then cp "$uvcpp/bin/libuvcpp.dll" "$root/lib/"; fi
    ;;
  macos-*)
    cp "$build_dir"/libuvnvr_sdk*.dylib "$root/lib/"
    cp "$build_dir"/uvnvr_sample_basic "$build_dir"/uvnvr_sample_live \
       "$build_dir"/uvnvr_sample_download "$root/samples/"
    if [ -n "$uvcpp" ] && [ -d "$uvcpp/lib" ]; then cp "$uvcpp"/lib/libuvcpp*.dylib "$root/lib/" 2>/dev/null || true; fi
    ;;
  *)
    cp -a "$build_dir"/libuvnvr_sdk.so* "$root/lib/"
    cp "$build_dir"/uvnvr_sample_basic "$build_dir"/uvnvr_sample_live \
       "$build_dir"/uvnvr_sample_download "$root/samples/"
    if [ -n "$uvcpp" ] && [ -d "$uvcpp/lib" ]; then cp -a "$uvcpp"/lib/libuvcpp.so* "$root/lib/" 2>/dev/null || true; fi
    ;;
esac

cat > "$root/README.txt" <<EOF
UvNVR private-protocol SDK ${version} (${platform})

include/uvnvr_sdk.h   - the only public header, plain C
lib/                  - shared library (import library on Windows)
samples/              - sample_basic / sample_live / sample_download
docs/                 - protocol and configuration reference

Default endpoint: <nvr>:1515, TCP and UDP/QUIC on the same port. The client
probes QUIC first and falls back to TCP; applications only see uvnvr_connect().
On Windows keep libuvcpp.dll next to the executables (or on PATH).
EOF

if [ "$use_zip" = "1" ]; then
  archive="$out_abs/uvnvr-sdk-$version-$platform.zip"
  rm -f "$archive"
  (cd "$stage" && zip -qr "$archive" "$(basename "$root")")
else
  archive="$out_abs/uvnvr-sdk-$version-$platform.tar.gz"
  tar -C "$stage" -czf "$archive" "$(basename "$root")"
fi
echo "packaged $archive"
ls -l "$archive"