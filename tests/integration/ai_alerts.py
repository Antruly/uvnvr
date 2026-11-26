#!/usr/bin/env python3
"""AI alert-rule integration test (GH-37).

Starts UvNVR with a synthetic RTSP source served by an in-process RTSP/TCP
server, forces the deterministic stub AI engine, and checks the three GH-37
acceptance items end to end:

  * a plate on the blacklist writes an ``alerts`` row linked to its
    ``ai_plate_events`` row and pushes it on ``/ws/events``; a whitelist entry
    wins over the blacklist;
  * the face library does a 1:N cosine search with ``ai.face.matchThreshold``,
    both in the live pipeline and in ``POST /api/v1/ai/faces/search``;
  * the alert list links back to the AI event and acknowledgement records the
    operator, timestamp and note (unknown ids return 404).

The stub engines always report plate ``京A12345`` and a fixed 128-float face
feature, which the test reproduces to seed the library.

Usage: tests/integration/ai_alerts.py [path-to-uvnvr] [http-port]
Requires ffmpeg in PATH.
"""
import base64
import http.cookiejar
import json
import math
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 18580)
RTSP_PORT = PORT + 100
SOURCE_PORT = PORT + 101
PASSWORD = "Admin12345"
PLATE = "京A12345"
FFMPEG = shutil.which("ffmpeg")

TS_CHUNK = 188 * 7  # integral MPEG-TS packets, as RFC 2250 recommends

failures = []


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name + (" :: " + str(detail) if detail else ""))
    if not condition:
        failures.append(name)


def stub_feature():
    """Mirror of StubFeature() in src/ai/ai_engine.cpp."""
    values = [math.sin(i * 0.37) + math.cos(i * 0.11) for i in range(128)]
    norm = math.sqrt(sum(value * value for value in values)) or 1.0
    return [value / norm for value in values]


class FakeRtspServer:
    """Serves an MPEG-TS file as RTP/MP2T over an RTSP/TCP connection."""

    def __init__(self, ts_data, port):
        self.ts_data = ts_data
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", port))
        self.socket.listen(4)
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass

    def _serve(self):
        while True:
            try:
                connection, _ = self.socket.accept()
            except OSError:
                return
            threading.Thread(target=self._client, args=(connection,), daemon=True).start()

    @staticmethod
    def _response(code, reason, headers, cseq):
        lines = [f"RTSP/1.0 {code} {reason}", f"CSeq: {cseq}"]
        lines.extend(f"{name}: {value}" for name, value in headers.items())
        return ("\r\n".join(lines) + "\r\n\r\n").encode()

    def _client(self, connection):
        connection.settimeout(120)
        buffer = b""
        playing = threading.Event()
        stop = threading.Event()

        def stream():
            sequence = 0
            timestamp = 0
            offset = 0
            started = time.monotonic()
            chunk_seconds = 0.008  # a little faster than real time (15 fps)
            while not stop.is_set():
                chunk = self.ts_data[offset:offset + TS_CHUNK]
                if len(chunk) < TS_CHUNK:
                    offset = 0
                    chunk = self.ts_data[:TS_CHUNK]
                offset += TS_CHUNK
                rtp = struct.pack(">BBHII", 0x80, 33, sequence & 0xFFFF,
                                  timestamp & 0xFFFFFFFF, 0x12345678)
                payload = rtp + chunk
                try:
                    connection.sendall(b"$" + bytes([0]) + struct.pack(">H", len(payload)) + payload)
                except OSError:
                    stop.set()
                    return
                sequence += 1
                timestamp += int(90000 * chunk_seconds)
                started += chunk_seconds
                delay = started - time.monotonic()
                if delay > 0:
                    time.sleep(delay)

        try:
            while not stop.is_set():
                data = connection.recv(65536)
                if not data:
                    break
                buffer += data
                while b"\r\n\r\n" in buffer:
                    head, buffer = buffer.split(b"\r\n\r\n", 1)
                    lines = head.decode("latin-1", "replace").split("\r\n")
                    method, url, _ = lines[0].split(" ", 2)
                    headers = {}
                    for line in lines[1:]:
                        if ":" in line:
                            name, value = line.split(":", 1)
                            headers[name.strip().lower()] = value.strip()
                    cseq = headers.get("cseq", "0")
                    if method == "OPTIONS":
                        connection.sendall(self._response(200, "OK", {
                            "Public": "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN",
                        }, cseq))
                    elif method == "DESCRIBE":
                        sdp = (
                            "v=0\r\n"
                            "o=- 0 0 IN IP4 127.0.0.1\r\n"
                            "s=UvNVR test source\r\n"
                            "c=IN IP4 0.0.0.0\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 33\r\n"
                            "a=rtpmap:33 MP2T/90000\r\n"
                            "a=control:track1\r\n"
                        ).encode()
                        connection.sendall(self._response(200, "OK", {
                            "Content-Base": url.rstrip("/") + "/",
                            "Content-Type": "application/sdp",
                            "Content-Length": str(len(sdp)),
                        }, cseq) + sdp)
                    elif method == "SETUP":
                        connection.sendall(self._response(200, "OK", {
                            "Transport": "RTP/AVP/TCP;unicast;interleaved=0-1",
                            "Session": "12345678;timeout=60",
                        }, cseq))
                    elif method == "PLAY":
                        connection.sendall(self._response(200, "OK", {
                            "Session": "12345678;timeout=60",
                            "Range": "npt=0.000-",
                        }, cseq))
                        if not playing.is_set():
                            playing.set()
                            threading.Thread(target=stream, daemon=True).start()
                    elif method == "TEARDOWN":
                        connection.sendall(self._response(200, "OK", {}, cseq))
                        stop.set()
                        return
                    else:
                        connection.sendall(self._response(200, "OK", {}, cseq))
        except (OSError, ValueError):
            pass
        finally:
            stop.set()
            connection.close()


