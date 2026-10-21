#!/usr/bin/env python3
"""ONVIF video encoder configuration integration test.

Usage: tests/integration/onvif_encoder.py [path-to-uvnvr] [http-port]
"""
import http.cookiejar
import http.server
import json
import os
import re
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
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19410)
RTSP_PORT = PORT + 100
MOCK_PORT = PORT + 200
CAMERA_PASSWORD = "CamPass123"

ENVELOPE = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
    'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
    'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
    'xmlns:tt="http://www.onvif.org/ver10/schema">'
    "<s:Body>{}</s:Body></s:Envelope>"
)


class MockEncoderCamera(http.server.BaseHTTPRequestHandler):
    lock = threading.Lock()
    encoding = "H264"
    width = 1920
    height = 1080
    fps = 25
    bitrate = 4096
    quality = 5
    set_requests = 0

    def log_message(self, *_args):
        pass

    def _reply(self, body: str):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/soap+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        if "GetCapabilities" in body:
            self._reply(
                ENVELOPE.format(
                    "<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Media>"
                    f"<tt:XAddr>http://127.0.0.1:{MOCK_PORT}/onvif/media</tt:XAddr>"
                    "</tt:Media></tds:Capabilities></tds:GetCapabilitiesResponse>"
                )
            )
            return
        if "GetVideoEncoderConfigurations" in body:
            with MockEncoderCamera.lock:
                values = (
                    MockEncoderCamera.encoding,
                    MockEncoderCamera.width,
                    MockEncoderCamera.height,
                    MockEncoderCamera.fps,
                    MockEncoderCamera.bitrate,
                    MockEncoderCamera.quality,
                )
            encoding, width, height, fps, bitrate, quality = values
            self._reply(
                ENVELOPE.format(
                    "<trt:GetVideoEncoderConfigurationsResponse>"
                    '<trt:Configurations token="v1">'
                    "<tt:Name>main</tt:Name><tt:UseCount>1</tt:UseCount>"
                    f"<tt:Encoding>{encoding}</tt:Encoding>"
                    f"<tt:Resolution><tt:Width>{width}</tt:Width>"
                    f"<tt:Height>{height}</tt:Height></tt:Resolution>"
                    f"<tt:Quality>{quality}</tt:Quality>"
                    "<tt:RateControl>"
                    f"<tt:FrameRateLimit>{fps}</tt:FrameRateLimit>"
                    f"<tt:BitrateLimit>{bitrate}</tt:BitrateLimit>"
                    "</tt:RateControl>"
                    "</trt:Configurations>"
                    "</trt:GetVideoEncoderConfigurationsResponse>"
                )
            )
            return
        if "SetVideoEncoderConfiguration" in body:
            with MockEncoderCamera.lock:
                MockEncoderCamera.set_requests += 1
                for tag, attr in (
                    ("Encoding", "encoding"),
                    ("Width", "width"),
                    ("Height", "height"),
                    ("FrameRateLimit", "fps"),
                    ("BitrateLimit", "bitrate"),
                    ("Quality", "quality"),
                ):
                    match = re.search(rf"<tt:{tag}>([^<]+)</tt:{tag}>", body)
                    if match:
                        setattr(MockEncoderCamera, attr, match.group(1))
            self._reply(
                ENVELOPE.format("<trt:SetVideoEncoderConfigurationResponse/>")
            )
            return
        self._reply(ENVELOPE.format(""))


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def pick_port(preferred: int) -> int:
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.0.1", preferred))
            return preferred
        except OSError:
            return free_port()


def main() -> int:
    global PORT, RTSP_PORT, MOCK_PORT
    PORT = pick_port(PORT)
    RTSP_PORT = pick_port(PORT + 100)
    MOCK_PORT = pick_port(PORT + 200)

    mock = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockEncoderCamera)
    threading.Thread(target=mock.serve_forever, daemon=True).start()

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-encoder-"))
    data_dir = temp / "data"
    data_dir.mkdir()
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
    log = open(temp / "server.log", "ab")
    app = subprocess.Popen(
        [str(BIN), "--config", str(config)],
        stdout=log,
        stderr=log,
        env=os.environ.copy(),
    )
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/healthz", timeout=1).read()
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("server did not start; see " + str(temp / "server.log"))

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

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "encoder camera",
                "protocol": "onvif",
                "host": "127.0.0.1",
                "port": MOCK_PORT,
                "username": "admin",
                "password": CAMERA_PASSWORD,
                "mainUrl": "rtsp://127.0.0.1:1/none",
                "enabled": False,
            },
        )
        assert status == 201, (status, channel)
        channel_id = channel["id"]

        status, encoder = call("GET", f"/api/v1/channels/{channel_id}/encoder")
        assert status == 200, (status, encoder)
        assert encoder["encoding"] == "H264", encoder
        assert encoder["width"] == 1920 and encoder["height"] == 1080, encoder
        assert encoder["fps"] == 25 and encoder["bitrateKbps"] == 4096, encoder

        status, applied = call(
            "PUT",
            f"/api/v1/channels/{channel_id}/encoder",
            {
                "token": "v1",
                "encoding": "h265",
                "width": 1280,
                "height": 720,
                "fps": 30,
                "bitrateKbps": 2048,
                "quality": 6,
            },
        )
        assert status == 200, (status, applied)
        assert applied["encoding"] == "H265", applied
        assert MockEncoderCamera.set_requests == 1, MockEncoderCamera.set_requests
        assert MockEncoderCamera.encoding == "H265", MockEncoderCamera.encoding
        assert MockEncoderCamera.width == "1280", MockEncoderCamera.width
        assert MockEncoderCamera.fps == "30", MockEncoderCamera.fps
        assert MockEncoderCamera.bitrate == "2048", MockEncoderCamera.bitrate

        status, rows = call("GET", "/api/v1/channels")
        current = next(item for item in rows if item["id"] == channel_id)
        assert current["media"]["encoder"]["encoding"] == "H265", current["media"]

        status, rtsp_channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "rtsp channel",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": 554,
                "mainUrl": "rtsp://127.0.0.1:554/none",
                "enabled": False,
            },
        )
        assert status == 201, (status, rtsp_channel)
        status, rejected = call("GET", f"/api/v1/channels/{rtsp_channel['id']}/encoder")
        assert status == 400, (status, rejected)
        print("ONVIF encoder integration passed")
        return 0
    finally:
        app.send_signal(signal.SIGTERM)
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
        mock.shutdown()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
