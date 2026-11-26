#!/usr/bin/env python3
"""GB/T 28181 PTZ, subscription and AI-alarm check (GH-20).

The script plays the platform side and the camera side at the same time:

  * a fake ONVIF camera answers GetCapabilities/GetProfiles and records every
    ContinuousMove/Stop/preset SOAP request;
  * a fake SIP platform registers the device, sends DeviceControl PTZCmd
    messages, subscribes to Event: Alarm, and collects the device's Alarm
    MESSAGE and NOTIFY requests;
  * a synthetic RTSP source feeds the real uvnvr AI pipeline, whose stub plate
    engine is configured to recognise a watchlisted plate. That produces a
    genuine RaiseAlert("ai_plate") on a worker thread, which the new bridge
    forwards to GB/T 28181 as an Alarm.

It starts the real uvnvr binary against a temporary data directory.

Usage:
  tests/integration/gb28181_ptz_alarm.py [--binary build/uvnvr] [--ffmpeg ffmpeg]

Exit status is non-zero on the first failed assertion. All evidence is printed
to stdout so it can be quoted in the PR.
"""

import argparse
import datetime
import http.server
import json
import os
import random
import re
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

DEVICE_ID = "34020000001180000001"
CHANNEL_ID = "34020000001320000001"
CENTER_CODE = "34020000"
SIP_PORT = 15160
DEVICE_SIP_PORT = 15161
ONVIF_PORT = 18181
RTSP_PORT = 18654
HTTP_PORT = 18180
PRIVATE_PORT = 11615
CHANNEL_LOCAL_ID = 1


class CheckFailed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise CheckFailed(message)
    print("OK:", message)


def http_ok(port):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=0.5) as resp:
            return resp.status == 200
    except Exception:
        return False


