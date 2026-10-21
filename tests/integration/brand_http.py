#!/usr/bin/env python3
"""Hikvision ISAPI / Dahua CGI HTTP Digest integration test.

Usage: tests/integration/brand_http.py [path-to-uvnvr] [http-port]
"""
import hashlib
import http.cookiejar
import http.server
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19810)
RTSP_PORT = PORT + 100
MOCK_PORT = PORT + 200
USERNAME = "admin"
PASSWORD = "Camera123!"
REALM = "testrealm"
NONCE = "abcdef0123456789"
OPAQUE = "opaque-token"


def md5(value: str) -> str:
    return hashlib.md5(value.encode()).hexdigest()


class MockCamera(http.server.BaseHTTPRequestHandler):
    time_updates = 0
    requests: list[tuple[str, str]] = []
    lock = threading.Lock()
    encoding = "H.264"
    width = 1920
    height = 1080
    fps = 25
    bitrate = 4096
    body = ""

    def log_message(self, *_args):
        pass

    def digest_params(self, header: str) -> dict[str, str]:
        params: dict[str, str] = {}
        for key, quoted, bare in re.findall(r'(\w+)=(?:"([^"]*)"|([^,]+))', header):
            params[key] = quoted if quoted else bare.strip()
        return params

    def authorized(self) -> bool:
        header = self.headers.get("Authorization", "")
        if not header.startswith("Digest "):
            return False
        params = self.digest_params(header)
        if params.get("username") != USERNAME or params.get("nonce") != NONCE:
            return False
        ha1 = md5(f"{USERNAME}:{REALM}:{PASSWORD}")
        ha2 = md5(f"{self.command}:{params.get('uri', '')}")
        expected = md5(
            f"{ha1}:{NONCE}:{params.get('nc', '')}:{params.get('cnonce', '')}:auth:{ha2}"
        )
        return params.get("response") == expected

    def challenge(self):
        data = b"unauthorized"
        self.send_response(401)
        self.send_header(
            "WWW-Authenticate",
            f'Digest realm="{REALM}", nonce="{NONCE}", qop="auth", opaque="{OPAQUE}"',
        )
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def reply(self, body: str, status: int = 200):
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_request(self):
        if not self.authorized():
            self.challenge()
            return
        with MockCamera.lock:
            MockCamera.requests.append((self.command, self.path))
        if self.path == "/ISAPI/System/deviceInfo":
            self.reply(
                "<DeviceInfo><deviceName>Hikvision Front</deviceName>"
                "<model>DS-2CD2143</model><serialNumber>HK123456</serialNumber>"
                "<firmwareVersion>5.7.3</firmwareVersion></DeviceInfo>"
            )
        elif self.path == "/ISAPI/Streaming/channels/101":
            with MockCamera.lock:
                values = (
                    MockCamera.encoding,
                    MockCamera.width,
                    MockCamera.height,
                    MockCamera.fps,
                    MockCamera.bitrate,
                )
            if self.command == "PUT":
                for tag, attr in (
                    ("videoCodecType", "encoding"),
                    ("videoResolutionWidth", "width"),
                    ("videoResolutionHeight", "height"),
                    ("videoFrameRate", "fps"),
                    ("vbrUpperCap", "bitrate"),
                ):
                    match = re.search(rf"<{tag}>([^<]+)</{tag}>", self.body)
                    if match:
                        value = match.group(1)
                        if attr == "fps":
                            setattr(MockCamera, attr, int(value) // 100)
                        elif value.isdigit():
                            setattr(MockCamera, attr, int(value))
                        else:
                            setattr(MockCamera, attr, value)
                self.reply("<ResponseStatus><statusCode>200</statusCode></ResponseStatus>")
            else:
                encoding, width, height, fps, bitrate = values
                self.reply(
                    '<StreamingChannel version="2.0">'
                    "<Video>"
                    f"<videoCodecType>{encoding}</videoCodecType>"
                    f"<videoResolutionWidth>{width}</videoResolutionWidth>"
                    f"<videoResolutionHeight>{height}</videoResolutionHeight>"
                    f"<videoFrameRate>{fps * 100}</videoFrameRate>"
                    f"<maxFrameRate>{fps * 100}</maxFrameRate>"
                    f"<vbrUpperCap>{bitrate}</vbrUpperCap>"
                    "</Video></StreamingChannel>"
                )
        elif self.path.startswith("/cgi-bin/configManager.cgi?action=getConfig"):
            with MockCamera.lock:
                values = (
                    MockCamera.encoding,
                    MockCamera.width,
                    MockCamera.height,
                    MockCamera.fps,
                    MockCamera.bitrate,
                )
            encoding, width, height, fps, bitrate = values
            self.reply(
                "table.Encode[0].MainFormat[0].Video.Compression="
                f"{encoding}\n"
                f"table.Encode[0].MainFormat[0].Video.Width={width}\n"
                f"table.Encode[0].MainFormat[0].Video.Height={height}\n"
                f"table.Encode[0].MainFormat[0].Video.FPS={fps}\n"
                f"table.Encode[0].MainFormat[0].Video.BitRate={bitrate}\n"
            )
        elif self.path.startswith("/cgi-bin/configManager.cgi?action=setConfig"):
            for key, attr in (
                ("Compression", "encoding"),
                ("Width", "width"),
                ("Height", "height"),
                ("FPS", "fps"),
                ("BitRate", "bitrate"),
            ):
                match = re.search(rf"Video\.{key}=([^&]+)", self.path)
                if match:
                    value = urllib.parse.unquote(match.group(1))
                    setattr(
                        MockCamera,
                        attr,
                        int(value) if value.isdigit() else value,
                    )
            self.reply("OK")
        elif self.path == "/ISAPI/System/time" and self.command == "PUT":
            with MockCamera.lock:
                MockCamera.time_updates += 1
            self.reply("<ResponseStatus><statusCode>200</statusCode></ResponseStatus>")
        elif self.path.startswith("/cgi-bin/magicBox.cgi?action=getDeviceType"):
            self.reply("type=IPC-HDW2231\n")
        elif self.path.startswith("/cgi-bin/magicBox.cgi?action=getMachineName"):
            self.reply("name=Dahua Door\n")
        elif self.path.startswith("/cgi-bin/magicBox.cgi?action=getSoftwareVersion"):
            self.reply("version=2.800.1\nserialNumber=DH9876\n")
        elif self.path.startswith("/cgi-bin/global.cgi?action=setCurrentTime"):
            with MockCamera.lock:
                MockCamera.time_updates += 1
            self.reply("OK")
        else:
            self.reply("not found", 404)

    def do_GET(self):
        self.body = ""
        self.handle_request()

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", 0))
        self.body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        self.handle_request()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        self.handle_request()


def main() -> int:
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockCamera)
    threading.Thread(target=mock.serve_forever, daemon=True).start()

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-brand-"))
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
    process = subprocess.Popen(
        [str(BIN), "--config", str(config)], stdout=log, stderr=log, env=os.environ.copy()
    )
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
            with opener.open(request, timeout=30) as response:
                return response.status, json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"null")

    try:
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

        status, detected = call(
            "POST",
            "/api/v1/channels/detect",
            {
                "host": "127.0.0.1",
                "httpPort": MOCK_PORT,
                "username": USERNAME,
                "password": PASSWORD,
                "brand": "hikvision",
            },
        )
        assert status == 200, (status, detected)
        assert detected["protocol"] == "hikvision", detected
        assert detected["name"] == "Hikvision Front", detected
        assert detected["model"] == "DS-2CD2143", detected
        print(f"hikvision detect: {detected}")

        status, detected = call(
            "POST",
            "/api/v1/channels/detect",
            {
                "host": "127.0.0.1",
                "httpPort": MOCK_PORT,
                "username": USERNAME,
                "password": PASSWORD,
                "brand": "dahua",
            },
        )
        assert status == 200, (status, detected)
        assert detected["protocol"] == "dahua", detected
        assert detected["name"] == "Dahua Door", detected
        print(f"dahua detect: {detected}")

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": detected["name"],
                "protocol": "dahua",
                "host": "127.0.0.1",
                "port": 1,
                "httpPort": MOCK_PORT,
                "username": USERNAME,
                "password": PASSWORD,
                "enabled": True,
            },
        )
        assert status == 201, (status, channel)
        dahua_channel_id = channel["id"]

        status, hik_channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "Hikvision encoder",
                "protocol": "hikvision",
                "host": "127.0.0.1",
                "port": 1,
                "httpPort": MOCK_PORT,
                "username": USERNAME,
                "password": PASSWORD,
                "enabled": False,
            },
        )
        assert status == 201, (status, hik_channel)
        hik_channel_id = hik_channel["id"]

        MockCamera.encoding = "H.264"
        MockCamera.width = 1920
        MockCamera.height = 1080
        MockCamera.fps = 25
        MockCamera.bitrate = 4096
        status, encoder = call("GET", f"/api/v1/channels/{hik_channel_id}/encoder")
        assert status == 200, (status, encoder)
        assert encoder["encoding"] == "H264" and encoder["width"] == 1920, encoder
        status, applied = call(
            "PUT",
            f"/api/v1/channels/{hik_channel_id}/encoder",
            {
                "encoding": "h265",
                "width": 1280,
                "height": 720,
                "fps": 30,
                "bitrateKbps": 2048,
            },
        )
        assert status == 200, (status, applied)
        assert MockCamera.encoding == "H.265", MockCamera.encoding
        assert MockCamera.width == 1280 and MockCamera.fps == 30, MockCamera.width
        assert MockCamera.bitrate == 2048, MockCamera.bitrate

        MockCamera.encoding = "H.264"
        MockCamera.width = 1920
        MockCamera.height = 1080
        MockCamera.fps = 25
        MockCamera.bitrate = 4096
        status, encoder = call("GET", f"/api/v1/channels/{dahua_channel_id}/encoder")
        assert status == 200, (status, encoder)
        assert encoder["encoding"] == "H264" and encoder["height"] == 1080, encoder
        status, applied = call(
            "PUT",
            f"/api/v1/channels/{dahua_channel_id}/encoder",
            {
                "encoding": "h265",
                "width": 1280,
                "height": 720,
                "fps": 30,
                "bitrateKbps": 2048,
            },
        )
        assert status == 200, (status, applied)
        assert MockCamera.encoding == "H.265", MockCamera.encoding
        assert MockCamera.width == 1280 and MockCamera.fps == 30, MockCamera.width
        print("brand encoder read/apply passed")

        status, synced = call(
            "POST", "/api/v1/system/time/sync", {"epochMs": int(time.time() * 1000)}
        )
        assert status == 200, (status, synced)
        assert synced["cameras"]["synced"] >= 1, synced
        assert MockCamera.time_updates >= 1, MockCamera.time_updates
        print(f"brand time sync: {synced['cameras']}")
        print("brand HTTP integration passed")
        return 0
    finally:
        mock.shutdown()
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
