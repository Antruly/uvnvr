#!/usr/bin/env python3
"""Playback of the segment that is still being recorded.

Usage: tests/integration/active_playback.py [path-to-uvnvr] [http-port]
Requires ffmpeg in PATH and MEDIAMTX_BIN (or mediamtx in PATH).
"""
import asyncio
import http.cookiejar
import json
import os
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

import websockets

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19110)
RTSP_PORT = PORT + 100
MEDIAMTX_PORT = PORT + 200

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


def main() -> int:
    global PORT, RTSP_PORT, MEDIAMTX_PORT
    if not FFMPEG or not MEDIAMTX:
        print("skipping active playback test: ffmpeg or MediaMTX is not available")
        return 0
    encoders = subprocess.run(
        [FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True
    ).stdout
    encoder = "libx264" if "libx264" in encoders else "libopenh264" if "libopenh264" in encoders else ""
    if not encoder:
        print("skipping active playback test: no H.264 encoder in ffmpeg")
        return 0

    PORT = pick_port(PORT)
    RTSP_PORT = pick_port(PORT + 100)
    MEDIAMTX_PORT = pick_port(PORT + 200)
    rtp_port = pick_port(MEDIAMTX_PORT + 1)
    rtcp_port = pick_port(MEDIAMTX_PORT + 2)

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-active-playback-"))
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
    publisher = None
    app = None
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", MEDIAMTX_PORT), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            print("skipping active playback test: MediaMTX did not listen")
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
            print("skipping active playback test: the publisher failed")
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
            {"password": "Admin123!@#", "confirmPassword": "Admin123!@#", "timezone": "UTC"},
        )
        assert status == 200, (status, setup)
        csrf = setup["csrfToken"]

        status, created = call(
            "POST",
            "/api/v1/storage/volumes",
            {
                "path": str(volume),
                "label": "active",
                "mode": "loop",
                "maxUsagePct": 95,
                "reserveGb": 0,
                "retentionDays": 0,
            },
        )
        assert status == 201, (status, created)

        weekday = (time.localtime().tm_wday + 1) % 7
        status, result = call(
            "PUT",
            "/api/v1/plans",
            {
                "channelId": 1,
                "plans": [
                    {
                        "weekday": weekday,
                        "startMin": 0,
                        "endMin": 1440,
                        "mode": "timed",
                        "stream": "main",
                    }
                ],
            },
        )
        assert status == 200, (status, result)

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "active playback",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": MEDIAMTX_PORT,
                "mainUrl": f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam",
                "enabled": True,
            },
        )
        assert status == 201, (status, channel)
        channel_id = channel["id"]

        active = None
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            status, live = call("GET", "/api/v1/recordings/status")
            if status == 200:
                active = next(
                    (
                        item
                        for item in live.get("recordings", [])
                        if item.get("channelId") == channel_id and item.get("active")
                    ),
                    None,
                )
                if active:
                    break
            time.sleep(0.5)
        assert active is not None, "no active recorder appeared"
        assert active["partPath"].endswith(".ts.part"), active

        now_ms = int(time.time() * 1000)
        status, rows = call(
            "GET",
            f"/api/v1/recordings?channelId={channel_id}&stream=main"
            f"&from={now_ms - 3600_000}&to={now_ms + 3600_000}",
        )
        assert status == 200, (status, rows)
        live_row = next((item for item in rows if item.get("active")), None)
        assert live_row is not None, rows
        assert live_row["path"].endswith(".ts.part"), live_row

        status, ticket = call(
            "POST",
            "/api/v1/playback/ticket",
            {
                "channelId": channel_id,
                "stream": "main",
                "startMs": live_row["startMs"],
                "endMs": now_ms + 15000,
                "speed": 1,
            },
        )
        assert status == 200, (status, ticket)

        async def receive_stream():
            received = 0
            url = f"ws://127.0.0.1:{PORT}{ticket['wsPath']}"
            async with websockets.connect(url, max_size=None) as websocket:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline and received < 200_000:
                    try:
                        message = await asyncio.wait_for(websocket.recv(), timeout=5)
                    except asyncio.TimeoutError:
                        break
                    if isinstance(message, bytes):
                        received += len(message)
            return received

        received = asyncio.run(receive_stream())
        assert received > 10_000, received
        print(f"active playback: {received} bytes streamed from {active['partPath']}")
        print("active playback integration passed")
        return 0
    finally:
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
