#!/usr/bin/env python3
"""Motion settings, audit export and maintenance-mode integration test.

Usage: tests/integration/maintenance.py [path-to-uvnvr] [http-port]
"""
import http.cookiejar
import json
import os
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19910)


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


class Server:
    def __init__(self, maintenance: bool):
        self.port = pick_port(PORT)
        self.rtsp_port = pick_port(self.port + 100)
        self.temp = Path(tempfile.mkdtemp(prefix="uvnvr-maintenance-"))
        self.data_dir = self.temp / "data"
        self.data_dir.mkdir()
        self.volume = self.temp / "volume"
        self.volume.mkdir()
        config = self.temp / "uvnvr.json"
        config.write_text(
            json.dumps(
                {
                    "http": {"port": self.port, "host": "127.0.0.1"},
                    "rtsp": {"port": self.rtsp_port},
                    "dataDir": str(self.data_dir),
                    "maintenance": maintenance,
                    "logging": {"level": "warn"},
                }
            )
        )
        self.log = open(self.temp / "server.log", "ab")
        self.process = subprocess.Popen(
            [str(BIN), "--config", str(config)],
            stdout=self.log,
            stderr=self.log,
            env=os.environ.copy(),
        )
        for _ in range(100):
            try:
                urllib.request.urlopen(
                    f"http://127.0.0.1:{self.port}/healthz", timeout=1
                ).read()
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise RuntimeError("server did not start; see " + str(self.temp / "server.log"))

        jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        self.csrf = ""

    def call(self, method: str, path: str, body: dict | None = None, raw: bool = False):
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=None if body is None else json.dumps(body).encode(),
            method=method,
        )
        if body is not None:
            request.add_header("Content-Type", "application/json")
        if self.csrf and method not in ("GET", "HEAD"):
            request.add_header("X-CSRF-Token", self.csrf)
        try:
            with self.opener.open(request, timeout=15) as response:
                data = response.read()
                if raw:
                    return response.status, data, dict(response.headers)
                return response.status, json.loads(data or b"null")
        except urllib.error.HTTPError as error:
            data = error.read()
            if raw:
                return error.code, data, dict(error.headers)
            return error.code, json.loads(data or b"null")

    def setup(self):
        status, setup = self.call(
            "POST",
            "/api/v1/setup",
            {
                "password": "Admin123!@#",
                "confirmPassword": "Admin123!@#",
                "timezone": "UTC",
            },
        )
        assert status == 200, (status, setup)
        self.csrf = setup["csrfToken"]

    def close(self):
        self.process.send_signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self.log.close()
        shutil.rmtree(self.temp, ignore_errors=True)


def insert_recording(data_dir: Path, volume_id: int, path: Path, start_ms: int) -> int:
    database = sqlite3.connect(data_dir / "uvnvr.db")
    database.execute(
        "INSERT INTO recordings(channel_id, stream, volume_id, path, start_ms, end_ms, "
        "duration_ms, size_bytes, codec, record_type, created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        (1, "main", volume_id, str(path), start_ms, start_ms + 1000, 1000, 3, "h264", "timed", 1),
    )
    database.commit()
    row = database.execute("SELECT last_insert_rowid()").fetchone()[0]
    database.close()
    return row