def wait_health(proc, port, timeout=25.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise CheckFailed(f"uvnvr exited during startup with {proc.returncode}")
        if http_ok(port):
            return
        time.sleep(0.2)
    raise CheckFailed("uvnvr did not become healthy")


def parse_sip(data):
    if b"\r\n\r\n" in data:
        head, body = data.split(b"\r\n\r\n", 1)
    else:
        head, body = data, b""
    text = head.decode("utf-8", "replace")
    lines = text.replace("\r\n", "\n").split("\n")
    first = lines[0] if lines else ""
    headers = []
    for line in lines[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers.append((name.strip(), value.strip()))
    return first, headers, body.decode("utf-8", "replace")


def header(headers, name):
    for key, value in headers:
        if key.lower() == name.lower():
            return value
    return ""


def headers_all(headers, name):
    return [value for key, value in headers if key.lower() == name.lower()]


def sip_response(data, code, reason, extra=None):
    first, headers, _ = parse_sip(data)
    del first
    to = header(headers, "To")
    if ";tag=" not in to:
        to += ";tag=fakeplatform%d" % random.randint(1000, 9999)
    lines = [f"SIP/2.0 {code} {reason}"]
    lines.extend(f"Via: {via}" for via in headers_all(headers, "Via"))
    lines.append(f"From: {header(headers, 'From')}")
    lines.append(f"To: {to}")
    lines.append(f"Call-ID: {header(headers, 'Call-ID')}")
    lines.append(f"CSeq: {header(headers, 'CSeq')}")
    lines.append("User-Agent: uvnvr-gb28181-integration")
    for name, value in extra or []:
        lines.append(f"{name}: {value}")
    lines.append("Content-Length: 0")
    return ("\r\n".join(lines) + "\r\n\r\n").encode()


def sip_request(method, target, call_id, cseq, body="", content_type=None, from_tag="plat",
                extra=None):
    branch = "z9hG4bK%08x" % random.getrandbits(32)
    lines = [
        f"{method} sip:{target} SIP/2.0",
        f"Via: SIP/2.0/UDP 127.0.0.1:{SIP_PORT};branch={branch};rport",
        "Max-Forwards: 70",
        f"From: <sip:platform@127.0.0.1:{SIP_PORT}>;tag={from_tag}",
        f"To: <sip:{target.split('@')[0]}@127.0.0.1>",
        f"Call-ID: {call_id}",
        f"CSeq: {cseq} {method}",
        f"Contact: <sip:platform@127.0.0.1:{SIP_PORT}>",
        "User-Agent: uvnvr-gb28181-integration",
    ]
    for name, value in extra or []:
        lines.append(f"{name}: {value}")
    if content_type:
        lines.append(f"Content-Type: {content_type}")
    payload = body.encode("utf-8")
    lines.append(f"Content-Length: {len(payload)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + payload


def ptz_cmd_hex(action, h_speed=0xFF, v_speed=0x00, zoom_speed=0x00, preset=0):
    codes = {
        "stop": 0x00,
        "right": 0x01,
        "left": 0x02,
        "up": 0x03,
        "down": 0x04,
        "up-right": 0x05,
        "up-left": 0x06,
        "down-right": 0x07,
        "down-left": 0x08,
        "zoom-in": 0x09,
        "zoom-out": 0x0A,
        "set-preset": 0x0F,
        "goto-preset": 0x10,
        "remove-preset": 0x11,
    }
    data = [0xA5, codes[action], h_speed, v_speed, zoom_speed, (preset >> 8) & 0xFF, preset & 0xFF]
    data.append(sum(data) & 0xFF)
    return "".join(f"{value:02X}" for value in data)


def device_control_xml(sn, action, **kwargs):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\r\n'
        "<Control>\r\n"
        "  <CmdType>DeviceControl</CmdType>\r\n"
        f"  <SN>{sn}</SN>\r\n"
        f"  <DeviceID>{CHANNEL_ID}</DeviceID>\r\n"
        f"  <PTZCmd>{ptz_cmd_hex(action, **kwargs)}</PTZCmd>\r\n"
        "  <Info><ControlPriority>1</ControlPriority></Info>\r\n"
        "</Control>\r\n"
    )


# ---------------------------------------------------------------------------
# Fake ONVIF camera
# ---------------------------------------------------------------------------


class OnvifState:
    def __init__(self):
        self.lock = threading.Lock()
        self.bodies = []

    def record(self, body):
        with self.lock:
            self.bodies.append(body)

    def snapshot(self):
        with self.lock:
            return list(self.bodies)


class OnvifHandler(http.server.BaseHTTPRequestHandler):
    state = None

    def log_message(self, *_args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        self.state.record(body)
        if "GetCapabilities" in body:
            media = f"http://127.0.0.1:{ONVIF_PORT}/onvif/media_service"
            ptz = f"http://127.0.0.1:{ONVIF_PORT}/onvif/ptz_service"
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
                'xmlns:tt="http://www.onvif.org/ver10/schema">'
                "<s:Body><tds:GetCapabilitiesResponse><tds:Capabilities>"
                f"<tt:Media><tt:XAddr>{media}</tt:XAddr></tt:Media>"
                f"<tt:PTZ><tt:XAddr>{ptz}</tt:XAddr></tt:PTZ>"
                "</tds:Capabilities></tds:GetCapabilitiesResponse></s:Body></s:Envelope>"
            )
        elif "GetProfiles" in body:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
                'xmlns:tt="http://www.onvif.org/ver10/schema">'
                "<s:Body><trt:GetProfilesResponse>"
                '<trt:Profiles token="profile_main" fixed="true">'
                "<tt:Name>main</tt:Name></trt:Profiles>"
                "</trt:GetProfilesResponse></s:Body></s:Envelope>"
            )
        else:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
                '<s:Body><tptz:Response xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl"/>'
                "</s:Body></s:Envelope>"
            )
        payload = response.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/soap+xml")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def start_onvif_server():
    state = OnvifState()
    handler = type("BoundOnvifHandler", (OnvifHandler,), {"state": state})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", ONVIF_PORT), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, state


# ---------------------------------------------------------------------------
# Fake SIP platform
# ---------------------------------------------------------------------------


