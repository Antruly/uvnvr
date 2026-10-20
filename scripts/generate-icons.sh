#!/usr/bin/env bash
# Regenerate every platform icon from resources/branding/uvnvr.svg.
# Requires ImageMagick (`convert`) and Python 3.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/resources/branding/uvnvr.svg"
out="$root/resources/branding"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for size in 16 32 48 64 128 256 512 1024; do
  convert -background none "$src" -resize "${size}x${size}" "$tmp/icon-${size}.png"
done

cp "$tmp/icon-512.png" "$out/uvnvr-512.png"
cp "$tmp/icon-1024.png" "$out/uvnvr-1024.png"
cp "$tmp/icon-256.png" "$out/uvnvr-256.png"

convert "$tmp/icon-256.png" \
  \( "$tmp/icon-128.png" \) \
  \( "$tmp/icon-64.png" \) \
  \( "$tmp/icon-48.png" \) \
  \( "$tmp/icon-32.png" \) \
  \( "$tmp/icon-16.png" \) \
  -define icon:auto-resize=256,128,64,48,32,16 \
  "$out/uvnvr.ico"

# A minimal ICNS writer: PNG-backed elements, no external icns tool required.
python3 - "$tmp" "$out/uvnvr.icns" <<'PY'
import struct
import sys
from pathlib import Path

tmp = Path(sys.argv[1])
out = Path(sys.argv[2])
# (element type, pixel size); these types accept PNG payloads.
elements = [
    (b"icp4", 16),
    (b"icp5", 32),
    (b"icp6", 64),
    (b"ic07", 128),
    (b"ic08", 256),
    (b"ic09", 512),
    (b"ic10", 1024),
]
chunks = []
for kind, size in elements:
    data = (tmp / f"icon-{size}.png").read_bytes()
    chunks.append(kind + struct.pack(">I", len(data) + 8) + data)
payload = b"".join(chunks)
out.write_bytes(b"icns" + struct.pack(">I", len(payload) + 8) + payload)
PY

# Keep the web favicon and PWA icon next to the frontend entry.
mkdir -p "$root/frontend/public"
cp "$src" "$root/frontend/public/favicon.svg"
cp "$out/uvnvr-512.png" "$root/frontend/public/icon-512.png"

echo "icons written to $out and frontend/public"