def main() -> int:
    global PORT
    PORT = pick_port(PORT)
    server = Server(True)
    try:
        server.setup()
        status, health = server.call("GET", "/healthz")
        assert status == 200 and health["maintenance"] is True, health

        status, settings = server.call("GET", "/api/v1/settings")
        assert status == 200 and settings["motion"]["sensitivity"] == 50, settings
        status, saved = server.call(
            "PUT",
            "/api/v1/settings",
            {
                "motion": {
                    "enabled": True,
                    "sensitivity": 80,
                    "cooldownSeconds": 30,
                    "region": {"x": 10, "y": 5, "width": 50, "height": 40},
                }
            },
        )
        assert status == 200, (status, saved)
        assert saved["motion"]["sensitivity"] == 80, saved
        assert saved["motion"]["region"] == {"x": 10, "y": 5, "width": 50, "height": 40}, saved
        status, invalid = server.call(
            "PUT",
            "/api/v1/settings",
            {"motion": {"region": {"x": 80, "y": 0, "width": 50, "height": 100}}},
        )
        assert status == 400, (status, invalid)

        status, created = server.call(
            "POST",
            "/api/v1/storage/volumes",
            {
                "path": str(server.volume),
                "label": "maintenance",
                "mode": "loop",
                "maxUsagePct": 95,
                "reserveGb": 0,
                "retentionDays": 0,
            },
        )
        assert status == 201, (status, created)
        volume_id = created["id"]
        fake_dir = server.volume / "uvnvr" / "1" / "2026" / "10" / "21"
        fake_dir.mkdir(parents=True)
        first = fake_dir / "1_main_1_1.ts"
        second = fake_dir / "1_main_2_2.ts"
        first.write_bytes(b"one")
        second.write_bytes(b"two")
        first_id = insert_recording(server.data_dir, volume_id, first, 1000)
        insert_recording(server.data_dir, volume_id, second, 3000)

        status, summary = server.call("GET", "/api/v1/dashboard/summary")
        assert status == 200 and summary["counts"]["recordings"] == 2, summary["counts"]

        status, deleted = server.call("DELETE", f"/api/v1/recordings/{first_id}")
        assert status == 200, (status, deleted)
        assert not first.exists(), "the recording file was not deleted"
        database = sqlite3.connect(server.data_dir / "uvnvr.db")
        remaining = database.execute(
            "SELECT COUNT(*) FROM recordings WHERE id = ?", (first_id,)
        ).fetchone()[0]
        database.close()
        assert remaining == 0

        status, mismatched = server.call(
            "POST", "/api/v1/storage/format", {"volumeId": volume_id, "confirm": "wrong"}
        )
        assert status == 400, (status, mismatched)
        status, formatted = server.call(
            "POST",
            "/api/v1/storage/format",
            {"volumeId": volume_id, "confirm": str(server.volume)},
        )
        assert status == 200 and formatted["recordings"] == 1, (status, formatted)
        assert not (server.volume / "uvnvr").exists(), "recordings were not removed"

        status, csv, headers = server.call("GET", "/api/v1/audit/export", raw=True)
        assert status == 200, status
        text = csv.decode("utf-8-sig")
        assert text.startswith("id,ts,category,level,actor"), text[:80]
        assert "recording" in text and "format" in text, text
        assert "text/csv" in headers.get("Content-Type", ""), headers

        status, serial = server.call("PUT", "/api/v1/system/serial", {"serial": "LAB-0001"})
        assert status == 200 and serial["serial"] == "LAB-0001", (status, serial)
        status, overview = server.call("GET", "/api/v1/system/overview")
        assert overview["serial"] == "LAB-0001", overview["serial"]
        status, version = server.call("PUT", "/api/v1/system/version", {"version": "9.9.9-lab"})
        assert status == 200 and version["version"] == "9.9.9-lab", (status, version)
        assert version["buildVersion"] != "9.9.9-lab", version
        status, overview = server.call("GET", "/api/v1/system/overview")
        assert overview["version"] == "9.9.9-lab", overview["version"]
        status, health = server.call("GET", "/healthz")
        assert health["version"] == version["buildVersion"], health
        status, version = server.call("PUT", "/api/v1/system/version", {"version": ""})
        assert status == 200 and version["version"] == version["buildVersion"], version

        status, purged = server.call("POST", "/api/v1/audit/purge", {"confirm": "purge"})
        assert status == 200 and purged["deleted"] >= 1, (status, purged)
        status, logs = server.call("GET", "/api/v1/audit")
        assert logs["total"] == 1, logs  # only the purge entry remains
        print("maintenance integration passed")
    finally:
        server.close()

    locked = Server(False)
    try:
        locked.setup()
        status, _ = locked.call("DELETE", "/api/v1/recordings/1")
        assert status == 403, status
        status, _ = locked.call("POST", "/api/v1/audit/purge", {"confirm": "purge"})
        assert status == 403, status
        status, _ = locked.call("PUT", "/api/v1/system/serial", {"serial": "X-1"})
        assert status == 403, status
        status, _ = locked.call("PUT", "/api/v1/system/version", {"version": "1"})
        assert status == 403, status
        status, _ = locked.call("POST", "/api/v1/storage/format", {"volumeId": 1, "confirm": ""})
        assert status == 403, status
        print("maintenance gate passed")
    finally:
        locked.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