class Platform:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", SIP_PORT))
        self.sock.settimeout(0.2)
        self.device_addr = None
        self.registered = threading.Event()
        self.alarm_bodies = []
        self.notify_bodies = []
        self.responses = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.sock.close()

    def send(self, payload):
        check(self.device_addr is not None, "device address is known")
        self.sock.sendto(payload, self.device_addr)

    def loop(self):
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            self.device_addr = addr
            first, headers, body = parse_sip(data)
            if first.startswith("SIP/2.0"):
                with self.lock:
                    self.responses.append((first, header(headers, "CSeq")))
                continue
            method = first.split(" ", 1)[0] if first else ""
            if method == "REGISTER":
                self.sock.sendto(sip_response(data, 200, "OK"), addr)
                self.registered.set()
            elif method == "MESSAGE":
                self.sock.sendto(sip_response(data, 200, "OK"), addr)
                if "<CmdType>Alarm</CmdType>" in body:
                    with self.lock:
                        self.alarm_bodies.append(body)
            elif method == "NOTIFY":
                self.sock.sendto(sip_response(data, 200, "OK"), addr)
                with self.lock:
                    # Keep the headers as well as the body: the subscription
                    # state lives in the former and the alarm XML in the latter.
                    self.notify_bodies.append(data.decode("utf-8", "replace"))
            else:
                self.sock.sendto(sip_response(data, 200, "OK"), addr)

    def wait(self, predicate, timeout=15.0, description="condition"):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            time.sleep(0.1)
        raise CheckFailed(f"timed out waiting for {description}")


# ---------------------------------------------------------------------------
# Minimal RTSP/H.264-over-TCP source
#
# The ffmpeg build used by the project has no RTSP listen muxer (the option is
# demuxer-only), and VLC's server rejects RTP/AVP/TCP. uvnvr's media pipeline
# always asks for TCP transport, so the test ships a small server that speaks
# exactly the subset ffmpeg needs: OPTIONS, DESCRIBE, SETUP (interleaved TCP),
# PLAY and TEARDOWN, and packetises a pre-encoded H.264 file into RTP.
# ---------------------------------------------------------------------------


def generate_h264(ffmpeg, path):
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        "testsrc=size=640x480:rate=10",
        "-t",
        "12",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-g",
        "10",
        "-x264-params",
        "aud=1:bframes=0",
        "-pix_fmt",
        "yuv420p",
        "-f",
        "h264",
        path,
    ]
    subprocess.run(command, check=True)


def split_annex_b(data):
    # Record both the start-code offset (the end of the previous NAL) and the
    # payload offset. Using the next payload offset as the end would fold the
    # next 00 00 01 marker into the previous NAL and corrupt every slice.
    units = []
    index = 0
    while True:
        found = data.find(b"\x00\x00\x01", index)
        if found < 0:
            break
        code_start = found - 1 if found >= 1 and data[found - 1] == 0 else found
        payload = found + 3
        if payload < len(data):
            units.append((code_start, payload))
        index = found + 3
    nals = []
    for position, (_, start) in enumerate(units):
        end = units[position + 1][0] if position + 1 < len(units) else len(data)
        nal = data[start:end]
        while nal.endswith(b"\x00"):
            nal = nal[:-1]
        if nal:
            nals.append(nal)
    return nals


def access_units(nals):
    units = []
    current = []
    for nal in nals:
        nal_type = nal[0] & 0x1F
        if nal_type == 9 and current:
            units.append(current)
            current = []
        current.append(nal)
    if current:
        units.append(current)
    return [unit for unit in units if any((nal[0] & 0x1F) in (1, 5) for nal in unit)]


def rtp_packets(nal, timestamp, marker, sequence, ssrc, mtu=1200):
    def header(seq, final):
        # Second RTP byte: marker in the top bit, payload type 96 in the low 7.
        return (bytes([0x80, (0x80 if final else 0) | 96]) +
                (seq & 0xFFFF).to_bytes(2, "big") +
                timestamp.to_bytes(4, "big") + ssrc.to_bytes(4, "big"))

    if len(nal) <= mtu:
        return [header(sequence, marker) + nal]
    packets = []
    nal_header = nal[0]
    fu_indicator = (nal_header & 0xE0) | 28
    fu_header_base = nal_header & 0x1F
    payload = nal[1:]
    offset = 0
    while offset < len(payload):
        first = offset == 0
        chunk = payload[offset:offset + mtu]
        offset += len(chunk)
        last = offset >= len(payload)
        fu_header = (0x80 if first else 0) | (0x40 if last else 0) | fu_header_base
        packets.append(header(sequence + len(packets), marker and last) +
                       bytes([fu_indicator, fu_header]) + chunk)
    return packets


