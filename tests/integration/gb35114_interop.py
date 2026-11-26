#!/usr/bin/env python3
"""GB 35114 A-level -> B-level platform interop run.

The script drives `gb35114_interop` twice: once as a reference 35114 platform
and once as the UvNVR device. The two processes exchange real IPv4/UDP traffic
on loopback:

  1. SIP MESSAGE / ClientHello / ServerHello: mutual SM2 authentication and
     signature-suite negotiation;
  2. SIP MESSAGE / VkekDelivery / VkekAck: an SM2-wrapped VKEK bound to the
     handshake transcript;
  3. RTP payload type 96: the SM4-CTR sealed H.264 elementary stream;
  4. a side channel: the A-level SM3/SM2 frame signatures plus two tampered
     probes (a modified signature and a modified ciphertext).

The platform writes a libpcap file of every datagram it sends and receives.
This script parses that capture and asserts the four acceptance items:
identity, verifiable signatures, decryptable media and rejected tampering.

Usage:
    python3 tests/integration/gb35114_interop.py ./build/gb35114_interop
        [--evidence-dir DIR]

Without --evidence-dir the run happens in a temporary directory and prints its
path. With it, the capture, the two process logs and summary.txt are kept.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

STREAM_ID = "34020000001320000001"
PCAP_GLOBAL_HEADER = struct.Struct("<IHHiIII")
PCAP_PACKET_HEADER = struct.Struct("<IIII")
SIP_PREFIX = b"MESSAGE "


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_executable(path: str, label: str) -> str:
    resolved = str(Path(path).resolve())
    if not os.access(resolved, os.X_OK):
        raise SystemExit("error: %s %s is not executable" % (label, resolved))
    return resolved


def require_tool(env_name: str, default: str) -> str:
    value = os.environ.get(env_name, default)
    if shutil.which(value) is None:
        raise SystemExit("error: %s not found; set %s" % (value, env_name))
    return value


def read_udp_payload(packet: bytes) -> bytes | None:
    if len(packet) < 14 + 20 + 8:
        return None
    ether_type = struct.unpack_from(">H", packet, 12)[0]
    if ether_type != 0x0800:
        return None
    ip = packet[14:]
    version_ihl = ip[0]
    if version_ihl >> 4 != 4:
        return None
    ip_header_length = (version_ihl & 0x0F) * 4
    if ip[9] != 17:
        return None
    total_length = struct.unpack_from(">H", ip, 2)[0]
    udp = ip[ip_header_length : total_length]
    if len(udp) < 8:
        return None
    return udp[8:]


def parse_capture(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < PCAP_GLOBAL_HEADER.size:
        raise AssertionError("pcap is truncated")
    magic, _major, _minor, _zone, _sigfigs, _snap, network = PCAP_GLOBAL_HEADER.unpack_from(data, 0)
    if magic not in (0xA1B2C3D4, 0xD4C3B2A1):
        raise AssertionError("not a libpcap file: magic=0x%08x" % magic)
    if network != 1:
        raise AssertionError("expected Ethernet link type, got %d" % network)

    sip_messages: dict[str, int] = {}
    rtp_packets = 0
    rtp_bytes = 0
    sig_messages: dict[int, int] = {}
    other = 0
    offset = PCAP_GLOBAL_HEADER.size
    packets = 0
    while offset + PCAP_PACKET_HEADER.size <= len(data):
        _sec, _usec, incl, _orig = PCAP_PACKET_HEADER.unpack_from(data, offset)
        offset += PCAP_PACKET_HEADER.size
        packet = data[offset : offset + incl]
        offset += incl
        packets += 1
        payload = read_udp_payload(packet)
        if payload is None:
            other += 1
            continue
        if payload.startswith(SIP_PREFIX):
            header_end = payload.find(b"\r\n\r\n")
            headers = payload[:header_end].decode("ascii", "replace") if header_end >= 0 else ""
            message_type = ""
            for line in headers.split("\r\n"):
                if line.lower().startswith("x-gb35114-message:"):
                    message_type = line.split(":", 1)[1].strip()
                    break
            sip_messages[message_type or "unknown"] = sip_messages.get(message_type or "unknown", 0) + 1
        elif payload.startswith(b"GSIG"):
            if len(payload) >= 6:
                kind = payload[4]
                sig_messages[kind] = sig_messages.get(kind, 0) + 1
        elif payload and (payload[0] >> 6) == 2:
            rtp_packets += 1
            rtp_bytes += len(payload) - 12 if len(payload) >= 12 else 0
        else:
            other += 1
    return {
        "packets": packets,
        "sip_messages": sip_messages,
        "rtp_packets": rtp_packets,
        "rtp_payload_bytes": rtp_bytes,
        "side_channel": sig_messages,
        "other": other,
    }


def run_negative_control(binary: str, source: Path, workdir: Path, sample: Path,
                         stream_id: str, kind: str) -> dict[str, object]:
    """Runs one impersonation scenario and requires the receiver to fail closed.

    forged-device:   the platform pins the wrong device key, so a correctly
                     signed ClientHello from the real device must be rejected.
    forged-platform: the device pins the wrong platform key, so a correctly
                     signed ServerHello from the real platform must be rejected.
    """
    directory = workdir / ("negative-" + kind)
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    for name in ("device_private_key.pem", "device_public_key.pem",
                 "platform_private_key.pem", "platform_public_key.pem"):
        shutil.copyfile(source / name, directory / name)
    if kind == "forged-device":
        shutil.copyfile(directory / "platform_public_key.pem",
                        directory / "device_public_key.pem")
    elif kind == "forged-platform":
        shutil.copyfile(directory / "device_public_key.pem",
                        directory / "platform_public_key.pem")
    else:
        raise AssertionError("unknown negative control: %s" % kind)

    pcap = directory / "negative.pcap"
    platform = subprocess.Popen(
        [binary, "platform", "--dir", str(directory), "--pcap", str(pcap),
         "--stream-id", stream_id, "--timeout-ms", "1500"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert platform.stdout is not None
    ready_line = ""
    deadline = time.time() + 10
    while time.time() < deadline:
        line = platform.stdout.readline()
        if not line:
            break
        ready_line = line
        if "platform ready" in line:
            break
    if "platform ready" not in ready_line:
        platform.kill()
        raise AssertionError("%s: platform did not become ready: %r" % (kind, ready_line))
    fields = dict(part.split("=", 1) for part in ready_line.split() if "=" in part)
    device = subprocess.run(
        [
            binary, "device", "--dir", str(directory),
            "--negotiate-port", fields["negotiate"],
            "--media-port", fields["media"],
            "--signature-port", fields["signature"],
            "--h264", str(sample),
            "--stream-id", stream_id,
            "--timeout-ms", "1500",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=20,
    )
    remaining, _ = platform.communicate(timeout=20)
    platform_output = ready_line + (remaining or "")
    (directory / "platform.log").write_text(platform_output)
    (directory / "device.log").write_text(device.stdout)

    if kind == "forged-device":
        if platform.returncode == 0 or "identity=rejected" not in platform_output:
            raise AssertionError(
                "forged device identity was not rejected:\n%s" % platform_output)
        if device.returncode == 0:
            raise AssertionError(
                "forged device identity run unexpectedly succeeded:\n%s" % device.stdout)
    else:
        if device.returncode == 0 or "ServerHello bad-signature" not in device.stdout:
            raise AssertionError(
                "forged platform identity was not rejected:\n%s" % device.stdout)
        if platform.returncode == 0:
            raise AssertionError(
                "forged platform identity run unexpectedly succeeded:\n%s" % platform_output)
    return {
        "kind": kind,
        "verdict": "rejected",
        "device_exit": device.returncode,
        "platform_exit": platform.returncode,
    }


def run_checked(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs)
    if proc.returncode != 0:
        raise AssertionError(
            "command failed (%d): %s\nstdout:\n%s\nstderr:\n%s"
            % (proc.returncode, " ".join(command), proc.stdout, proc.stderr)
        )
    return proc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to gb35114_interop")
    parser.add_argument("--evidence-dir", help="keep artifacts in this directory")
    parser.add_argument("--stream-id", default=STREAM_ID)
    parser.add_argument("--skip-negative-controls", action="store_true",
                        help="skip the two forged-identity runs")
    args = parser.parse_args()

    binary = require_executable(args.binary, "gb35114_interop")
    ffmpeg = require_tool("FFMPEG", "ffmpeg")

    if args.evidence_dir:
        workdir = Path(args.evidence_dir).resolve()
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        workdir = Path(tempfile.mkdtemp(prefix="uvnvr-gb35114-interop-"))
    print("gb35114_interop: workdir %s" % workdir)

    pcap = workdir / "gb35114_interop.pcap"
    sample = workdir / "sample.h264"

    print("[1/7] generate the SM2 identities and an H.264 fixture")
    run_checked([binary, "keygen", "--dir", str(workdir)])
    run_checked(
        [
            ffmpeg, "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=10", "-t", "2",
            "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline",
            "-g", "10", "-f", "h264", "-y", str(sample),
        ]
    )
    print("      fixture: %d bytes sha256=%s" % (sample.stat().st_size, sha256(sample)))

    print("[2/7] start the reference platform")
    platform_log = workdir / "platform.log"
    platform = subprocess.Popen(
        [binary, "platform", "--dir", str(workdir), "--pcap", str(pcap),
         "--stream-id", args.stream_id],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert platform.stdout is not None
    ready_line = ""
    deadline = time.time() + 10
    while time.time() < deadline:
        line = platform.stdout.readline()
        if not line:
            break
        ready_line = line
        if "platform ready" in line:
            break
    if "platform ready" not in ready_line:
        platform.kill()
        raise AssertionError("platform did not become ready: %r" % ready_line)
    fields = dict(part.split("=", 1) for part in ready_line.split() if "=" in part)
    negotiate_port = int(fields["negotiate"])
    media_port = int(fields["media"])
    signature_port = int(fields["signature"])
    print("      %s" % ready_line.strip())

    print("[3/7] run the device handshake, media and tamper probes")
    device = subprocess.run(
        [
            binary, "device", "--dir", str(workdir),
            "--negotiate-port", str(negotiate_port),
            "--media-port", str(media_port),
            "--signature-port", str(signature_port),
            "--h264", str(sample),
            "--stream-id", args.stream_id,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    print("[4/7] collect the platform verdict")
    remaining, _ = platform.communicate(timeout=30)
    platform_output = ready_line + (remaining or "")
    platform_log.write_text(platform_output)
    (workdir / "device.log").write_text(device.stdout)
    if device.returncode != 0:
        raise AssertionError("device failed (%d):\n%s" % (device.returncode, device.stdout))
    if platform.returncode != 0:
        raise AssertionError("platform failed (%d):\n%s" % (platform.returncode, platform_output))

    for token in (
        "identity=ok",
        "signature_tamper=rejected",
        "cipher_tamper=rejected",
        "plaintext_match=yes",
        "overall=ok",
    ):
        if token not in device.stdout:
            raise AssertionError("device report is missing %r:\n%s" % (token, device.stdout))
    for token in ("identity=ok", "tampered_signature=rejected",
                  "tampered_ciphertext=rejected", "overall=ok"):
        if token not in platform_output:
            raise AssertionError("platform output is missing %r:\n%s" % (token, platform_output))

    print("[5/7] parse the capture")
    stats = parse_capture(pcap)
    required_sip = {"ClientHello", "ServerHello", "VkekDelivery", "VkekAck", "Report"}
    missing = required_sip - set(stats["sip_messages"])
    if missing:
        raise AssertionError("capture is missing SIP messages: %s" % sorted(missing))
    if stats["rtp_packets"] <= 20:
        raise AssertionError("capture has too few RTP packets: %d" % stats["rtp_packets"])
    side = stats["side_channel"]
    if side.get(1, 0) < 1 or side.get(2, 0) != 1 or side.get(3, 0) != 1 or side.get(0, 0) != 1:
        raise AssertionError("unexpected side-channel records: %s" % side)

    negative_controls = []
    if args.skip_negative_controls:
        print("[6/7] negative identity controls skipped")
    else:
        print("[6/7] negative identity controls (forged device / forged platform)")
        for kind in ("forged-device", "forged-platform"):
            result = run_negative_control(binary, workdir, workdir, sample,
                                          args.stream_id, kind)
            negative_controls.append(result)
            print("      %s: %s (device exit %d, platform exit %d)"
                  % (kind, result["verdict"], result["device_exit"],
                     result["platform_exit"]))

    print("[7/7] write the record")
    summary = {
        "stream_id": args.stream_id,
        "capture": str(pcap),
        "capture_sha256": sha256(pcap),
        "capture_bytes": pcap.stat().st_size,
        "fixture_sha256": sha256(sample),
        "device_sha256": sha256(workdir / "device_private_key.pem"),
        "platform_sha256": sha256(workdir / "platform_private_key.pem"),
        "capture_stats": stats,
        "device_exit": device.returncode,
        "platform_exit": platform.returncode,
        "negative_controls": negative_controls,
    }
    summary_path = workdir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    report = []
    report.append("GB 35114 A-level -> B-level interop record")
    report.append("stream id: %s" % args.stream_id)
    report.append("capture: %s (%d bytes, sha256=%s)" % (pcap.name, pcap.stat().st_size, summary["capture_sha256"]))
    report.append("packets: %d, RTP: %d (%d payload bytes), other: %d"
                  % (stats["packets"], stats["rtp_packets"], stats["rtp_payload_bytes"], stats["other"]))
    report.append("SIP MESSAGE: " + ", ".join("%s=%d" % item for item in sorted(stats["sip_messages"].items())))
    report.append("side channel: type0(end)=%d type1(signature)=%d type2(tampered signature)=%d "
                  "type3(tampered ciphertext)=%d"
                  % (side.get(0, 0), side.get(1, 0), side.get(2, 0), side.get(3, 0)))
    labels = {"forged-device": "negative identity (forged device key)",
              "forged-platform": "negative identity (forged platform key)"}
    for result in negative_controls:
        report.append("%s = %s" % (labels[result["kind"]], result["verdict"]))
    report.append("")
    report.append("device output:")
    report.extend(device.stdout.rstrip().splitlines())
    report.append("")
    report.append("platform output:")
    report.extend(platform_output.rstrip().splitlines())
    report_text = "\n".join(report) + "\n"
    (workdir / "interop-summary.txt").write_text(report_text)
    print("\n" + report_text, end="")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print("FAIL: %s" % error, file=sys.stderr)
        sys.exit(1)
