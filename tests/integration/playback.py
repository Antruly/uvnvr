#!/usr/bin/env python3
"""Playback integration test for 0.2.0.

Inserts a synthetic keyframe-aligned MPEG-TS segment and drives the playback
WebSocket through the API. Verifies server-side pacing at 8x and keyframe-based
seeking to the middle of a segment.

Usage: tests/integration/playback.py [path-to-uvnvr] [http-port]
Requires the Python `websockets` package.
"""
import asyncio
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
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 18300)
RTSP_PORT = PORT + 255


def build_ts(path: Path, packets: int = 22369) -> None:
    """Write a TS file with an IDR NAL every 50 packets."""
    data = bytearray()
    for index in range(packets):
        packet = bytearray(188)
        packet[0] = 0x47
        packet[1] = 0x41  # payload unit start
        packet[2] = 0x00
        packet[3] = 0x10  # payload only
        nal = b"\x00\x00\x00\x01" + (b"\x65" if index % 50 == 0 else b"\x41")
        packet[4 : 4 + len(nal)] = nal
        data += packet
    path.write_bytes(bytes(data))


def main() -> int:
    temp = Path(tempfile.mkdtemp(prefix="uvnvr-playback-"))
    data_dir = temp / "data"
    data_dir.mkdir()
    config = temp / "uvnvr.json"
    config.write_text(
        json.dumps(
            {
                "http": {"port": PORT, "host": "127.0.0.1"},
                "rtsp": {"port": RTSP_PORT},
                "dataDir": str(data_dir),
            }
        )
    )
    log = open(temp / "server.log", "ab")

    def start() -> subprocess.Popen:
        process = subprocess.Popen(
            [str(BIN), "--config", str(config)],
            stdout=log,
            stderr=log,
            env=os.environ.copy(),
        )
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/healthz", timeout=1).read()
                return process
            except Exception:
                time.sleep(0.2)
        raise RuntimeError("server did not start; see " + str(temp / "server.log"))

    def stop(process: subprocess.Popen) -> None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()

    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    def post(path: str, body: dict, csrf: str | None = None) -> dict:
        request = urllib.request.Request(
            f"http://127.0.0.1:{PORT}{path}",
            data=json.dumps(body).encode(),
            method="POST",
        )
        request.add_header("Content-Type", "application/json")
        if csrf:
            request.add_header("X-CSRF-Token", csrf)
        return json.loads(opener.open(request, timeout=10).read())

    process = start()
    setup = post(
        "/api/v1/setup",
        {
            "password": "Admin123!@#",
            "confirmPassword": "Admin123!@#",
            "timezone": "UTC",
        },
    )
    csrf = setup["csrfToken"]
    stop(process)

    segment = data_dir / "rec.ts"
    build_ts(segment)
    size = segment.stat().st_size
    now_ms = int(time.time() * 1000)
    start_ms = now_ms - 60_000
    connection = sqlite3.connect(data_dir / "uvnvr.db")
    connection.execute(
        "INSERT INTO channels(id,name,protocol,host,port,enabled,created_at,updated_at) "
        "VALUES(1,'test','rtsp','127.0.0.1',554,1,?,?)",
        (now_ms // 1000, now_ms // 1000),
    )
    connection.execute(
        "INSERT INTO recordings(channel_id,stream,volume_id,path,start_ms,end_ms,duration_ms,"
        "size_bytes,codec,record_type,created_at) VALUES(1,'main',1,?,?,?,60000,?,'','timed',?)",
        (str(segment), start_ms, now_ms, size, now_ms // 1000),
    )
    connection.commit()
    connection.close()

    process = start()
    login = post("/api/v1/auth/login", {"username": "admin", "password": "Admin123!@#"})
    csrf = login["csrfToken"]

    async def stream(start_at: int, speed: float) -> tuple[int, float]:
        import websockets

        ticket = post(
            "/api/v1/playback/ticket",
            {
                "channelId": 1,
                "stream": "main",
                "startMs": start_at,
                "endMs": now_ms,
                "speed": speed,
            },
            csrf,
        )
        total = 0
        began = time.monotonic()
        async with websockets.connect(
            f"ws://127.0.0.1:{PORT}{ticket['wsPath']}", max_size=None
        ) as socket:
            while True:
                try:
                    message = await asyncio.wait_for(socket.recv(), timeout=20)
                except Exception:
                    break
                if isinstance(message, bytes):
                    total += len(message)
        return total, time.monotonic() - began

    try:
        total, elapsed = asyncio.run(stream(start_ms, 8.0))
        print(f"full segment at 8x: {total} bytes in {elapsed:.2f}s")
        assert total == size, (total, size)
        assert 5.0 <= elapsed <= 12.0, elapsed

        total, elapsed = asyncio.run(stream(start_ms + 30_000, 8.0))
        print(f"seek to +30s at 8x: {total} bytes in {elapsed:.2f}s")
        assert 0 < total < size, total
        assert 2.0 <= elapsed <= 8.0, elapsed
        print("playback integration passed")
        return 0
    finally:
        stop(process)
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