class RtspSource:
    def __init__(self, h264_path):
        self.nals = split_annex_b(open(h264_path, "rb").read())
        self.units = access_units(self.nals)
        self.sps = next(nal for nal in self.nals if (nal[0] & 0x1F) == 7)
        self.pps = next(nal for nal in self.nals if (nal[0] & 0x1F) == 8)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", RTSP_PORT))
        self.sock.listen(4)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.accept_loop, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        try:
            self.sock.close()
        except OSError:
            pass
        self.thread.join(timeout=2)

    def accept_loop(self):
        while not self.stop.is_set():
            try:
                client, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve_client, args=(client,), daemon=True).start()

    def serve_client(self, client):
        session = str(random.randint(10000000, 99999999))
        sending = threading.Event()
        write_lock = threading.Lock()
        sender = None
        try:
            while not self.stop.is_set():
                request = self.read_request(client)
                if request is None:
                    return
                method, path, headers, _ = request
                print("RTSP:", method, path,
                      headers.get("transport", ""), flush=True)
                cseq = headers.get("cseq", "0")
                if method == "OPTIONS":
                    self.reply(client, cseq, 200, "OK",
                               ["Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"])
                elif method == "DESCRIBE":
                    sdp = self.sdp()
                    self.reply(client, cseq, 200, "OK",
                               ["Content-Type: application/sdp",
                                f"Content-Base: rtsp://127.0.0.1:{RTSP_PORT}{path}/"], sdp,
                               write_lock)
                elif method == "SETUP":
                    transport = headers.get("transport", "")
                    if "TCP" not in transport.upper() or "interleaved" not in transport:
                        self.reply(client, cseq, 461, "Unsupported Transport", lock=write_lock)
                        continue
                    self.reply(client, cseq, 200, "OK",
                               [f"Transport: {transport}",
                                f"Session: {session};timeout=60"],
                               lock=write_lock)
                elif method == "PLAY":
                    self.reply(client, cseq, 200, "OK",
                               [f"Session: {session}",
                                "Range: npt=0.000-",
                                f"RTP-Info: url=rtsp://127.0.0.1:{RTSP_PORT}{path}/trackID=0;"
                                "seq=1;rtptime=0"],
                               lock=write_lock)
                    if sender is None:
                        sending.set()
                        sender = threading.Thread(target=self.send_rtp,
                                                  args=(client, sending, write_lock), daemon=True)
                        sender.start()
                elif method == "TEARDOWN":
                    self.reply(client, cseq, 200, "OK", [f"Session: {session}"], lock=write_lock)
                    sending.clear()
                    return
                elif method in ("GET_PARAMETER", "SET_PARAMETER"):
                    self.reply(client, cseq, 200, "OK", [f"Session: {session}"], lock=write_lock)
                else:
                    self.reply(client, cseq, 501, "Not Implemented", lock=write_lock)
        except OSError:
            # The client may reset the TCP connection during shutdown; that is
            # not a test failure.
            pass
        finally:
            sending.clear()
            try:
                client.close()
            except OSError:
                pass

    def sdp(self):
        import base64
        profile = self.sps[1:4].hex() if len(self.sps) >= 4 else "42e01f"
        sps = base64.b64encode(self.sps).decode()
        pps = base64.b64encode(self.pps).decode()
        return (
            "v=0\r\n"
            "o=- 0 0 IN IP4 127.0.0.1\r\n"
            "s=uvnvr-gb28181-test\r\n"
            "c=IN IP4 127.0.0.1\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            f"a=fmtp:96 packetization-mode=1;profile-level-id={profile};"
            f"sprop-parameter-sets={sps},{pps}\r\n"
            f"a=control:rtsp://127.0.0.1:{RTSP_PORT}/live/trackID=0\r\n"
        )

    @staticmethod
    def read_request(client):
        data = b""
        while b"\r\n\r\n" not in data:
            try:
                chunk = client.recv(4096)
            except OSError:
                return None
            if not chunk:
                return None
            data += chunk
        head, rest = data.split(b"\r\n\r\n", 1)
        lines = head.decode("utf-8", "replace").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) < 2:
            return None
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        while len(rest) < length:
            try:
                chunk = client.recv(4096)
            except OSError:
                return None
            if not chunk:
                return None
            rest += chunk
        return parts[0], parts[1], headers, rest[:length]

    @staticmethod
    def reply(client, cseq, code, reason, headers=None, body="", lock=None):
        payload = body.encode("utf-8")
        lines = [f"RTSP/1.0 {code} {reason}", f"CSeq: {cseq}"]
        lines.extend(headers or [])
        lines.append(f"Content-Length: {len(payload)}")
        message = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + payload
        if lock is None:
            client.sendall(message)
        else:
            with lock:
                client.sendall(message)

    def send_rtp(self, client, sending, write_lock):
        sequence = 1
        ssrc = random.randint(1, 0xFFFFFFFF)
        timestamp = 0
        index = 0
        while sending.is_set() and not self.stop.is_set():
            unit = self.units[index % len(self.units)]
            index += 1
            nals = unit
            packets = []
            for position, nal in enumerate(nals):
                marker = position == len(nals) - 1
                before = len(packets)
                packets.extend(rtp_packets(nal, timestamp, marker, sequence, ssrc))
                sequence = (sequence + len(packets) - before) & 0xFFFF
            for packet in packets:
                frame = b"$" + bytes([0]) + len(packet).to_bytes(2, "big") + packet
                try:
                    # RTSP replies and interleaved RTP share one TCP stream;
                    # a partial interleave from two threads corrupts both.
                    with write_lock:
                        client.sendall(frame)
                except OSError:
                    sending.clear()
                    return
            timestamp = (timestamp + 9000) & 0xFFFFFFFF
            time.sleep(0.1)


