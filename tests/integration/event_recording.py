#!/usr/bin/env python3
"""Event-triggered recording integration test.

Combines a MediaMTX test stream with a mock ONVIF camera: an event plan is
active, the mock reports motion, and the recorder must produce an ``event``
segment that starts with pre-roll data and stops after the post-roll.

Usage: tests/integration/event_recording.py [path-to-uvnvr] [http-port]
Requires ffmpeg in PATH and MEDIAMTX_BIN (or mediamtx in PATH).
"""
import http.cookiejar
import http.server
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19610)
RTSP_PORT = PORT + 100
MEDIAMTX_PORT = PORT + 200
MOCK_PORT = PORT + 300

FFMPEG = shutil.which("ffmpeg")
MEDIAMTX = os.environ.get("MEDIAMTX_BIN") or shutil.which("mediamtx")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def pick_port(preferred: int) -> int:
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.0.1", preferred))
            return preferred
        except OSError:
            return free_port()


class MockOnvif(http.server.BaseHTTPRequestHandler):
    pulls = 0
    send_motion = False
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        if "GetCapabilities" in body:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
                'xmlns:tt="http://www.onvif.org/ver10/schema">'
                "<s:Body><tds:GetCapabilitiesResponse><tds:Capabilities><tt:Events>"
                f"<tt:XAddr>http://127.0.0.1:{MOCK_PORT}/onvif/events</tt:XAddr>"
                "</tt:Events></tds:Capabilities></tds:GetCapabilitiesResponse></s:Body>"
                "</s:Envelope>"
            )
        elif "CreatePullPointSubscription" in body:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tev="http://www.onvif.org/ver10/events/wsdl" '
                'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" '
                'xmlns:wsa5="http://www.w3.org/2005/08/addressing">'
                "<s:Body><tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
                f"<wsa5:Address>http://127.0.0.1:{MOCK_PORT}/pull1</wsa5:Address>"
                "</tev:SubscriptionReference></tev:CreatePullPointSubscriptionResponse>"
                "</s:Body></s:Envelope>"
            )
        elif "PullMessages" in body:
            with MockOnvif.lock:
                MockOnvif.pulls += 1
                first = MockOnvif.pulls == 1
                motion = MockOnvif.send_motion and (first or MockOnvif.pulls % 2 == 0)
            notification = ""
            if motion:
                notification = (
                    '<wsnt:NotificationMessage xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2">'
                    '<wsnt:Topic Dialect="http://www.onvif.org/ver10/tev/topicExpression/'
                    'ConcreteSet">tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>'
                    "<wsnt:Message><tt:Message "
                    'xmlns:tt="http://www.onvif.org/ver10/schema">'
                    '<tt:Data><tt:SimpleItem Name="IsMotion" Value="true"/></tt:Data>'
                    "</tt:Message></wsnt:Message></wsnt:NotificationMessage>"
                )
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tev="http://www.onvif.org/ver10/events/wsdl">'
                f"<s:Body><tev:PullMessagesResponse>{notification}"
                "</tev:PullMessagesResponse></s:Body></s:Envelope>"
            )
        else:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body/>'
                "</s:Envelope>"
            )
        data = response.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/soap+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    global PORT, RTSP_PORT, MEDIAMTX_PORT, MOCK_PORT
    if not FFMPEG or not MEDIAMTX:
        print("skipping event recording test: ffmpeg or MediaMTX is not available")
        return 0
    encoders = subprocess.run(
        [FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True
    ).stdout
    encoder = "libx264" if "libx264" in encoders else "libopenh264" if "libopenh264" in encoders else ""
    if not encoder:
        print("skipping event recording test: no H.264 encoder in ffmpeg")
        return 0

    PORT = pick_port(PORT)
    RTSP_PORT = pick_port(PORT + 100)
    MEDIAMTX_PORT = pick_port(PORT + 200)
    MOCK_PORT = pick_port(PORT + 300)
    rtp_port = pick_port(MEDIAMTX_PORT + 1)
    rtcp_port = pick_port(MEDIAMTX_PORT + 2)

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-event-"))
    data_dir = temp / "data"
    data_dir.mkdir()
    volume = temp / "volume"
    volume.mkdir()
    mediamtx_config = temp / "mediamtx.yml"
    mediamtx_config.write_text(
        "\n".join(
            [
                f"rtspAddress: :{MEDIAMTX_PORT}",
                "rtspTransports: [tcp]",
                f"rtpAddress: :{rtp_port}",
                f"rtcpAddress: :{rtcp_port}",
                "rtmp: false",
                "hls: false",
                "webrtc: false",
                "srt: false",
                "api: false",
                "metrics: false",
                "playback: false",
                "moq: false",
                "paths:",
                "  all_others:",
            ]
        )
        + "\n"
    )
    config = temp / "uvnvr.json"
    config.write_text(
        json.dumps(
            {
                "http": {"port": PORT, "host": "127.0.0.1"},
                "rtsp": {"port": RTSP_PORT},
                "dataDir": str(data_dir),
                "logging": {"level": "warn"},
            }
        )
    )
    log = open(temp / "logs.txt", "ab")
    mediamtx = subprocess.Popen([MEDIAMTX, str(mediamtx_config)], stdout=log, stderr=log)
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockOnvif)
    threading.Thread(target=mock.serve_forever, daemon=True).start()

    publisher = None
    app = None
    try:
        ready = False
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", MEDIAMTX_PORT), timeout=0.5).close()
                ready = True
                break
            except OSError:
                time.sleep(0.2)
        if not ready:
            print("skipping event recording test: MediaMTX did not listen")
            return 0
        publisher = subprocess.Popen(
            [
                FFMPEG,
                "-hide_banner",
                "-loglevel",
                "error",
                "-re",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=320x240:rate=15",
                "-c:v",
                encoder,
                "-g",
                "30",
                "-pix_fmt",
                "yuv420p",
                "-f",
                "rtsp",
                "-rtsp_transport",
                "tcp",
                f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam",
            ],
            stdout=log,
            stderr=log,
        )
        time.sleep(2)
        if publisher.poll() is not None:
            print("skipping event recording test: the publisher failed")
            return 0

        environment = os.environ.copy()
        environment["UVNVR_FFMPEG"] = FFMPEG
        app = subprocess.Popen(
            [str(BIN), "--config", str(config)], stdout=log, stderr=log, env=environment
        )

        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        csrf = ""

        def call(method: str, path: str, body: dict | None = None):
            request = urllib.request.Request(
                f"http://127.0.0.1:{PORT}{path}",
                data=None if body is None else json.dumps(body).encode(),
                method=method,
            )
            if body is not None:
                request.add_header("Content-Type", "application/json")
            if csrf and method not in ("GET", "HEAD"):
                request.add_header("X-CSRF-Token", csrf)
            try:
                with opener.open(request, timeout=20) as response:
                    return response.status, json.loads(response.read() or b"null")
            except urllib.error.HTTPError as error:
                return error.code, json.loads(error.read() or b"null")

        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/healthz", timeout=1).read()
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("server did not start; see " + str(temp / "logs.txt"))

        status, setup = call(
            "POST",
            "/api/v1/setup",
            {
                "password": "Admin123!@#",
                "confirmPassword": "Admin123!@#",
                "timezone": "UTC",
            },
        )
        assert status == 200, (status, setup)
        csrf = setup["csrfToken"]

        # Invalid ranges are rejected and nothing is written.
        status, invalid = call(
            "PUT",
            "/api/v1/settings",
            {"event": {"preSeconds": 3, "postSeconds": 50, "maxSeconds": 30}},
        )
        assert status == 400, (status, invalid)

        status, saved = call(
            "PUT",
            "/api/v1/settings",
            {"event": {"enabled": True, "preSeconds": 3, "postSeconds": 4, "maxSeconds": 30}},
        )
        assert status == 200, (status, saved)
        assert saved["event"]["preSeconds"] == 3, saved["event"]
        assert saved["event"]["postSeconds"] == 4, saved["event"]

        status, created = call(
            "POST",
            "/api/v1/storage/volumes",
            {
                "path": str(volume),
                "label": "event",
                "mode": "loop",
                "maxUsagePct": 95,
                "reserveGb": 0,
                "retentionDays": 0,
            },
        )
        assert status == 201, (status, created)

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "event camera",
                "protocol": "onvif",
                "host": "127.0.0.1",
                "port": MOCK_PORT,
                "mainUrl": f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam",
                "enabled": True,
                "eventsEnabled": True,
            },
        )
        assert status == 201, (status, channel)
        channel_id = channel["id"]

        weekday = (time.localtime().tm_wday + 1) % 7
        status, result = call(
            "PUT",
            "/api/v1/plans",
            {
                "channelId": channel_id,
                "plans": [
                    {
                        "weekday": weekday,
                        "startMin": 0,
                        "endMin": 1440,
                        "mode": "event",
                        "stream": "main",
                    }
                ],
            },
        )
        assert status == 200, (status, result)

        # Wait until the stream is online and the scheduler has armed pre-roll.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status, rows = call("GET", "/api/v1/channels")
            current = next((item for item in rows if item["id"] == channel_id), None)
            if current and current.get("lastStatus") == "online":
                break
            time.sleep(0.5)
        else:
            raise AssertionError("the test channel never came online")
        time.sleep(6)

        MockOnvif.send_motion = True
        alert = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status, alerts = call("GET", "/api/v1/alerts?pageSize=50")
            if status == 200:
                alert = next(
                    (
                        item
                        for item in alerts
                        if item.get("channelId") == channel_id
                        and item.get("type") == "motion"
                    ),
                    None,
                )
                if alert:
                    break
            time.sleep(0.5)
        assert alert is not None, "no ONVIF motion alert was created"
        alert_ms = alert["createdAt"] * 1000

        # The status endpoint must expose the event recorder while it runs.
        active = None
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status, live = call("GET", "/api/v1/recordings/status")
            if status == 200:
                active = next(
                    (
                        item
                        for item in live.get("recordings", [])
                        if item.get("recordType") == "event" and item.get("active")
                    ),
                    None,
                )
                if active:
                    assert active["volumePath"] == str(volume), active
                    break
            time.sleep(0.25)
        assert active is not None, "the event recorder never appeared in the status endpoint"

        # The clip finalizes after the post-roll and is indexed as type event.
        recording = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status, rows = call(
                "GET", f"/api/v1/recordings?channelId={channel_id}&stream=main"
            )
            if status == 200:
                recording = next(
                    (item for item in rows if item.get("recordType") == "event"), None
                )
                if recording:
                    break
            time.sleep(0.5)
        assert recording is not None, "no event recording was indexed"
        assert recording["sizeBytes"] > 0, recording
        assert recording["durationMs"] >= 4000, recording
        # Pre-roll: the clip starts before the alert was raised.
        assert recording["startMs"] <= alert_ms - 1000, (recording, alert)
        print(
            "event recording:",
            json.dumps(
                {
                    "startMs": recording["startMs"],
                    "durationMs": recording["durationMs"],
                    "sizeBytes": recording["sizeBytes"],
                    "alertMs": alert_ms,
                }
            ),
        )
        print("event recording integration passed")
        return 0
    finally:
        mock.shutdown()
        if app is not None:
            app.send_signal(signal.SIGTERM)
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
        if publisher is not None:
            publisher.send_signal(signal.SIGTERM)
            try:
                publisher.wait(timeout=5)
            except subprocess.TimeoutExpired:
                publisher.kill()
        mediamtx.send_signal(signal.SIGTERM)
        try:
            mediamtx.wait(timeout=5)
        except subprocess.TimeoutExpired:
            mediamtx.kill()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
