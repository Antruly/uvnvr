#!/usr/bin/env python3
"""End-to-end check for the GB/T 28181 PS/RTP path.

For each codec and transport the script:

  1. generates a short Annex B H.264/H.265 elementary stream with ffmpeg;
  2. runs build/gb28181_ps_cli to mux it to PS and send it as RTP (UDP or TCP
     passive);
  3. receives the RTP packets, validates the header fields, sequence numbers,
     timestamps, marker bits and key-frame marker placement;
  4. concatenates the RTP payloads back into PS and compares it byte-for-byte
     with the sender's --ps-out file;
  5. asks ffprobe/ffmpeg to demux and decode the received PS.

FFmpeg has no PS/90000 RTP depacketizer (its RTP payload table has MP2T, H264,
H265, ... but not PS), so the small receiver below performs the RTP
de-encapsulation and hands the PS bytes to ffmpeg's mpegps demuxer. The PS
itself is decoded by ffmpeg, which is what verifies the encapsulation.

Example:
  python3 tests/integration/gb28181_ps_rtp.py --cli build/gb28181_ps_cli
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kwargs)


def generate_es(ffmpeg: str, codec: str, fps: int, seconds: float, path: str) -> None:
    common = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=size=320x240:rate={fps}",
        "-t",
        str(seconds),
        "-pix_fmt",
        "yuv420p",
    ]
    if codec == "h264":
        cmd = common + [
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-x264-params",
            f"keyint={fps}:scenecut=0:aud=1",
            "-f",
            "h264",
            path,
        ]
    else:
        cmd = common + [
            "-c:v",
            "libx265",
            "-preset",
            "ultrafast",
            "-x265-params",
            f"keyint={fps}:scenecut=0:aud=1:bframes=0:log-level=error",
            "-f",
            "hevc",
            path,
        ]
    run(cmd)


def read_exact(sock: socket.socket, count: int) -> bytes:
    chunks = []
    remaining = count
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def parse_rtp(packet: bytes):
    if len(packet) < 12:
        raise ValueError(f"short RTP packet: {len(packet)} bytes")
    byte0, byte1, sequence, timestamp, ssrc = struct.unpack("!BBHII", packet[:12])
    version = byte0 >> 6
    padding = bool(byte0 & 0x20)
    extension = bool(byte0 & 0x10)
    csrc = byte0 & 0x0F
    header = 12 + 4 * csrc
    if extension:
        if len(packet) < header + 4:
            raise ValueError("truncated RTP extension")
        words = struct.unpack("!H", packet[header + 2 : header + 4])[0]
        header += 4 + 4 * words
    padding_size = packet[-1] if padding else 0
    payload = packet[header : len(packet) - padding_size if padding_size else None]
    return {
        "version": version,
        "marker": bool(byte1 & 0x80),
        "payload_type": byte1 & 0x7F,
        "sequence": sequence,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload": payload,
    }


def finalize_group(groups, current):
    if current is None:
        return
    ps = b"".join(p["payload"] for p in current["packets"])
    markers = [p["marker"] for p in current["packets"]]
    current["ps"] = ps
    current["marker_ok"] = markers[-1] and not any(markers[:-1])
    # A key-frame PS pack starts with the pack header and repeats the system
    # header plus PSM; delta frames only have the pack header and PES.
    current["keyframe"] = (
        b"\x00\x00\x01\xbb" in ps[:64] and b"\x00\x00\x01\xbc" in ps[:96]
    )
    groups.append(current)


def receive_udp(sock: socket.socket, proc: subprocess.Popen, timeout: float):
    packets = []
    sock.settimeout(0.5)
    deadline = time.monotonic() + timeout
    idle_after_exit = 0.0
    last_packet = time.monotonic()
    while time.monotonic() < deadline:
        try:
            data, peer = sock.recvfrom(65535)
        except socket.timeout:
            if proc.poll() is not None:
                idle_after_exit = time.monotonic() - last_packet
                if idle_after_exit >= 0.5:
                    break
            continue
        last_packet = time.monotonic()
        packets.append(parse_rtp(data))
    return packets


def receive_tcp(sock: socket.socket, timeout: float):
    packets = []
    buffer = bytearray()
    sock.settimeout(timeout)
    while True:
        chunk = sock.recv(65535)
        if not chunk:
            break
        buffer.extend(chunk)
        while len(buffer) >= 2:
            length = struct.unpack("!H", buffer[:2])[0]
            if len(buffer) < 2 + length:
                break
            packet = bytes(buffer[2 : 2 + length])
            del buffer[: 2 + length]
            packets.append(parse_rtp(packet))
    if buffer:
        raise ValueError(f"trailing {len(buffer)} bytes in TCP media stream")
    return packets


def validate_rtp(packets, fps: int, expected_ssrc: int):
    if not packets:
        raise AssertionError("no RTP packets received")

    for index, packet in enumerate(packets):
        if packet["version"] != 2:
            raise AssertionError(f"packet {index}: RTP version {packet['version']}")
        if packet["payload_type"] != 96:
            raise AssertionError(
                f"packet {index}: payload type {packet['payload_type']} != 96"
            )
        if packet["ssrc"] != expected_ssrc:
            raise AssertionError(
                f"packet {index}: SSRC 0x{packet['ssrc']:08x} != 0x{expected_ssrc:08x}"
            )
        if index:
            expected = (packets[index - 1]["sequence"] + 1) & 0xFFFF
            if packet["sequence"] != expected:
                raise AssertionError(
                    f"packet {index}: sequence {packet['sequence']} != {expected}"
                )

    groups = []
    current = None
    for packet in packets:
        if current is None or packet["timestamp"] != current["timestamp"]:
            finalize_group(groups, current)
            current = {"timestamp": packet["timestamp"], "packets": []}
        current["packets"].append(packet)
    finalize_group(groups, current)

    for index, group in enumerate(groups):
        if not group["marker_ok"]:
            raise AssertionError(
                f"frame {index}: marker not set only on the last packet"
            )

    deltas = [
        groups[i + 1]["timestamp"] - groups[i]["timestamp"]
        for i in range(len(groups) - 1)
    ]
    expected_delta = 90000 // fps
    for index, delta in enumerate(deltas):
        if delta != expected_delta:
            raise AssertionError(
                f"frame {index}: timestamp delta {delta} != {expected_delta}"
            )

    keyframes = [group for group in groups if group["keyframe"]]
    if not keyframes:
        raise AssertionError("no key-frame PS packs found in the RTP stream")
    if not keyframes[0]["marker_ok"]:
        raise AssertionError("I-frame group does not end with marker=1")

    ps = b"".join(group["ps"] for group in groups)
    return {
        "packets": len(packets),
        "frames": len(groups),
        "keyframes": len(keyframes),
        "keyframe_timestamps": [group["timestamp"] for group in keyframes],
        "marker_count": sum(1 for group in groups if group["marker_ok"]),
        "first_sequence": packets[0]["sequence"],
        "last_sequence": packets[-1]["sequence"],
        "first_timestamp": packets[0]["timestamp"],
        "last_timestamp": packets[-1]["timestamp"],
        "delta": expected_delta,
        "ps": ps,
    }


def probe_and_decode(ffprobe: str, ffmpeg: str, ps_path: str):
    probe = run(
        [
            ffprobe,
            "-hide_banner",
            "-v",
            "error",
            "-show_entries",
            "stream=codec_name,width,height,avg_frame_rate",
            "-of",
            "json",
            ps_path,
        ]
    )
    streams = json.loads(probe.stdout)["streams"]
    decoded = run(
        [
            ffmpeg,
            "-hide_banner",
            "-v",
            "error",
            "-i",
            ps_path,
            "-f",
            "framemd5",
            "-",
        ]
    )
    frame_lines = [
        line for line in decoded.stdout.splitlines() if line and line[0].isdigit()
    ]
    if not frame_lines:
        raise AssertionError("ffmpeg decoded no frames from the PS stream")
    first = frame_lines[0].split(",")
    return {
        "streams": streams,
        "decoded_frames": len(frame_lines),
        "first_frame_md5": first[-1].strip(),
    }


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_case(args, codec: str, transport: str, workdir: str) -> None:
    print(f"\n== {codec} over {transport} ==")
    es_path = os.path.join(workdir, f"es.{codec}")
    generate_es(args.ffmpeg, codec, args.fps, args.seconds, es_path)
    ps_out = os.path.join(workdir, f"sent-{codec}-{transport}.ps")
    sdp_out = os.path.join(workdir, f"{codec}-{transport}.sdp")

    base = [
        args.cli,
        "--input",
        es_path,
        "--codec",
        codec,
        "--fps",
        str(args.fps),
        "--ssrc",
        "305419896",
        "--ps-out",
        ps_out,
        "--sdp-out",
        sdp_out,
        "--local",
        "127.0.0.1",
        "--advertise-address",
        "127.0.0.1",
        "--origin-id",
        "34020000001320000001",
    ]

    if transport == "udp":
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        port = receiver.getsockname()[1]
        command = base + [
            "--transport",
            "udp",
            "--remote",
            "127.0.0.1",
            "--port",
            str(port),
        ]
        print("receiver: udp 127.0.0.1:%d" % port)
        proc = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        packets = receive_udp(receiver, proc, timeout=30)
        output, _ = proc.communicate(timeout=30)
        receiver.close()
    else:
        command = base + [
            "--transport",
            "tcp-passive",
            "--local-port",
            "0",
            "--accept-timeout-ms",
            "10000",
        ]
        proc = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        prelude = []
        port = None
        assert proc.stdout is not None
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            prelude.append(line)
            match = re.search(r"listening on .*:([0-9]+)", line)
            if match:
                port = int(match.group(1))
                break
        if port is None:
            proc.kill()
            raise AssertionError(
                "sender did not report a TCP passive port:\n" + "".join(prelude)
            )
        print("sender listening: tcp 127.0.0.1:%d" % port)
        receiver = socket.create_connection(("127.0.0.1", port), timeout=10)
        packets = receive_tcp(receiver, timeout=10)
        receiver.close()
        remaining, _ = proc.communicate(timeout=30)
        output = "".join(prelude) + remaining

    if proc.returncode != 0:
        raise AssertionError(f"gb28181_ps_cli failed:\n{output}")

    stats = validate_rtp(packets, args.fps, 0x12345678)
    received_ps = os.path.join(workdir, f"received-{codec}-{transport}.ps")
    with open(received_ps, "wb") as handle:
        handle.write(stats["ps"])
    received_hash = sha256(received_ps)
    sent_hash = sha256(ps_out)
    if received_hash != sent_hash:
        raise AssertionError(
            f"RTP payload does not reconstruct the PS: {received_hash} != {sent_hash}"
        )

    media = probe_and_decode(args.ffprobe, args.ffmpeg, received_ps)
    stream = media["streams"][0]
    expected_codec = "h264" if codec == "h264" else "hevc"
    if stream.get("codec_name") != expected_codec:
        raise AssertionError(f"ffprobe codec {stream.get('codec_name')} != {expected_codec}")
    if stream.get("width") != 320 or stream.get("height") != 240:
        raise AssertionError(f"unexpected dimensions: {stream}")

    summary = re.search(r"summary: (.*)", output)
    print("sender: " + (summary.group(1) if summary else "summary missing"))
    print(
        "receiver: rtp_packets={packets} frames={frames} keyframes={keyframes} "
        "marker_packets={marker_count} seq={first_sequence}..{last_sequence} "
        "ts={first_timestamp}..{last_timestamp} delta={delta}".format(**stats)
    )
    print(
        "I-frame timestamps with marker=1: "
        + ", ".join(str(value) for value in stats["keyframe_timestamps"])
    )
    print(f"PS: sent_sha256={sent_hash} received_sha256={received_hash} match=yes")
    print(
        "ffprobe: codec_name={codec_name} width={width} height={height} "
        "avg_frame_rate={avg_frame_rate}".format(**stream)
    )
    print(
        "ffmpeg: decoded_frames={decoded_frames} first_frame_md5={first_frame_md5}".format(
            **media
        )
    )
    print("SDP:")
    print("  " + open(sdp_out, encoding="utf-8").read().replace("\r\n", "\n  ").rstrip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", default="build/gb28181_ps_cli")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--workdir", default="")
    parser.add_argument("--codec", choices=["h264", "h265", "both"], default="both")
    parser.add_argument(
        "--transport", choices=["udp", "tcp-passive", "both"], default="both"
    )
    parser.add_argument("--fps", type=int, default=25)
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(args.cli):
        parser.error(f"--cli does not exist: {args.cli}")
    workdir = args.workdir or tempfile.mkdtemp(prefix="gb28181_ps_rtp_")
    os.makedirs(workdir, exist_ok=True)
    print(f"working directory: {workdir}")

    codecs = ["h264", "h265"] if args.codec == "both" else [args.codec]
    transports = (
        ["udp", "tcp-passive"] if args.transport == "both" else [args.transport]
    )
    try:
        for codec in codecs:
            for transport in transports:
                run_case(args, codec, transport, workdir)
    except Exception as error:  # noqa: BLE001 - the report is the point
        print(f"\nFAILED: {error}", file=sys.stderr)
        return 1
    if not args.keep:
        print("(samples kept: this run is the evidence)")
    print("\ngb28181_ps_rtp: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