# ---------------------------------------------------------------------------
# Database and service lifecycle
# ---------------------------------------------------------------------------


def first_run(binary, data_dir):
    proc = subprocess.Popen(
        [binary, "--data-dir", data_dir, "--port", str(HTTP_PORT),
         "--rtsp-port", str(RTSP_PORT + 1), "--private-port", str(PRIVATE_PORT),
         "--log-level", "warn"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        wait_health(proc, HTTP_PORT)
    finally:
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=5)


def prepare_database(data_dir):
    db_path = os.path.join(data_dir, "uvnvr.db")
    now = int(time.time())
    db = sqlite3.connect(db_path)
    try:
        db.execute(
            "INSERT OR REPLACE INTO channels(id,name,protocol,host,port,username,password_enc,"
            "main_url,sub_url,sub_enabled,enabled,brand,media_json,watermark_json,last_status,"
            "last_error,last_online_at,last_offline_at,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (CHANNEL_LOCAL_ID, "ai-gate", "onvif", "127.0.0.1", 80, "", "",
             f"rtsp://127.0.0.1:{RTSP_PORT}/live", "", 0, 1, "",
             json.dumps({"httpPort": ONVIF_PORT}), "{}", "unknown", "", 0, 0, 0, 0))
        db.execute(
            "INSERT OR REPLACE INTO ai_channel_settings(channel_id,plate_enabled,face_enabled,"
            "interval_ms,plate_roi,face_roi,updated_at) VALUES(?,?,?,?,?,?,?)",
            (CHANNEL_LOCAL_ID, 1, 0, 500, "", "", now))
        settings = {
            "gb28181.enabled": "1",
            "gb28181.device_id": DEVICE_ID,
            "gb28181.name": "UvNVR-ptz-integration",
            "gb28181.center_code": CENTER_CODE,
            "gb28181.server_host": "127.0.0.1",
            "gb28181.server_port": str(SIP_PORT),
            "gb28181.transport": "udp",
            "gb28181.local_ip": "127.0.0.1",
            "gb28181.bind_ip": "127.0.0.1",
            "gb28181.local_port": str(DEVICE_SIP_PORT),
            "gb28181.username": DEVICE_ID,
            "gb28181.password": "",
            "gb28181.keepalive": "10",
            f"gb28181.channel.{CHANNEL_LOCAL_ID}.device_id": CHANNEL_ID,
            "ai.enabled": "1",
            "ai.plate.enabled": "1",
            "ai.face.enabled": "0",
            "ai.stub": "1",
            "ai.intervalMs": "500",
            "ai.plateWorkers": "1",
            "ai.plate.watchlist": "京A12345",
            "ai.dedupSeconds": "0",
            "ai.snapshotMaxPerDay": "1000",
            "ai.snapshotMaxTotalMb": "512",
            "ai.snapshotMinFreeMb": "16",
            "motion.enabled": "0",
        }
        for key, value in settings.items():
            db.execute("INSERT OR REPLACE INTO settings(key,value,updated_at) VALUES(?,?,?)",
                       (key, value, now))
        db.commit()
    finally:
        db.close()


