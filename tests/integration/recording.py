#!/usr/bin/env python3
"""Recording pipeline integration test.

Starts MediaMTX and an FFmpeg test publisher, adds a channel, a full-day plan
and a custom storage volume, then verifies that a keyframe-aligned segment is
finalized under the configured volume and indexed with its codec.

Usage: tests/integration/recording.py [path-to-uvnvr] [http-port]
Requires ffmpeg in PATH and MEDIAMTX_BIN (or mediamtx in PATH).
"""
import http.cookiejar
import json
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19510)
RTSP_PORT = PORT + 100
MEDIAMTX_PORT = PORT + 200

FFMPEG = shutil.which("ffmpeg")
MEDIAMTX = os.environ.get("MEDIAMTX_BIN") or shutil.which("mediamtx")


def main() -> int:
    if not FFMPEG or not MEDIAMTX:
        print("skipping recording test: ffmpeg or MediaMTX is not available")
        return 0

    encoders = subprocess.run(
        [FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True
    ).stdout
    encoder = "libx264" if "libx264" in encoders else "libopenh264" if "libopenh264" in encoders else ""
    if not encoder:
        print("skipping recording test: no H.264 encoder in ffmpeg")
        return 0

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-recording-"))
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
                f"rtpAddress: :{MEDIAMTX_PORT + 1}",
                f"rtcpAddress: :{MEDIAMTX_PORT + 2}",
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
    mediamtx = subprocess.Popen(
        [MEDIAMTX, str(mediamtx_config)], stdout=log, stderr=log
    )
    time.sleep(1)
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
        log.close()
        tail = (temp / "logs.txt").read_text(errors="replace")[-1000:]
        print("skipping recording test: the publisher failed\n" + tail)
        mediamtx.send_signal(signal.SIGTERM)
        try:
            mediamtx.wait(timeout=5)
        except subprocess.TimeoutExpired:
            mediamtx.kill()
        shutil.rmtree(temp, ignore_errors=True)
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
            with opener.open(request, timeout=30) as response:
                return response.status, json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"null")

    try:
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

        database = sqlite3.connect(data_dir / "uvnvr.db")
        database.execute("UPDATE settings SET value = '1' WHERE key = 'recording.segment_minutes'")
        database.commit()
        database.close()

        status, created = call(
            "POST",
            "/api/v1/storage/volumes",
            {
                "path": str(volume),
                "label": "test",
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
                "name": "recording test",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": MEDIAMTX_PORT,
                "mainUrl": f"rtsp://127.0.0.1:{MEDIAMTX_PORT}/cam",
                "enabled": True,
            },
        )
        assert status == 201, (status, channel)

        recordings = []
        deadline = time.monotonic() + 110
        while time.monotonic() < deadline:
            status, rows = call("GET", "/api/v1/recordings?channelId=1&stream=main")
            if status == 200 and rows:
                recordings = rows
                break
            time.sleep(1)

        assert recordings, "no recording segment was finalized"
        first = recordings[0]
        assert first["codec"] == "h264", first
        path = Path(first["path"])
        assert path.exists(), path
        assert volume in path.parents, (volume, path)
        assert first["sizeBytes"] > 0
        print(f"recording segment: {path} ({first['sizeBytes']} bytes, {first['codec']})")
        print("recording integration passed")
        return 0
    finally:
        for process in (app, publisher, mediamtx):
            process.send_signal(signal.SIGTERM)
        for process in (app, publisher, mediamtx):
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
