#!/usr/bin/env python3
"""RTSP output integration test.

Publishes a test stream through MediaMTX, adds it to UvNVR, then probes the
RTSP output with ffprobe over TCP and UDP. The output must advertise H.264 RTP
(payload type 96) rather than MPEG-TS.

Usage: tests/integration/rtsp_output.py [path-to-uvnvr] [http-port]
Requires ffmpeg/ffprobe in PATH and MEDIAMTX_BIN (or mediamtx in PATH).
"""
import base64
import hashlib
import http.cookiejar
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19710)
RTSP_PORT = PORT + 100
MEDIAMTX_PORT = PORT + 200
PASSWORD = "Admin12345"

FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")
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


def rtsp_options(headers: dict[str, str]) -> str:
    url = f"rtsp://127.0.0.1:{RTSP_PORT}/live/1/main"
    lines = [f"OPTIONS {url} RTSP/1.0", "CSeq: 1", "User-Agent: uvnvr-test"]
    for name, value in headers.items():
        lines.append(f"{name}: {value}")
    with socket.create_connection(("127.0.0.1", RTSP_PORT), timeout=5) as connection:
        connection.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = connection.recv(4096)
            if not chunk:
                break
            data += chunk
    return data.decode("utf-8", "replace")


def check_rtsp_auth():
    challenge = rtsp_options({})
    assert "401" in challenge.split("\r\n", 1)[0], challenge
    assert 'WWW-Authenticate: Digest' in challenge, challenge
    digest = re.search(r'Digest realm="([^"]+)", qop="auth", nonce="([^"]+)"', challenge)
    assert digest, challenge
    realm, nonce = digest.group(1), digest.group(2)
    url = f"rtsp://127.0.0.1:{RTSP_PORT}/live/1/main"
    ha1 = hashlib.md5(f"admin:{realm}:{PASSWORD}".encode()).hexdigest()
    ha2 = hashlib.md5(f"OPTIONS:{url}".encode()).hexdigest()
    nc = "00000001"
    cnonce = "0a4f113b"
    response = hashlib.md5(
        f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}".encode()
    ).hexdigest()
    authorization = (
        f'Digest username="admin", realm="{realm}", nonce="{nonce}", uri="{url}", '
        f'response="{response}", qop=auth, nc={nc}, cnonce="{cnonce}"'
    )
    digest_reply = rtsp_options({"Authorization": authorization})
    assert digest_reply.startswith("RTSP/1.0 200"), digest_reply
    basic = base64.b64encode(f"admin:{PASSWORD}".encode()).decode()
    basic_reply = rtsp_options({"Authorization": f"Basic {basic}"})
    assert basic_reply.startswith("RTSP/1.0 200"), basic_reply
    print("RTSP Digest and Basic authentication passed")


def probe_stream(transport: str, channel_id: int = 1, deadline: float = 20):
    url = f"rtsp://admin:{PASSWORD}@127.0.0.1:{RTSP_PORT}/live/{channel_id}/main"
    result = subprocess.run(
        [
            FFPROBE,
            "-v",
            "error",
            "-rtsp_transport",
            transport,
            "-show_streams",
            "-show_entries",
            "stream=codec_name,codec_type,width,height",
            "-of",
            "json",
            "-rw_timeout",
            "5000000",
            url,
        ],
        capture_output=True,
        text=True,
        timeout=deadline,
    )
    if result.returncode != 0:
        return None, result.stderr.strip()
    return json.loads(result.stdout), ""