def start_service(binary, data_dir, log_path):
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [binary, "--data-dir", data_dir, "--port", str(HTTP_PORT),
         "--rtsp-port", str(RTSP_PORT + 1), "--private-port", str(PRIVATE_PORT),
         "--log-level", "debug"],
        stdout=log, stderr=subprocess.STDOUT, text=True)
    return proc, log


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def run(args):
    data_dir = tempfile.mkdtemp(prefix="uvnvr-gb28181-ptz-")
    log_path = os.path.join(data_dir, "uvnvr.log")
    onvif_server = None
    rtsp = None
    service = None
    platform = None
    log = None
    try:
        first_run(args.binary, data_dir)
        print("database: schema initialised at", os.path.join(data_dir, "uvnvr.db"))
        prepare_database(data_dir)

        onvif_server, onvif_state = start_onvif_server()
        platform = Platform()
        h264_path = os.path.join(data_dir, "source.h264")
        generate_h264(args.ffmpeg, h264_path)
        rtsp = RtspSource(h264_path)
        check(len(rtsp.units) > 10, "synthetic RTSP/H.264 source is ready")

        service, log = start_service(args.binary, data_dir, log_path)
        wait_health(service, HTTP_PORT)
        platform.wait(lambda: platform.registered.is_set(), timeout=20,
                      description="REGISTER")
        check(True, "device registered with the fake platform")

        # --- PTZ: continuous move, stop, goto preset ---------------------
        platform.send(sip_request(
            "MESSAGE", f"{DEVICE_ID}@127.0.0.1:{DEVICE_SIP_PORT}", "ptz-call-1", 10,
            device_control_xml(1, "right", h_speed=0xFF),
            "Application/MANSCDP+xml"))

        def found(marker):
            return any(marker in body for body in onvif_state.snapshot())

        platform.wait(lambda: found("ContinuousMove"), timeout=10,
                      description="ONVIF ContinuousMove")
        moves = [body for body in onvif_state.snapshot() if "ContinuousMove" in body]
        move = moves[-1]
        check("profile_main" in move, "ONVIF ContinuousMove uses the media profile token")
        match = re.search(r'<tt:PanTilt x="([-0-9.]+)" y="([-0-9.]+)"', move)
        check(match is not None and float(match.group(1)) > 0.9,
              f"platform PTZ right arrives as a positive ONVIF pan velocity ({match.group(0) if match else 'missing'})")

        platform.send(sip_request(
            "MESSAGE", f"{DEVICE_ID}@127.0.0.1:{DEVICE_SIP_PORT}", "ptz-call-2", 11,
            device_control_xml(2, "stop"), "Application/MANSCDP+xml"))
        platform.wait(lambda: found("<tptz:Stop>"), timeout=10, description="ONVIF Stop")
        check(True, "platform PTZ stop reaches the ONVIF Stop operation")

        platform.send(sip_request(
            "MESSAGE", f"{DEVICE_ID}@127.0.0.1:{DEVICE_SIP_PORT}", "ptz-call-3", 12,
            device_control_xml(3, "goto-preset", preset=5), "Application/MANSCDP+xml"))
        platform.wait(lambda: found("<tptz:GotoPreset>"), timeout=10,
                      description="ONVIF GotoPreset")
        preset = [body for body in onvif_state.snapshot() if "<tptz:GotoPreset>" in body][-1]
        check("<tptz:PresetToken>5</tptz:PresetToken>" in preset,
              "platform PTZ call-preset 5 arrives as ONVIF PresetToken 5")

        # --- SUBSCRIBE / NOTIFY ------------------------------------------
        subscription_call = "alarm-sub-1"
        platform.send(sip_request(
            "SUBSCRIBE", f"{DEVICE_ID}@127.0.0.1:{DEVICE_SIP_PORT}", subscription_call, 20,
            extra=[("Event", "Alarm"), ("Expires", "60")]))
        platform.wait(lambda: any("Subscription-State: active" in body
                                  for body in platform.notify_bodies),
                      timeout=10, description="initial Alarm NOTIFY")
        check(True, "SUBSCRIBE Alarm answered with an active NOTIFY")

        # --- AI plate event -> GB Alarm ----------------------------------
        platform.wait(lambda: any("AI-PLATE" in body and "京A12345" in body
                                  for body in platform.alarm_bodies),
                      timeout=60, description="AI plate Alarm MESSAGE")
        alarm = [body for body in platform.alarm_bodies
                 if "AI-PLATE" in body and "京A12345" in body][0]
        print("--- Alarm MESSAGE ---")
        print(alarm)
        check("<CmdType>Alarm</CmdType>" in alarm, "AI event is carried by a MANSCDP Alarm")
        check("<AlarmMethod>5</AlarmMethod>" in alarm, "AI event uses AlarmMethod 5 (video)")
        check("<AlarmTypeParam>AI-PLATE</AlarmTypeParam>" in alarm,
              "AlarmTypeParam identifies the AI plate source")
        check("<DeviceID>" + CHANNEL_ID + "</DeviceID>" in alarm,
              "alarm DeviceID is the GB channel identifier")

        platform.wait(lambda: any("AI-PLATE" in body and "京A12345" in body
                                  for body in platform.notify_bodies),
                      timeout=30, description="AI alarm NOTIFY")
        notified = [body for body in platform.notify_bodies
                    if "AI-PLATE" in body and "京A12345" in body]
        check(len(notified) > 0, "AI alarm is also pushed to the Alarm subscriber as NOTIFY")

        # --- unsubscribe --------------------------------------------------
        before = len([body for body in platform.notify_bodies
                      if "Subscription-State: terminated" in body])
        platform.send(sip_request(
            "SUBSCRIBE", f"{DEVICE_ID}@127.0.0.1:{DEVICE_SIP_PORT}", subscription_call, 21,
            extra=[("Event", "Alarm"), ("Expires", "0")]))
        platform.wait(lambda: len([body for body in platform.notify_bodies
                                   if "Subscription-State: terminated" in body]) > before,
                      timeout=10, description="terminated NOTIFY")
        check(True, "SUBSCRIBE Expires: 0 terminates the subscription")

        print("--- last 5 ONVIF requests ---")
        for body in onvif_state.snapshot()[-5:]:
            operation = re.search(r"<tptz:([A-Za-z]+)", body)
            print("  ", operation.group(1) if operation else "GetCapabilities/GetProfiles")
        return 0
    except CheckFailed as exc:
        print("FAILED:", exc)
        if log is not None:
            log.flush()
        if os.path.exists(log_path):
            print(f"--- uvnvr log tail ({log_path}) ---")
            with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
                for line in handle.readlines()[-80:]:
                    print(line.rstrip())
        return 1
    finally:
        if service is not None and service.poll() is None:
            service.send_signal(signal.SIGTERM)
            try:
                service.wait(timeout=8)
            except subprocess.TimeoutExpired:
                service.kill()
        if log is not None:
            log.close()
        if platform is not None:
            platform.close()
        if rtsp is not None:
            rtsp.close()
        if onvif_server is not None:
            onvif_server.shutdown()
            onvif_server.server_close()
        if not args.keep:
            shutil.rmtree(data_dir, ignore_errors=True)
        else:
            print("data directory kept at", data_dir)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=os.environ.get("UVNVR_BINARY", "build/uvnvr"))
    parser.add_argument("--ffmpeg", default=os.environ.get("FFMPEG", "ffmpeg"))
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    if not os.path.exists(args.binary):
        print("binary not found:", args.binary, file=sys.stderr)
        return 2
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
