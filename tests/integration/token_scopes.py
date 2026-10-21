#!/usr/bin/env python3
"""API token scope integration test.

Usage: tests/integration/token_scopes.py [path-to-uvnvr] [http-port]
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
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 19210)
RTSP_PORT = PORT + 100


def main() -> int:
    temp = Path(tempfile.mkdtemp(prefix="uvnvr-tokens-"))
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

    def call(method: str, path: str, body: dict | None = None, token: str | None = None):
        request = urllib.request.Request(
            f"http://127.0.0.1:{PORT}{path}",
            data=None if body is None else json.dumps(body).encode(),
            method=method,
        )
        if body is not None:
            request.add_header("Content-Type", "application/json")
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        elif csrf and method not in ("GET", "HEAD"):
            request.add_header("X-CSRF-Token", csrf)
        try:
            with opener.open(request, timeout=15) as response:
                return response.status, json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"null")

    def create_token(name: str, scopes: list[str] | None):
        body = {"name": name, "expiresInDays": 1}
        if scopes is not None:
            body["scopes"] = scopes
        status, result = call("POST", "/api/v1/tokens", body)
        assert status == 201, (status, result)
        return result["token"]

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

        status, _ = call("POST", "/api/v1/tokens", {"name": "bad", "scopes": ["nonsense"]})
        assert status == 400, status

        preview = create_token("preview", ["preview", "channels.read"])
        status, _ = call("GET", "/api/v1/channels", token=preview)
        assert status == 200, status
        status, _ = call("GET", "/api/v1/dashboard/summary", token=preview)
        assert status == 403, status

        creator = create_token("creator", ["channels.create"])
        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "api-created",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": 554,
                "mainUrl": "rtsp://127.0.0.1:554/none",
                "enabled": False,
            },
            token=creator,
        )
        assert status == 201, (status, channel)
        status, _ = call("GET", "/api/v1/channels", token=creator)
        assert status == 403, status

        deleter = create_token("deleter", ["channels.delete"])
        status, _ = call("DELETE", f"/api/v1/channels/{channel['id']}", token=deleter)
        assert status == 200, status

        status, _ = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "no-create",
                "protocol": "rtsp",
                "host": "127.0.0.1",
                "port": 554,
                "mainUrl": "rtsp://127.0.0.1:554/none",
                "enabled": False,
            },
            token=deleter,
        )
        assert status == 403, status

        clock = create_token("clock", ["time"])
        status, _ = call("GET", "/api/v1/system/time", token=clock)
        assert status == 200, status

        inherited = create_token("inherited", None)
        status, _ = call("GET", "/api/v1/dashboard/summary", token=inherited)
        assert status == 200, status

        status, tokens = call("GET", "/api/v1/tokens")
        assert status == 200
        assert any(item["name"] == "preview" and item.get("scopes") == ["preview", "channels.read"]
                   for item in tokens), tokens
        print("token scope integration passed")
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
