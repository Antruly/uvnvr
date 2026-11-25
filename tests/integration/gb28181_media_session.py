#!/usr/bin/env python3
"""GB/T 28181 real-time stream lifecycle check (INVITE -> RTP/RTCP -> BYE).

This script plays the platform side of the dialog:

  1. generates an Annex B elementary stream with ffmpeg;
  2. writes an SDP offer for UDP, TCP passive (device listens) or TCP active
     (device connects) media;
  3. launches gb28181_session_cli, which parses the offer, answers it, opens
     the RTP/RTCP sockets and streams PS/90000;
  4. receives RTP, validates payload type, SSRC, sequence, marker and
     timestamp continuity, and reassembles the PS;
  5. receives the device's RTCP Sender Reports, sends Receiver Reports, PLI and
     FIR, and checks that the sender reacts to the key-frame requests;
  6. sends an RTCP BYE and checks that the dialog closes;
  7. decodes the received PS with ffprobe/ffmpeg (the "platform displays a
     picture" evidence).

Example:
  python3 tests/integration/gb28181_media_session.py \
      --cli build/gb28181_session_cli
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


# ---------------------------------------------------------------------------
# Media helpers
# ---------------------------------------------------------------------------


def run(cmd, **kwargs):
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
            "-bf",
            "0",
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


def build_offer(transport: str, port: int) -> str:
    if transport == "udp":
        media = f"m=video {port} RTP/AVP 96\r\n"
        extra = ""
    elif transport == "tcp-passive":
        # The platform initiates the TCP connection; the device answers passive.
        media = f"m=video {port} TCP/RTP/AVP 96\r\n"
        extra = "a=setup:active\r\na=connection:new\r\n"
    elif transport == "tcp-active":
        # The platform listens; the device answers active and connects.
        media = f"m=video {port} TCP/RTP/AVP 96\r\n"
        extra = "a=setup:passive\r\na=connection:new\r\n"
    else:
        raise ValueError(transport)
    return (
        "v=0\r\n"
        "o=34020000002000000001 0 0 IN IP4 127.0.0.1\r\n"
        "s=Play\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        + media
        + "a=recvonly\r\n"
        + "a=rtpmap:96 PS/90000\r\n"
        + "y=0000012345\r\n"
        + extra
    )


def parse_rtp(packet: bytes):
    if len(packet) < 12:
        raise ValueError(f"short RTP packet: {len(packet)} bytes")
    byte0, byte1, sequence, timestamp, ssrc = struct.unpack("!BBHII", packet[:12])
    header = 12 + 4 * (byte0 & 0x0F)
    if byte0 & 0x10:
        words = struct.unpack("!H", packet[header + 2 : header + 4])[0]
        header += 4 + 4 * words
    padding = packet[-1] if byte0 & 0x20 else 0
    payload = packet[header : len(packet) - padding if padding else None]
    return {
        "version": byte0 >> 6,
        "marker": bool(byte1 & 0x80),
        "payload_type": byte1 & 0x7F,
        "sequence": sequence,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload": payload,
    }


def is_rtcp_frame(frame: bytes) -> bool:
    # RTCP payload types are 200..207 (the full byte; the RTP marker bit is
    # not part of this field).
    return len(frame) >= 2 and 200 <= frame[1] <= 207


def rtcp_packet(count: int, payload_type: int, body: bytes) -> bytes:
    size = 4 + len(body)
    if size % 4:
        raise ValueError("RTCP body must be padded to 32 bits")
    return struct.pack("!BBH", 0x80 | count, payload_type, size // 4 - 1) + body


def build_receiver_report(our_ssrc: int, block: dict) -> bytes:
    cumulative = block.get("cumulative_lost", 0) & 0xFFFFFF
    report_block = (
        struct.pack("!I", block.get("ssrc", 0))
        + bytes([block.get("fraction_lost", 0)])
        + cumulative.to_bytes(3, "big")
        + struct.pack(
            "!IIII",
            block.get("extended_highest_sequence", 0),
            block.get("jitter", 0),
            block.get("lsr", 0),
            block.get("dlsr", 0),
        )
    )
    return rtcp_packet(1, 201, struct.pack("!I", our_ssrc) + report_block)


def build_pli(our_ssrc: int, media_ssrc: int) -> bytes:
    return rtcp_packet(1, 206, struct.pack("!II", our_ssrc, media_ssrc))


def build_fir(our_ssrc: int, media_ssrc: int, sequence: int) -> bytes:
    fci = struct.pack("!IB3s", media_ssrc, sequence & 0xFF, b"\x00\x00\x00")
    return rtcp_packet(4, 206, struct.pack("!II", our_ssrc, 0) + fci)


def build_bye(ssrc: int, reason: str = "platform done") -> bytes:
    body = struct.pack("!I", ssrc)
    encoded = reason.encode("utf-8")[:255]
    body += bytes([len(encoded)]) + encoded
    body += b"\x00" * ((4 - len(body) % 4) % 4)
    return rtcp_packet(1, 203, body)


def parse_rtcp_compound(data: bytes):
    packets = []
    offset = 0
    while offset + 4 <= len(data):
        if data[offset : offset + 4] == b"\x00\x00\x00\x00":
            break
        count = data[offset] & 0x1F
        payload_type = data[offset + 1]
        size = (struct.unpack("!H", data[offset + 2 : offset + 4])[0] + 1) * 4
        if offset + size > len(data):
            raise ValueError("truncated RTCP compound packet")
        packet = data[offset : offset + size]
        entry = {"payload_type": payload_type, "count": count, "size": size}
        if payload_type == 200:
            entry.update(
                {
                    "sender_ssrc": struct.unpack("!I", packet[4:8])[0],
                    "ntp_seconds": struct.unpack("!I", packet[8:12])[0],
                    "ntp_fraction": struct.unpack("!I", packet[12:16])[0],
                    "rtp_timestamp": struct.unpack("!I", packet[16:20])[0],
                    "packet_count": struct.unpack("!I", packet[20:24])[0],
                    "octet_count": struct.unpack("!I", packet[24:28])[0],
                }
            )
        elif payload_type == 201:
            entry["sender_ssrc"] = struct.unpack("!I", packet[4:8])[0]
        elif payload_type == 203:
            entry["ssrcs"] = [
                struct.unpack("!I", packet[4 + 4 * i : 8 + 4 * i])[0]
                for i in range(count)
            ]
        elif payload_type == 206:
            entry["format"] = count
            entry["sender_ssrc"] = struct.unpack("!I", packet[4:8])[0]
            entry["media_ssrc"] = struct.unpack("!I", packet[8:12])[0]
        packets.append(entry)
        offset += size
    return packets


# ---------------------------------------------------------------------------
# Process output
# ---------------------------------------------------------------------------


class LineCollector:
    def __init__(self, stream):
        self.lines: list[str] = []
        self.answer_ready = threading.Event()
        self.done = threading.Event()
        self._stream = stream
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        try:
            for line in self._stream:
                line = line.rstrip("\n")
                self.lines.append(line)
                if line.startswith("answer-sdp-end"):
                    self.answer_ready.set()
        finally:
            self.done.set()

    def wait_for_answer(self, timeout: float) -> None:
        if not self.answer_ready.wait(timeout):
            raise AssertionError("sender did not produce an SDP answer:\n" + self.text())

    def text(self) -> str:
        return "\n".join(self.lines)


def parse_answer(lines: list[str]):
    info = {}
    answer_lines = []
    in_answer = False
    for line in lines:
        if line.startswith("answer-sdp-begin"):
            in_answer = True
            continue
        if line.startswith("answer-sdp-end"):
            in_answer = False
            continue
        if in_answer:
            answer_lines.append(line.rstrip("\r"))
            continue
        match = re.match(
            r"mode=(\S+) setup=(\S*) rtp_port=(\d+) rtcp_port=(\d+) ssrc=0x([0-9a-fA-F]+)",
            line,
        )
        if match:
            info = {
                "mode": match.group(1),
                "setup": match.group(2),
                "rtp_port": int(match.group(3)),
                "rtcp_port": int(match.group(4)),
                "ssrc": int(match.group(5), 16),
            }
    info["sdp"] = "\r\n".join(answer_lines) + ("\r\n" if answer_lines else "")
    return info


# ---------------------------------------------------------------------------
# One case
# ---------------------------------------------------------------------------


def receive_udp_case(args, platform_rtp, platform_rtcp, info):
    packets = []
    sr_packets = []
    events = []
    sent_rr = sent_pli = sent_fir = sent_bye = False
    bye_at = None
    keyframe_after_pli = None
    keyframe_after_fir = None
    keyframe_timestamps = []
    pli_waiting = False
    fir_waiting = False
    last_answer_port = info["rtcp_port"]

    platforms = [platform_rtp, platform_rtcp]
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        if bye_at is not None and time.monotonic() - bye_at > 1.0:
            break
        readable, _, _ = select.select(platforms, [], [], 0.2)
        if not readable:
            if args.proc.poll() is not None and packets:
                break
            continue
        for sock in readable:
            try:
                data, _ = sock.recvfrom(65535)
            except OSError:
                continue
            if sock is platform_rtcp:
                try:
                    sr_packets.extend(parse_rtcp_compound(data))
                except ValueError:
                    pass
                continue
            if len(data) < 12:
                continue
            packet = parse_rtp(data)
            packets.append(packet)

            # Receiver report from the last SR we saw.
            if len(packets) == 6 and not sent_rr:
                last_sr = next(
                    (p for p in reversed(sr_packets) if p["payload_type"] == 200), None
                )
                lsr = 0
                if last_sr:
                    lsr = ((last_sr["ntp_seconds"] & 0xFFFF) << 16) | (
                        last_sr["ntp_fraction"] >> 16
                    )
                report = build_receiver_report(
                    0x0BADF00D,
                    {
                        "ssrc": info["ssrc"],
                        "fraction_lost": 2,
                        "cumulative_lost": -1,
                        "extended_highest_sequence": packet["sequence"],
                        "jitter": 12,
                        "lsr": lsr,
                        "dlsr": 0,
                    },
                )
                dlsr_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                dlsr_sock.sendto(report, ("127.0.0.1", last_answer_port))
                dlsr_sock.close()
                sent_rr = True
                events.append("sent RR")

            if len(packets) == 8 and not sent_pli:
                pli = build_pli(0x0BADF00D, info["ssrc"])
                send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                send_sock.sendto(pli, ("127.0.0.1", last_answer_port))
                send_sock.close()
                sent_pli = True
                pli_waiting = True
                events.append("sent PLI")

            new_keyframe = new_keyframe_timestamp(packets, keyframe_timestamps)
            if new_keyframe is not None and pli_waiting:
                keyframe_after_pli = len(packets)
                pli_waiting = False
                events.append(f"keyframe after PLI at packet {len(packets)}")
            if new_keyframe is not None and fir_waiting:
                keyframe_after_fir = len(packets)
                fir_waiting = False
                events.append(f"keyframe after FIR at packet {len(packets)}")

            if keyframe_after_pli is not None and not sent_fir:
                fir = build_fir(0x0BADF00D, info["ssrc"], 7)
                send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                send_sock.sendto(fir, ("127.0.0.1", last_answer_port))
                send_sock.close()
                sent_fir = True
                fir_waiting = True
                events.append("sent FIR")

            if keyframe_after_fir is not None and not sent_bye and len(packets) >= 8:
                bye = build_bye(info["ssrc"])
                send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                send_sock.sendto(bye, ("127.0.0.1", last_answer_port))
                send_sock.close()
                sent_bye = True
                bye_at = time.monotonic()
                events.append("sent RTCP BYE")
        # Keep reading so the final access unit is complete.

    return {
        "packets": packets,
        "sr": [p for p in sr_packets if p["payload_type"] == 200],
        "events": events,
        "sent_rr": sent_rr,
        "sent_pli": sent_pli,
        "sent_fir": sent_fir,
        "sent_bye": sent_bye,
        "keyframe_after_pli": keyframe_after_pli,
        "keyframe_after_fir": keyframe_after_fir,
    }


def new_keyframe_timestamp(packets, seen) -> int | None:
    """Returns the timestamp when a new I-frame PS pack starts, else None."""
    if not packets:
        return None
    timestamp = packets[-1]["timestamp"]
    if seen and seen[-1] == timestamp:
        return None
    group = [p for p in packets if p["timestamp"] == timestamp]
    ps = b"".join(p["payload"] for p in group)
    if b"\x00\x00\x01\xbb" in ps[:64] and b"\x00\x00\x01\xbc" in ps[:96]:
        seen.append(timestamp)
        return timestamp
    return None


def receive_tcp_case(args, media, info):
    packets = []
    sr_packets = []
    events = []
    buffer = bytearray()
    sent_rr = sent_pli = sent_fir = sent_bye = False
    bye_at = None
    keyframe_after_pli = None
    keyframe_after_fir = None
    keyframe_timestamps = []
    pli_waiting = False
    fir_waiting = False
    deadline = time.monotonic() + args.timeout

    def send_rtcp(payload: bytes):
        media.sendall(struct.pack("!H", len(payload)) + payload)

    while time.monotonic() < deadline:
        if bye_at is not None and time.monotonic() - bye_at > 1.0:
            break
        readable, _, _ = select.select([media], [], [], 0.2)
        if not readable:
            if args.proc.poll() is not None and packets:
                break
            continue
        chunk = media.recv(65535)
        if not chunk:
            break
        buffer.extend(chunk)
        while len(buffer) >= 2:
            length = struct.unpack("!H", buffer[:2])[0]
            if len(buffer) < 2 + length:
                break
            frame = bytes(buffer[2 : 2 + length])
            del buffer[: 2 + length]
            if is_rtcp_frame(frame):
                try:
                    sr_packets.extend(parse_rtcp_compound(frame))
                except ValueError:
                    pass
                continue
            packet = parse_rtp(frame)
            packets.append(packet)

            if len(packets) == 6 and not sent_rr:
                last_sr = next(
                    (p for p in reversed(sr_packets) if p["payload_type"] == 200), None
                )
                lsr = 0
                if last_sr:
                    lsr = ((last_sr["ntp_seconds"] & 0xFFFF) << 16) | (
                        last_sr["ntp_fraction"] >> 16
                    )
                send_rtcp(
                    build_receiver_report(
                        0x0BADF00D,
                        {
                            "ssrc": info["ssrc"],
                            "fraction_lost": 2,
                            "cumulative_lost": -1,
                            "extended_highest_sequence": packet["sequence"],
                            "jitter": 12,
                            "lsr": lsr,
                            "dlsr": 0,
                        },
                    )
                )
                sent_rr = True
                events.append("sent RR")

            if len(packets) == 8 and not sent_pli:
                send_rtcp(build_pli(0x0BADF00D, info["ssrc"]))
                sent_pli = True
                pli_waiting = True
                events.append("sent PLI")

            new_keyframe = new_keyframe_timestamp(packets, keyframe_timestamps)
            if new_keyframe is not None and pli_waiting:
                keyframe_after_pli = len(packets)
                pli_waiting = False
                events.append(f"keyframe after PLI at packet {len(packets)}")
            if new_keyframe is not None and fir_waiting:
                keyframe_after_fir = len(packets)
                fir_waiting = False
                events.append(f"keyframe after FIR at packet {len(packets)}")

            if keyframe_after_pli is not None and not sent_fir:
                send_rtcp(build_fir(0x0BADF00D, info["ssrc"], 7))
                sent_fir = True
                fir_waiting = True
                events.append("sent FIR")

            if keyframe_after_fir is not None and not sent_bye and len(packets) >= 8:
                send_rtcp(build_bye(info["ssrc"]))
                sent_bye = True
                bye_at = time.monotonic()
                events.append("sent RTCP BYE")
        # Keep reading so the final access unit is complete.

    return {
        "packets": packets,
        "sr": [p for p in sr_packets if p["payload_type"] == 200],
        "events": events,
        "sent_rr": sent_rr,
        "sent_pli": sent_pli,
        "sent_fir": sent_fir,
        "sent_bye": sent_bye,
        "keyframe_after_pli": keyframe_after_pli,
        "keyframe_after_fir": keyframe_after_fir,
    }


def validate_packets(packets, fps: int, expected_ssrc: int):
    if not packets:
        raise AssertionError("no RTP packets received")
    for index, packet in enumerate(packets):
        if packet["version"] != 2:
            raise AssertionError(f"packet {index}: RTP version {packet['version']}")
        if packet["payload_type"] != 96:
            raise AssertionError(f"packet {index}: PT {packet['payload_type']} != 96")
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
            if current is not None:
                groups.append(current)
            current = {"timestamp": packet["timestamp"], "packets": []}
        current["packets"].append(packet)
    if current is not None:
        groups.append(current)

    for index, group in enumerate(groups):
        markers = [p["marker"] for p in group["packets"]]
        if not markers[-1] or any(markers[:-1]):
            raise AssertionError(
                f"frame {index}: marker is not only on the last packet: ts={group['timestamp']} "
                f"count={len(group['packets'])} markers={markers} "
                f"seqs={[p['sequence'] for p in group['packets']]}"
            )
    deltas = [groups[i + 1]["timestamp"] - groups[i]["timestamp"] for i in range(len(groups) - 1)]
    expected_delta = 90000 // fps
    if any(delta <= 0 for delta in deltas):
        raise AssertionError(f"non-monotonic timestamps: {deltas}")
    if any(delta % expected_delta for delta in deltas):
        raise AssertionError(f"timestamp delta not a multiple of {expected_delta}: {deltas}")
    keyframes = []
    for group in groups:
        ps = b"".join(p["payload"] for p in group["packets"])
        if b"\x00\x00\x01\xbb" in ps[:64] and b"\x00\x00\x01\xbc" in ps[:96]:
            keyframes.append(group["timestamp"])
    if not keyframes:
        raise AssertionError("no I-frame PS pack in the RTP stream")
    return {
        "packets": len(packets),
        "frames": len(groups),
        "keyframes": len(keyframes),
        "keyframe_timestamps": keyframes,
        "first_sequence": packets[0]["sequence"],
        "last_sequence": packets[-1]["sequence"],
        "first_timestamp": packets[0]["timestamp"],
        "last_timestamp": packets[-1]["timestamp"],
        "deltas": deltas,
        "ps": b"".join(p["payload"] for p in packets),
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
    frame_lines = [line for line in decoded.stdout.splitlines() if line and line[0].isdigit()]
    if not frame_lines:
        raise AssertionError("ffmpeg decoded no frames from the received PS")
    return {"streams": streams, "decoded_frames": len(frame_lines)}


# ---------------------------------------------------------------------------
# Case driver
# ---------------------------------------------------------------------------


def run_case(args, codec: str, transport: str, workdir: str) -> None:
    print(f"\n== {codec} over {transport} ==")
    es_path = os.path.join(workdir, f"es.{codec}")
    if not os.path.exists(es_path):
        generate_es(args.ffmpeg, codec, args.fps, args.seconds, es_path)

    offer_path = os.path.join(workdir, f"offer-{codec}-{transport}.sdp")
    base = [
        args.cli,
        "--input",
        es_path,
        "--codec",
        codec,
        "--fps",
        str(args.fps),
        "--offer",
        offer_path,
        "--call-id",
        f"lifecycle-{codec}-{transport}",
        "--device-id",
        "34020000001320000001",
        "--bind",
        "127.0.0.1",
        "--advertise",
        "127.0.0.1",
        "--port-min",
        str(args.port_min),
        "--port-max",
        str(args.port_max),
        "--sender-report-ms",
        "500",
        "--duration",
        str(args.duration),
        "--connect-timeout-ms",
        "5000",
    ]

    platform_rtp = platform_rtcp = None
    tcp_listener = None
    tcp_port = 0
    if transport == "udp":
        while True:
            candidate = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            candidate.bind(("127.0.0.1", 0))
            port = candidate.getsockname()[1]
            if port % 2 == 0 and port < 65535:
                try:
                    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    control.bind(("127.0.0.1", port + 1))
                    platform_rtp, platform_rtcp = candidate, control
                    offer_port = port
                    break
                except OSError:
                    control.close()
            candidate.close()
        with open(offer_path, "w", encoding="utf-8", newline="") as handle:
            handle.write(build_offer(transport, offer_port))
    elif transport == "tcp-passive":
        # Port in the offer is unused by a passive answer; it only has to be
        # nonzero for the SDP parser.
        with open(offer_path, "w", encoding="utf-8", newline="") as handle:
            handle.write(build_offer(transport, 6000))
    else:
        tcp_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tcp_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tcp_listener.bind(("127.0.0.1", 0))
        tcp_listener.listen(1)
        tcp_port = tcp_listener.getsockname()[1]
        with open(offer_path, "w", encoding="utf-8", newline="") as handle:
            handle.write(build_offer(transport, tcp_port))

    print("offer:\n  " + open(offer_path, encoding="utf-8").read().replace("\r\n", "\n  ").rstrip())
    proc = subprocess.Popen(
        base, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    args.proc = proc
    assert proc.stdout is not None
    collector = LineCollector(proc.stdout)
    collector.wait_for_answer(10)
    info = parse_answer(collector.lines)
    if not info:
        proc.kill()
        raise AssertionError("could not parse the answer line:\n" + collector.text())
    print(
        "answer: mode={mode} setup={setup} rtp_port={rtp_port} rtcp_port={rtcp_port} "
        "ssrc=0x{ssrc:08x}".format(**info)
    )

    answer_sdp = info["sdp"]
    media_line = re.search(r"m=video (\d+) (RTP/AVP|TCP/RTP/AVP) 96", answer_sdp)
    if not media_line or int(media_line.group(1)) != info["rtp_port"]:
        raise AssertionError("answer SDP has no matching m=video line:\n" + answer_sdp)
    if "a=rtpmap:96 PS/90000" not in answer_sdp:
        raise AssertionError("answer SDP does not advertise PS/90000:\n" + answer_sdp)
    ssrc_line = re.search(r"y=(\d{10})", answer_sdp)
    if not ssrc_line or int(ssrc_line.group(1)) != info["ssrc"]:
        raise AssertionError("answer SDP y= does not match the SSRC:\n" + answer_sdp)
    if transport == "udp" and "a=setup" in answer_sdp:
        raise AssertionError("UDP answer must not carry a=setup:\n" + answer_sdp)
    if transport == "tcp-passive" and "a=setup:passive" not in answer_sdp:
        raise AssertionError("TCP passive answer missing a=setup:passive:\n" + answer_sdp)
    if transport == "tcp-active" and "a=setup:active" not in answer_sdp:
        raise AssertionError("TCP active answer missing a=setup:active:\n" + answer_sdp)

    media = None
    if transport == "udp":
        result = receive_udp_case(args, platform_rtp, platform_rtcp, info)
        platform_rtp.close()
        platform_rtcp.close()
    else:
        if transport == "tcp-passive":
            media = socket.create_connection(("127.0.0.1", info["rtp_port"]), timeout=5)
        else:
            tcp_listener.settimeout(5)
            media, _ = tcp_listener.accept()
            tcp_listener.close()
        result = receive_tcp_case(args, media, info)
        media.close()

    if result["sent_bye"]:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
    else:
        proc.terminate()
        proc.wait(timeout=5)
    collector.done.wait(timeout=5)
    output = collector.text()
    if proc.returncode not in (0, -15):
        raise AssertionError(f"gb28181_session_cli exited {proc.returncode}:\n{output}")

    stats = validate_packets(result["packets"], args.fps, info["ssrc"])
    if not result["sr"]:
        raise AssertionError("no RTCP Sender Report received")
    if result["sent_rr"] is False:
        raise AssertionError("receiver report was not sent")
    summary_match = re.search(r"summary: (.*)", output)
    if not summary_match:
        raise AssertionError("sender printed no summary:\n" + output)
    summary = summary_match.group(1)
    if "state=closed" not in summary:
        raise AssertionError("dialog did not close after RTCP BYE: " + summary)
    if "rtcp_rr_recv=0" in summary:
        raise AssertionError("device did not process the receiver report: " + summary)
    if args.require_keyframe:
        if "rtcp_pli=0" in summary or "rtcp_fir=0" in summary:
            raise AssertionError("device did not parse PLI/FIR: " + summary)
        if "honored=0" in summary:
            raise AssertionError("device did not honour a key-frame request: " + summary)

    ps_path = os.path.join(workdir, f"received-{codec}-{transport}.ps")
    with open(ps_path, "wb") as handle:
        handle.write(stats["ps"])
    media_info = probe_and_decode(args.ffprobe, args.ffmpeg, ps_path)
    stream = media_info["streams"][0]
    expected_codec = "h264" if codec == "h264" else "hevc"
    if stream.get("codec_name") != expected_codec:
        raise AssertionError(f"ffprobe codec {stream.get('codec_name')} != {expected_codec}")

    sr = result["sr"][-1]
    print(
        "receiver: rtp_packets={packets} frames={frames} keyframes={keyframes} "
        "seq={first_sequence}..{last_sequence} ts={first_timestamp}..{last_timestamp} "
        "deltas={deltas}".format(**stats)
    )
    print(
        "RTCP: sr_sender=0x{sender_ssrc:08x} sr_packets={packet_count} sr_octets={octet_count} "
        "sr_rtp_ts={rtp_timestamp}".format(**sr)
    )
    print("events: " + "; ".join(result["events"]))
    print(
        "keyframes at timestamps: "
        + ", ".join(str(value) for value in stats["keyframe_timestamps"])
    )
    print(f"summary: {summary}")
    print(
        "ffprobe: codec_name={codec_name} width={width} height={height} "
        "avg_frame_rate={avg_frame_rate}".format(**stream)
    )
    print(f"ffmpeg: decoded_frames={media_info['decoded_frames']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", default="build/gb28181_session_cli")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--workdir", default="")
    parser.add_argument("--codec", choices=["h264", "h265", "both"], default="h264")
    parser.add_argument(
        "--transport", choices=["udp", "tcp-passive", "tcp-active", "all"], default="all"
    )
    parser.add_argument("--fps", type=int, default=25)
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--duration", type=int, default=6)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--port-min", type=int, default=34000)
    parser.add_argument("--port-max", type=int, default=34998)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument(
        "--no-keyframe-request",
        dest="require_keyframe",
        action="store_false",
        help="do not require PLI/FIR handling",
    )
    args = parser.parse_args()
    args.proc = None

    if not os.path.isfile(args.cli):
        parser.error(f"--cli does not exist: {args.cli}")
    workdir = args.workdir or tempfile.mkdtemp(prefix="gb28181_media_session_")
    os.makedirs(workdir, exist_ok=True)
    print(f"working directory: {workdir}")

    codecs = ["h264", "h265"] if args.codec == "both" else [args.codec]
    transports = (
        ["udp", "tcp-passive", "tcp-active"] if args.transport == "all" else [args.transport]
    )
    try:
        for codec in codecs:
            for transport in transports:
                run_case(args, codec, transport, workdir)
    except Exception as error:  # noqa: BLE001 - the report is the point
        if args.proc is not None and args.proc.poll() is None:
            args.proc.kill()
        print(f"\nFAILED: {error}", file=sys.stderr)
        return 1
    print("\ngb28181_media_session: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