def main() -> int:
    global PORT, RTSP_PORT, MEDIAMTX_PORT
    if not FFMPEG or not FFPROBE or not MEDIAMTX:
        print("skipping RTSP output test: ffmpeg, ffprobe or MediaMTX is not available")
        return 0
    encoders = subprocess.run(
        [FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True
    ).stdout
    encoder = "libx264" if "libx264" in encoders else "libopenh264" if "libopenh264" in encoders else ""
    if not encoder:
        print("skipping RTSP output test: no H.264 encoder in ffmpeg")
        return 0
    encoder265 = "libkvazaar" if "libkvazaar" in encoders else ""

    PORT = pick_port(PORT)
    RTSP_PORT = pick_port(PORT + 100)
    MEDIAMTX_PORT = pick_port(PORT + 200)
    rtp_port = pick_port(MEDIAMTX_PORT + 1)
    rtcp_port = pick_port(MEDIAMTX_PORT + 2)

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-rtsp-"))
    data_dir = temp / "data"
    data_dir.mkdir()
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
    publisher = None
    publisher265 = None
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
            print("skipping RTSP output test: MediaMTX did not listen")
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
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:sample_rate=48000",
                "-c:v",
                encoder,
                "-g",
                "30",
                "-pix_fmt",
                "yuv420p",
                "-c:a",
                "aac",
                "-b:a",
                "64k",
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
            print("skipping RTSP output test: the publisher failed")
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
            {"password": PASSWORD, "confirmPassword": PASSWORD, "timezone": "UTC"},
        )
        assert status == 200, (status, setup)
        csrf = setup["csrfToken"]

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "rtsp output",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": MEDIAMTX_PORT,
                "mainUrl": f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam",
                "enabled": True,
            },
        )
        assert status == 201, (status, channel)
        channel_id = channel["id"]

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status, rows = call("GET", "/api/v1/channels")
            current = next((item for item in rows if item["id"] == channel_id), None)
            if current and current.get("lastStatus") == "online":
                break
            time.sleep(0.5)
        else:
            raise AssertionError("the test channel never came online")

        check_rtsp_auth()

        for transport in ("tcp", "udp"):
            stream, error = probe_stream(transport)
            assert stream is not None, (transport, error)
            video = next(
                (item for item in stream["streams"] if item["codec_type"] == "video"), None
            )
            assert video is not None, stream
            assert video["codec_name"] == "h264", video
            assert video.get("width") == 320 and video.get("height") == 240, video
            audio = next(
                (item for item in stream["streams"] if item["codec_type"] == "audio"), None
            )
            assert audio is not None and audio["codec_name"] == "aac", stream["streams"]
            print(f"RTSP {transport}: video={video['codec_name']} audio={audio['codec_name']}")

        if encoder265:
            publisher265 = subprocess.Popen(
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
                    encoder265,
                    "-g",
                    "30",
                    "-pix_fmt",
                    "yuv420p",
                    "-f",
                    "rtsp",
                    "-rtsp_transport",
                    "tcp",
                    f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam265",
                ],
                stdout=log,
                stderr=log,
            )
            time.sleep(2)
            status, channel265 = call(
                "POST",
                "/api/v1/channels",
                {
                    "name": "rtsp output h265",
                    "protocol": "rtsp",
                    "host": "127.0.0.1",
                    "port": MEDIAMTX_PORT,
                    "mainUrl": f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam265",
                    "enabled": True,
                },
            )
            assert status == 201, (status, channel265)
            channel265_id = channel265["id"]
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                status, rows = call("GET", "/api/v1/channels")
                current = next(
                    (item for item in rows if item["id"] == channel265_id), None
                )
                if current and current.get("lastStatus") == "online":
                    break
                time.sleep(0.5)
            else:
                raise AssertionError("the H.265 test channel never came online")
            stream, error = probe_stream("tcp", channel265_id)
            assert stream is not None, error
            video = next(
                (item for item in stream["streams"] if item["codec_type"] == "video"), None
            )
            assert video is not None and video["codec_name"] == "hevc", video
            print(f"RTSP h265: {video}")
        else:
            print("skipping H.265 output: no libkvazaar encoder")

        print("RTSP output integration passed")
        return 0
    finally:
        if app is not None:
            app.send_signal(signal.SIGTERM)
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
        if publisher265 is not None:
            publisher265.send_signal(signal.SIGTERM)
            try:
                publisher265.wait(timeout=5)
            except subprocess.TimeoutExpired:
                publisher265.kill()
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