class EventSocket:
    """Minimal RFC 6455 client used to observe /ws/events pushes."""

    def __init__(self, port, path, timeout=10):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.socket.sendall(request.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("websocket handshake failed")
            data += chunk
        head, self.buffer = data.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError("websocket handshake rejected: " + head.decode("latin-1"))

    def _read(self, size):
        while len(self.buffer) < size:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("websocket closed")
            self.buffer += chunk
        result, self.buffer = self.buffer[:size], self.buffer[size:]
        return result

    def _pong(self, payload):
        self.socket.sendall(bytes([0x8A, len(payload)]) + payload)

    def recv(self, timeout=0.5):
        self.socket.settimeout(timeout)
        while True:
            header = self._read(2)
            opcode = header[0] & 0x0F
            masked = header[1] & 0x80
            length = header[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if masked else b""
            payload = self._read(length)
            if masked:
                payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
            if opcode == 0x9:
                self._pong(payload)
                continue
            if opcode == 0x8:
                return None
            if opcode in (0x1, 0x2):
                return payload.decode("utf-8", "replace")

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


def main():
    if FFMPEG is None:
        print("ffmpeg is required for the synthetic RTSP source", file=sys.stderr)
        return 2
    temp = Path(tempfile.mkdtemp(prefix="uvnvr-ai-alerts-"))
    source_file = temp / "source.ts"
    generated = subprocess.run(
        [FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
         "-f", "lavfi", "-i", "testsrc2=size=640x480:rate=15", "-t", "15",
         "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
         "-g", "15", "-pix_fmt", "yuv420p", "-f", "mpegts", str(source_file)],
        capture_output=True, text=True)
    if generated.returncode != 0:
        print(generated.stderr.strip(), file=sys.stderr)
        return 2
    fake = FakeRtspServer(source_file.read_bytes(), SOURCE_PORT)
    fake.start()

    data_dir = temp / "data"
    data_dir.mkdir()
    config = temp / "uvnvr.json"
    config.write_text(json.dumps({
        "http": {"port": PORT, "host": "127.0.0.1"},
        "rtsp": {"port": RTSP_PORT},
        "dataDir": str(data_dir),
        "logging": {"level": "info"},
    }))
    log = open(temp / "server.log", "ab")
    server = subprocess.Popen([str(BIN), "--config", str(config), "--ffmpeg", FFMPEG],
                              stdout=log, stderr=log)
    pull = None
    ws = None
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/healthz", timeout=1).read()
                break
            except Exception:
                time.sleep(0.2)
        else:
            print("server did not start; see " + str(temp / "server.log"), file=sys.stderr)
            return 1

        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        state = {"csrf": ""}

        def fetch(method, path, body=None, timeout=30):
            request = urllib.request.Request(
                f"http://127.0.0.1:{PORT}{path}",
                data=None if body is None else json.dumps(body).encode(),
                method=method)
            if body is not None:
                request.add_header("Content-Type", "application/json")
            if state["csrf"] and method not in ("GET", "HEAD"):
                request.add_header("X-CSRF-Token", state["csrf"])
            try:
                with opener.open(request, timeout=timeout) as response:
                    return (response.status, response.headers.get("Content-Type", ""),
                            response.read())
            except urllib.error.HTTPError as error:
                return error.code, error.headers.get("Content-Type", ""), error.read()

        def call(method, path, body=None, timeout=30):
            status, _, raw = fetch(method, path, body, timeout)
            return status, json.loads(raw or b"null")

        status, setup = call("POST", "/api/v1/setup", {
            "password": PASSWORD, "confirmPassword": PASSWORD, "timezone": "UTC"})
        check("setup", status == 200, status)
        state["csrf"] = setup["csrfToken"]

        status, created = call("POST", "/api/v1/channels", {
            "protocol": "rtsp", "name": "gh37-source",
            "mainUrl": f"rtsp://127.0.0.1:{SOURCE_PORT}/live"})
        check("channel create", status == 201, (status, created))
        channel_id = created["id"]

        status, config_json = call("PUT", "/api/v1/ai/config", {
            "enabled": True,
            "stub": True,
            "plateEnabled": True,
            "faceEnabled": True,
            "intervalMs": 500,
            "minPlateConfidence": 0.1,
            "minFaceQuality": 0.1,
            "dedupSeconds": 1,
            "faceDedupSeconds": 1,
            "faceMatchThreshold": 0.9,
            "plateBlacklist": PLATE,
            "plateWhitelist": "京A00001",
        })
        check("ai config update", status == 200, (status, config_json.get("error")))

        feature = stub_feature()
        face_ids = []
        for name in ("alice", "bob"):
            status, face = call("POST", "/api/v1/ai/faces", {"name": name, "feature": feature})
            check(f"face create {name}", status == 201, (status, face))
            face_ids.append(face.get("id"))
        lower_id = min(face_ids)
        status, config_json = call("PUT", "/api/v1/ai/config",
                                   {"faceWatchlist": str(lower_id)})
        check("face watchlist update", status == 200, (status, config_json.get("error")))

        status, enabled = call("PUT", "/api/v1/ai/channels", {
            "channelIds": [channel_id], "plateEnabled": True, "faceEnabled": True})
        check("channel ai enable", status == 200, (status, enabled))

        status, ticket = call("POST", "/api/v1/events/ticket")
        check("events ticket", status == 200, status)
        messages = []
        try:
            ws = EventSocket(PORT, ticket["wsPath"])

            def reader():
                while True:
                    try:
                        text = ws.recv(timeout=0.5)
                    except TimeoutError:
                        continue
                    except Exception:
                        return
                    if text is None:
                        return
                    try:
                        messages.append(json.loads(text))
                    except ValueError:
                        pass

            threading.Thread(target=reader, daemon=True).start()
        except Exception as error:
            check("event socket connected", False, error)

        pull_url = f"rtsp://admin:{PASSWORD}@127.0.0.1:{RTSP_PORT}/live/{channel_id}/main"
        pull = subprocess.Popen(
            [FFMPEG, "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
             "-i", pull_url, "-t", "90", "-c", "copy", "-f", "mpegts",
             "-y", str(temp / "pull.ts")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.time() + 45
        plates = []
        faces = []
        while time.time() < deadline:
            _, events = call("GET", "/api/v1/ai/events?type=plate&limit=5")
            _, face_events = call("GET", "/api/v1/ai/events?type=face&limit=5")
            plates = events.get("items", []) if isinstance(events, dict) else []
            faces = face_events.get("items", []) if isinstance(face_events, dict) else []
            if plates and faces:
                break
            time.sleep(1)
        check("plate detection produced an event", bool(plates), plates[:1])
        check("face detection produced an event", bool(faces), faces[:1])
        check("plate detection matches the stub plate",
              bool(plates) and plates[0].get("plate") == PLATE,
              plates[0].get("plate") if plates else None)
        check("1:N tie resolves to the lower face id",
              bool(faces) and faces[0].get("matchedFaceId") == lower_id,
              faces[0].get("matchedFaceId") if faces else None)
        check("live 1:N score reaches the threshold",
              bool(faces) and faces[0].get("matchedScore", 0) >= 0.9,
              faces[0].get("matchedScore") if faces else None)

        time.sleep(2)
        _, alerts = call("GET", "/api/v1/alerts?limit=50")
        items = alerts if isinstance(alerts, list) else alerts.get("items", [])
        plate_alerts = [alert for alert in items if alert.get("type") == "ai_plate"]
        face_alerts = [alert for alert in items if alert.get("type") == "ai_face"]
        check("plate hit wrote an alerts row", bool(plate_alerts), plate_alerts[:1])
        check("face hit wrote an alerts row", bool(face_alerts), face_alerts[:1])
        if plate_alerts:
            alert = plate_alerts[0]
            check("plate alert links its ai_plate_events row",
                  alert.get("sourceType") == "plate" and alert.get("sourceId", 0) > 0,
                  (alert.get("sourceType"), alert.get("sourceId")))
            check("plate alert exposes snapshot and source URLs",
                  bool(alert.get("snapshotUrl")) and bool(alert.get("sourceUrl")),
                  (alert.get("snapshotUrl"), alert.get("sourceUrl")))
        if face_alerts:
            alert = face_alerts[0]
            check("face alert links its ai_face_events row",
                  alert.get("sourceType") == "face" and alert.get("sourceId", 0) > 0,
                  (alert.get("sourceType"), alert.get("sourceId")))

        pushed = [message for message in messages if message.get("type") == "alert"]
        check("alert pushed on /ws/events", bool(pushed), pushed[:1])
        plate_push = [message for message in pushed
                      if message.get("alert", {}).get("type") == "ai_plate"]
        check("plate alert push carries source metadata",
              bool(plate_push) and plate_push[0]["alert"].get("sourceType") == "plate",
              plate_push[:1])
        plate_messages = [message for message in messages if message.get("type") == "ai.plate"]
        check("ai.plate push reports list=black",
              any(message.get("list") == "black" and message.get("alert") is True
                  for message in plate_messages), plate_messages[:1])

        status, _ = call("PUT", "/api/v1/ai/config",
                         {"plateWhitelist": PLATE, "dedupSeconds": 0})
        check("whitelist config", status == 200, status)
        # Frames queued under the old settings can still raise one alert, so
        # let the pipeline settle before taking the baseline.
        time.sleep(3)
        _, before_items = call("GET", "/api/v1/alerts?type=ai_plate")
        before = len(before_items) if isinstance(before_items, list) else 0
        time.sleep(6)
        _, after_items = call("GET", "/api/v1/alerts?type=ai_plate")
        after = len(after_items) if isinstance(after_items, list) else 0
        check("whitelist suppresses new alerts", after == before, (before, after))
        check("ai.plate push reports list=white",
              any(message.get("type") == "ai.plate" and message.get("list") == "white" and
                  message.get("alert") is False for message in messages))

        status, search = call("POST", "/api/v1/ai/faces/search",
                              {"feature": feature, "limit": 10})
        check("face search status", status == 200, (status, search))
        check("face search inherits ai.face.matchThreshold",
              search.get("threshold") == 0.9 and search.get("matched") is True, search)
        check("face search tie keeps the lower id first",
              bool(search.get("items")) and search["items"][0]["id"] == lower_id,
              search.get("items", [])[:2])
        status, strict = call("POST", "/api/v1/ai/faces/search",
                              {"feature": feature, "threshold": 1.0})
        check("face search honours an explicit threshold",
              status == 200 and strict.get("matched") is True, strict)
        unrelated = [1.0 if index == 0 else 0.0 for index in range(128)]
        status, miss = call("POST", "/api/v1/ai/faces/search",
                            {"feature": unrelated, "threshold": 0.99})
        check("face search rejects an unrelated feature",
              status == 200 and miss.get("matched") is False, miss.get("items", [])[:1])
        status, bad = call("POST", "/api/v1/ai/faces/search", {"feature": []})
        check("face search rejects an empty feature", status == 400, (status, bad))

        status, ack = call("POST", f"/api/v1/alerts/{plate_alerts[0]['id']}/ack",
                           {"note": "checked by e2e"}) if plate_alerts else (0, {})
        check("acknowledge with a note",
              status == 200 and ack.get("acknowledged") is True, (status, ack))
        check("acknowledgement records operator, time and note",
              ack.get("acknowledgedBy") == "admin" and ack.get("acknowledgedAt", 0) > 0 and
              ack.get("note") == "checked by e2e", ack)
        _, listed = call("GET", "/api/v1/alerts?acknowledged=1&type=ai_plate")
        acked = [alert for alert in listed if alert.get("id") == plate_alerts[0]["id"]] \
            if plate_alerts else []
        check("acknowledged alert is listed with its note",
              bool(acked) and acked[0].get("acknowledged") is True and
              acked[0].get("note") == "checked by e2e", acked[:1])
        status, missing = call("POST", "/api/v1/alerts/999999/ack", {"note": "x"})
        check("acknowledging an unknown id returns 404", status == 404, (status, missing))

        if plate_alerts:
            status, content_type, body = fetch(
                "GET", f"/api/v1/ai/events/{plate_alerts[0]['sourceId']}"
                       "/snapshot?type=plate&kind=crop")
            check("AI crop snapshot served",
                  status == 200 and content_type.startswith("image/jpeg") and
                  body[:2] == b"\xff\xd8", (status, content_type, len(body)))
            status, content_type, body = fetch("GET", plate_alerts[0]["snapshotUrl"])
            check("alert snapshot served",
                  status == 200 and content_type.startswith("image/jpeg") and
                  body[:2] == b"\xff\xd8", (status, content_type, len(body)))
    finally:
        if pull is not None:
            pull.terminate()
            try:
                pull.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pull.kill()
        if ws is not None:
            ws.close()
        fake.close()
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        if failures:
            print("--- server log tail ---", file=sys.stderr)
            print((temp / "server.log").read_text(errors="replace")[-4000:], file=sys.stderr)
        print(f"workdir: {temp}")
    if failures:
        print(f"{len(failures)} check(s) failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    print("ai_alerts integration passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
