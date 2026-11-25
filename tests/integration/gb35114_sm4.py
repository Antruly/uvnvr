#!/usr/bin/env python3
"""GB 35114 B-level end-to-end check: VKEK -> SM4 ES encryption -> PS/RTP -> decode.

The script is the acceptance evidence for the B-level video encryption module:

  1. a device SM2 key/certificate and a small H.264 fixture are generated;
  2. the platform wraps a random VKEK for the device certificate;
  3. the device unwraps it, derives the stream keys and seals every H.264
     access unit with SM4-CTR + HMAC-SM3;
  4. the sealed ES is carried through MPEG-PS and RTP (the same layers the
     GB/T 28181 pipeline uses) and must come out byte-identical;
  5. the platform derives the same keys from its stored VKEK, decrypts, and
     ffmpeg must decode the original 20 frames with a non-blank picture;
  6. a wrong VKEK, a wrong stream id and a modified ciphertext must all be
     rejected, and ffmpeg must not decode the encrypted file directly.

MPEG-PS and RTP are implemented here as a small reference transport: the
production PS/RTP stack is the separate GB/T 28181 work item. What matters for
this issue is that the ciphertext is transport-transparent and self-delimiting,
so the standard PS/RTP layers can carry it unchanged.

Usage:
    python3 tests/integration/gb35114_sm4.py ./build/gb35114_cli

Environment overrides: FFMPEG, FFPROBE, OPENSSL.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

STREAM_ID = "34020000001320000001"
OTHER_STREAM_ID = "34020000001320000002"
KEY_ID = "7"
RTP_SSRC = 0x35114025
RTP_PAYLOAD_TYPE = 96

PS_PACK_HEADER = bytes.fromhex("000001ba440004000401000003f8")
SEALED_FRAME_HEADER_SIZE = 56
SEALED_FRAME_MAGIC = b"GBS1"


def run(command: list[str], check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and proc.returncode != 0:
        raise AssertionError(
            "command failed (%d): %s\nstdout:\n%s\nstderr:\n%s"
            % (proc.returncode, " ".join(command), proc.stdout, proc.stderr)
        )
    return proc


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pes_pts_bytes(pts: int) -> bytes:
    return bytes(
        [
            0x21 | ((pts >> 29) & 0x0E),
            (pts >> 22) & 0xFF,
            ((pts >> 14) & 0xFE) | 1,
            (pts >> 7) & 0xFF,
            ((pts << 1) & 0xFE) | 1,
        ]
    )


def mux_ps(frames: list[bytes]) -> bytes:
    """One MPEG-PS pack with one PES packet (stream id 0xE0) per frame."""
    output = bytearray(PS_PACK_HEADER)
    for index, frame in enumerate(frames):
        header = bytes([0x80, 0x80, 0x05]) + pes_pts_bytes(index * 3600)
        length = len(header) + len(frame)
        if length > 0xFFFF:
            raise AssertionError(
                "test transport fixture exceeds one PES packet; split the access unit first"
            )
        output += b"\x00\x00\x01\xe0" + struct.pack(">H", length) + header + frame
    return bytes(output)


def demux_ps(data: bytes) -> list[bytes]:
    frames: list[bytes] = []
    offset = 0
    while offset < len(data):
        if data[offset : offset + 4] == b"\x00\x00\x01\xba":
            offset += 14  # MPEG-2 pack header, marker bits included
        elif data[offset : offset + 4] == b"\x00\x00\x01\xbb":
            length = int.from_bytes(data[offset + 4 : offset + 6], "big")
            offset += 6 + length
        elif data[offset : offset + 3] == b"\x00\x00\x01" and (data[offset + 3] & 0xF0) == 0xE0:
            length = int.from_bytes(data[offset + 4 : offset + 6], "big")
            assert length != 0, "the reference muxer never emits unbounded PES packets"
            payload = offset + 6
            header_length = data[payload + 2]
            frames.append(data[payload + 3 + header_length : offset + 6 + length])
            offset += 6 + length
        else:
            raise AssertionError("unexpected PS byte at offset %d: %s" % (offset, data[offset:offset+8].hex()))
    return frames


def rtp_packetize(payload: bytes, mtu: int = 1400) -> list[bytes]:
    packets: list[bytes] = []
    sequence = 0
    for offset in range(0, len(payload), mtu):
        chunk = payload[offset : offset + mtu]
        marker = 1 if offset + len(chunk) >= len(payload) else 0
        header = struct.pack(
            ">BBHII",
            0x80,
            RTP_PAYLOAD_TYPE | (marker << 7),
            sequence & 0xFFFF,
            0,
            RTP_SSRC,
        )
        packets.append(header + chunk)
        sequence += 1
    return packets


def rtp_depacketize(packets: list[bytes]) -> bytes:
    output = bytearray()
    expected_sequence = None
    for packet in packets:
        assert len(packet) >= 12, "short RTP packet"
        version_cc, type_marker, sequence, _timestamp, ssrc = struct.unpack(">BBHII", packet[:12])
        assert version_cc >> 6 == 2, "not RTP version 2"
        assert (type_marker & 0x7F) == RTP_PAYLOAD_TYPE, "unexpected RTP payload type"
        assert ssrc == RTP_SSRC, "unexpected RTP SSRC"
        if expected_sequence is not None:
            assert sequence == expected_sequence, "RTP sequence gap"
        expected_sequence = (sequence + 1) & 0xFFFF
        output += packet[12:]
    assert packets, "no RTP packets"
    assert packets[-1][1] & 0x80, "last RTP packet has no marker bit"
    return bytes(output)


def split_sealed_frames(sealed: bytes) -> list[bytes]:
    frames: list[bytes] = []
    offset = 0
    while offset < len(sealed):
        assert len(sealed) - offset >= SEALED_FRAME_HEADER_SIZE, "truncated sealed frame header"
        assert sealed[offset : offset + 4] == SEALED_FRAME_MAGIC, "bad sealed frame magic"
        assert sealed[offset + 4] == 1, "unexpected sealed frame version"
        header_size = int.from_bytes(sealed[offset + 6 : offset + 8], "big")
        assert header_size == SEALED_FRAME_HEADER_SIZE, "unexpected sealed frame header size"
        plain_size = int.from_bytes(sealed[offset + 20 : offset + 28], "big")
        total = header_size + plain_size
        assert offset + total <= len(sealed), "truncated sealed frame"
        frames.append(sealed[offset : offset + total])
        offset += total
    return frames


def require_tool(env_name: str, default: str, description: str) -> str:
    candidate = os.environ.get(env_name) or shutil.which(default)
    if not candidate:
        raise SystemExit("error: %s (%s) not found; set %s" % (description, default, env_name))
    return candidate


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python3 tests/integration/gb35114_sm4.py <path-to-gb35114_cli>", file=sys.stderr)
        return 2

    cli = str(Path(sys.argv[1]).resolve())
    if not os.access(cli, os.X_OK):
        print("error: %s is not executable" % cli, file=sys.stderr)
        return 2

    ffmpeg = require_tool("FFMPEG", "ffmpeg", "ffmpeg")
    ffprobe = require_tool("FFPROBE", "ffprobe", "ffprobe")
    openssl = require_tool("OPENSSL", "openssl", "openssl")

    workdir = Path(tempfile.mkdtemp(prefix="uvnvr-gb35114-"))
    print("gb35114_sm4: workdir %s" % workdir)

    device_key = workdir / "device.key.pem"
    device_cert = workdir / "device.crt.pem"
    sample = workdir / "sample.h264"
    envelope = workdir / "vke.kek"
    platform_key = workdir / "platform.key"
    sealed_path = workdir / "sealed.gbes"
    reassembled_path = workdir / "reassembled.gbes"
    decrypted_path = workdir / "decrypted.h264"

    print("[1/6] generate the SM2 device identity and H.264 fixture")
    run([openssl, "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:SM2",
         "-out", str(device_key)])
    run([openssl, "req", "-new", "-x509", "-key", str(device_key), "-sm3",
         "-sigopt", "distid:1234567812345678", "-subj", "/CN=uvnvr-test-device",
         "-days", "30", "-out", str(device_cert)])
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "testsrc2=size=320x240:rate=10", "-t", "2", "-pix_fmt", "yuv420p",
         "-c:v", "libx264", "-profile:v", "baseline", "-g", "10", "-f", "h264",
         "-y", str(sample)])
    sample_hash = sha256(sample)
    print("      fixture: %d bytes sha256=%s" % (sample.stat().st_size, sample_hash))

    print("[2/6] platform wraps a random VKEK for the device certificate")
    wrap = run([cli, "wrap", str(device_cert), STREAM_ID, KEY_ID, str(envelope), str(platform_key)])
    print("      " + wrap.stdout.strip())
    assert envelope.read_bytes()[:4] == b"GVKE", "envelope magic"
    assert platform_key.read_bytes()[:4] == b"GPK1", "platform key magic"
    assert (platform_key.stat().st_mode & 0o077) == 0, "platform key is not private (mode 0600)"

    print("[3/6] device unwraps the VKEK and seals the ES with SM4")
    encrypt = run([cli, "encrypt", str(device_key), str(envelope), STREAM_ID, str(sample),
                   str(sealed_path)])
    print("      " + encrypt.stdout.strip())
    info = run([cli, "info", str(sealed_path)])
    print("      " + info.stdout.strip())
    sealed = sealed_path.read_bytes()
    assert sealed[:4] == SEALED_FRAME_MAGIC, "sealed stream magic"
    assert sealed != sample, "sealed stream equals the plaintext"
    assert b"libx264" not in sealed, "plaintext metadata leaked into the sealed stream"
    sealed_frames = split_sealed_frames(sealed)
    assert len(sealed_frames) >= 2, "expected more than one access unit"
    print("      sealed: %d bytes in %d self-delimiting frames" % (len(sealed), len(sealed_frames)))

    print("[4/6] carry the sealed ES through MPEG-PS and RTP")
    ps_stream = mux_ps(sealed_frames)
    transport_frames = demux_ps(ps_stream)
    assert transport_frames == sealed_frames, "PS round trip changed the sealed frames"
    rtp_packets = rtp_packetize(ps_stream)
    rtp_stream = rtp_depacketize(rtp_packets)
    assert rtp_stream == ps_stream, "RTP round trip changed the PS stream"
    reassembled = b"".join(demux_ps(rtp_stream))
    assert reassembled == sealed, "transport reassembly changed the sealed stream"
    reassembled_path.write_bytes(reassembled)
    print("      ps=%d bytes rtp=%d packets rtp_bytes=%d; PS/RTP round trip identical"
          % (len(ps_stream), len(rtp_packets), len(rtp_stream)))

    # Cross-check the reference PS muxer against ffmpeg: the same pack/PES
    # syntax carrying a real H.264 ES must decode as the original 20 frames.
    plain_ps = workdir / "plain.ps"
    plain_ps.write_bytes(mux_ps([sample.read_bytes()]))
    probe = run([ffprobe, "-hide_banner", "-loglevel", "error", "-count_frames",
                 "-select_streams", "v:0", "-show_entries",
                 "stream=codec_name,width,height,nb_read_frames",
                 "-of", "default=noprint_wrappers=1", str(plain_ps)])
    probe_fields = dict(line.split("=", 1) for line in probe.stdout.strip().splitlines() if "=" in line)
    assert probe_fields.get("codec_name") == "h264", "ffprobe did not see H.264 in the reference PS"
    assert probe_fields.get("nb_read_frames") == "20", "ffprobe decoded %s frames" % probe_fields.get("nb_read_frames")
    print("      reference PS accepted by ffprobe: %s" % probe.stdout.strip().replace("\n", " "))

    print("[5/6] platform decrypts with its stored VKEK and decodes the picture")
    decrypt = run([cli, "decrypt", str(platform_key), STREAM_ID, str(reassembled_path),
                   str(decrypted_path)])
    print("      " + decrypt.stdout.strip())
    decrypted_hash = sha256(decrypted_path)
    assert decrypted_hash == sample_hash, "decrypted ES differs from the original"
    print("      sha256 match: %s" % decrypted_hash)

    png = workdir / "frame.png"
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(decrypted_path),
         "-frames:v", "1", "-y", str(png)])
    assert png.stat().st_size > 1024, "decoded frame is suspiciously small"
    stats_path = workdir / "frame.stats"
    run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(decrypted_path),
         "-vf", "signalstats,metadata=print:file=" + str(stats_path), "-frames:v", "1",
         "-f", "null", "-"])
    stats = stats_path.read_text()
    yavg = next(float(line.split("=", 1)[1]) for line in stats.splitlines()
                if "lavfi.signalstats.YAVG=" in line)
    ymax = next(float(line.split("=", 1)[1]) for line in stats.splitlines()
                if "lavfi.signalstats.YMAX=" in line)
    assert yavg > 10.0 and ymax > 50.0, "decoded frame is blank"
    print("      decoded %s bytes -> frame.png (%d bytes), YAVG=%.3f YMAX=%.0f"
          % ("h264", png.stat().st_size, yavg, ymax))

    print("[6/6] negative paths: wrong VKEK, wrong stream, tampering, direct decode")
    wrong_key = workdir / "wrong.key"
    mutated = bytearray(platform_key.read_bytes())
    mutated[28] ^= 0xFF  # first byte of the VKEK in the GPK1 file
    wrong_key.write_bytes(bytes(mutated))
    wrong_out = workdir / "wrong.h264"
    wrong = run([cli, "decrypt", str(wrong_key), STREAM_ID, str(reassembled_path), str(wrong_out)],
                check=False)
    assert wrong.returncode != 0, "wrong VKEK was accepted"
    assert "authentication failed" in wrong.stderr.lower(), "unexpected wrong-key error: " + wrong.stderr
    assert not wrong_out.exists(), "wrong VKEK produced output"
    print("      wrong VKEK rejected: %s" % wrong.stderr.strip().splitlines()[-1])

    wrong_stream_out = workdir / "wrong-stream.h264"
    wrong_stream = run([cli, "decrypt", str(platform_key), OTHER_STREAM_ID, str(reassembled_path),
                        str(wrong_stream_out)], check=False)
    assert wrong_stream.returncode != 0, "wrong stream id was accepted"
    assert not wrong_stream_out.exists(), "wrong stream id produced output"
    print("      wrong stream id rejected: %s" % wrong_stream.stderr.strip().splitlines()[-1])

    tampered = bytearray(reassembled)
    tampered[-1] ^= 0x01
    tampered_path = workdir / "tampered.gbes"
    tampered_path.write_bytes(bytes(tampered))
    tampered_out = workdir / "tampered.h264"
    tamper = run([cli, "decrypt", str(platform_key), STREAM_ID, str(tampered_path),
                  str(tampered_out)], check=False)
    assert tamper.returncode != 0, "tampered ciphertext was accepted"
    assert "authentication failed" in tamper.stderr.lower(), "unexpected tamper error: " + tamper.stderr
    assert not tampered_out.exists(), "tampered ciphertext produced output"
    print("      modified ciphertext rejected: %s" % tamper.stderr.strip().splitlines()[-1])

    direct_png = workdir / "direct.png"
    direct = run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(sealed_path),
                  "-frames:v", "1", "-y", str(direct_png)], check=False)
    assert direct.returncode != 0, "ffmpeg decoded the encrypted file directly"
    assert not direct_png.exists(), "ffmpeg produced a picture from the encrypted file"
    print("      direct ffmpeg decode rejected: %s" % direct.stderr.strip().splitlines()[0])

    print("gb35114_sm4: PASS")
    print("gb35114_sm4: workdir %s" % workdir)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print("gb35114_sm4: FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
