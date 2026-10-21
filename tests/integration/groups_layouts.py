#!/usr/bin/env python3
"""Channel-group and personal-layout integration test.

Usage: tests/integration/groups_layouts.py [path-to-uvnvr] [http-port]
"""
import http.cookiejar
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 18400)
RTSP_PORT = PORT + 255


def main() -> int:
    temp = Path(tempfile.mkdtemp(prefix="uvnvr-groups-"))
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
    process = subprocess.Popen(
        [str(BIN), "--config", str(config)],
        stdout=log,
        stderr=log,
        env=os.environ.copy(),
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
            with opener.open(request, timeout=15) as response:
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

        status, parent = call("POST", "/api/v1/channel-groups", {"name": "Building A"})
        assert status == 201, (status, parent)
        parent_id = parent["id"]

        status, child = call(
            "POST", "/api/v1/channel-groups", {"name": "Floor 1", "parentId": parent_id}
        )
        assert status == 201, (status, child)
        child_id = child["id"]

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "front door",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": 554,
                "mainUrl": "rtsp://127.0.0.1:554/front",
                "enabled": False,
                "groupId": child_id,
            },
        )
        assert status == 201, (status, channel)
        channel_id = channel["id"]
        assert channel["groupId"] == child_id

        status, channels = call("GET", "/api/v1/channels")
        assert status == 200
        assert next(item for item in channels if item["id"] == channel_id)["groupId"] == child_id

        status, groups = call("GET", "/api/v1/channel-groups")
        assert status == 200
        counts = {item["id"]: item["channels"] for item in groups}
        assert counts[child_id] == 1, counts

        # Moving a parent under its own child must be rejected.
        status, _ = call("PUT", f"/api/v1/channel-groups/{parent_id}", {"parentId": child_id})
        assert status == 400, status

        status, moved = call(
            "PUT", f"/api/v1/channels/{channel_id}", {"groupId": parent_id, "enabled": False}
        )
        assert status == 200, (status, moved)
        assert moved["groupId"] == parent_id

        status, _ = call("DELETE", f"/api/v1/channel-groups/{child_id}")
        assert status == 200

        # Layouts are per user and exactly one can be the default.
        grid = {"grid": "4", "cols": 2, "rows": 2, "cells": [None, None, None, None]}
        status, first = call("POST", "/api/v1/layouts", {"name": "Desk", "grid": grid})
        assert status == 201, (status, first)
        assert first["isDefault"] is True
        status, second = call(
            "POST", "/api/v1/layouts", {"name": "Wall", "grid": grid, "isDefault": True}
        )
        assert status == 201, (status, second)
        status, layouts = call("GET", "/api/v1/layouts")
        assert status == 200
        defaults = [item for item in layouts if item["isDefault"]]
        assert len(defaults) == 1 and defaults[0]["id"] == second["id"], layouts

        status, updated = call(
            "PUT", f"/api/v1/layouts/{first['id']}", {"name": "Desk 2", "grid": grid}
        )
        assert status == 200 and updated["name"] == "Desk 2", (status, updated)

        status, _ = call("DELETE", f"/api/v1/layouts/{second['id']}")
        assert status == 200
        status, layouts = call("GET", "/api/v1/layouts")
        assert len(layouts) == 1 and layouts[0]["id"] == first["id"], layouts

        status, _ = call("DELETE", f"/api/v1/channels/{channel_id}")
        assert status == 200
        print("channel groups and layouts integration passed")
        return 0
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
