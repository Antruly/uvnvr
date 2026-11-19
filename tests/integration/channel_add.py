#!/usr/bin/env python3
"""Channel creation integration test.

Covers the 0.18.1 onboarding rules: a plain RTSP channel needs only its URL,
blank names are derived from the host/URL, ONVIF channels keep the ONVIF
transport, and invalid requests return specific errors.

Usage: tests/integration/channel_add.py [path-to-uvnvr] [http-port]
"""
import http.cookiejar
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 18610)
RTSP_PORT = PORT + 255


def main() -> int:
    temp = Path(tempfile.mkdtemp(prefix="uvnvr-channel-add-"))
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

        # A plain RTSP channel: URL only, no name, no host/credentials.
        status, created = call(
            "POST",
            "/api/v1/channels",
            {
                "protocol": "rtsp",
                "mainUrl": "rtsp://user:pass@10.0.0.20:554/stream1",
            },
        )
        assert status == 201, (status, created)
        assert created["name"] == "10.0.0.20", created

        status, created = call(
            "POST",
            "/api/v1/channels",
            {"protocol": "generic", "mainUrl": "rtsp://10.0.0.21/live"},
        )
        assert status == 201, (status, created)
        assert created["name"] == "10.0.0.21", created

        # RTSP without a URL is rejected with a specific error.
        status, error = call("POST", "/api/v1/channels", {"protocol": "rtsp"})
        assert status == 400 and error["error"] == "missing_url", (status, error)

        # Hikvision/Dahua still need a host so the URL template can be built.
        status, error = call("POST", "/api/v1/channels", {"protocol": "hikvision"})
        assert status == 400 and error["error"] == "missing_host", (status, error)

        status, error = call(
            "POST", "/api/v1/channels", {"protocol": "ftp", "mainUrl": "rtsp://x/y"}
        )
        assert status == 400 and error["error"] == "invalid_protocol", (status, error)

        # A discovered ONVIF camera keeps its ONVIF transport and profile URI,
        # even when the manufacturer says Dahua.
        status, created = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "onvif cam",
                "protocol": "onvif",
                "host": "10.0.0.30",
                "httpPort": 80,
                "username": "admin",
                "password": "secret",
                "mainUrl": "rtsp://10.0.0.30:554/cam/realmonitor?channel=1&subtype=0",
                "eventsEnabled": True,
                "brand": "Dahua",
            },
        )
        assert status == 201, (status, created)

        status, rows = call("GET", "/api/v1/channels")
        assert status == 200, (status, rows)
        onvif = [row for row in rows if row["id"] == created["id"]][0]
        assert onvif["protocol"] == "onvif", onvif
        assert onvif["brand"] == "Dahua", onvif
        assert onvif["mainUrl"].endswith("subtype=0"), onvif

        # ONVIF with a host but no URL probes the camera. Nothing listens on
        # port 1, so the request fails as a probe error, not a validation error.
        status, error = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "probe me",
                "protocol": "onvif",
                "host": "127.0.0.1",
                "httpPort": 1,
            },
        )
        assert status == 502 and error["error"] == "onvif_probe_failed", (status, error)

        # Saved default credentials are used by the probe/add flow and stored
        # encrypted on the channel; the password never enters the audit log.
        status, error = call(
            "POST",
            "/api/v1/channels/onvif/probe",
            {
                "xaddr": "http://127.0.0.1:1/onvif/device_service",
                "useDefaultCredentials": True,
            },
        )
        assert status == 400 and error["error"] == "camera_defaults_missing", (status, error)

        status, error = call(
            "PUT",
            "/api/v1/settings",
            {
                "camera": {
                    "defaultUsername": "camera-admin",
                    "password": "TopSecret!",
                    "maxChannels": 4,
                }
            },
        )
        assert status == 200, (status, error)
        assert error["camera"]["defaultUsername"] == "camera-admin", error
        assert error["camera"]["hasDefaultPassword"] is True, error
        assert error["camera"]["maxChannels"] == 4, error

        status, info = call("GET", "/api/v1/channels/onboarding")
        assert status == 200 and info["maxChannels"] == 4, (status, info)
        assert info["channelCount"] == 3, info

        status, created = call(
            "POST",
            "/api/v1/channels",
            {
                "protocol": "rtsp",
                "mainUrl": "rtsp://10.0.0.40/live",
                "useDefaultCredentials": True,
            },
        )
        assert status == 201, (status, created)
        status, rows = call("GET", "/api/v1/channels")
        saved = [row for row in rows if row["id"] == created["id"]][0]
        assert saved["username"] == "camera-admin" and saved["hasPassword"], saved

        status, error = call(
            "POST",
            "/api/v1/channels",
            {"protocol": "rtsp", "mainUrl": "rtsp://10.0.0.41/live"},
        )
        assert status == 409 and error["error"] == "channel_limit", (status, error)

        status, audit = call("GET", "/api/v1/audit")
        assert status == 200, (status, audit)
        assert "TopSecret!" not in json.dumps(audit), "the settings audit leaked the password"

        print("channel creation integration passed")
        return 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
