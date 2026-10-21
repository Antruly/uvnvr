#!/usr/bin/env python3
"""ONVIF PullPoint event integration test with a local mock camera.

Usage: tests/integration/onvif_events.py [path-to-uvnvr] [http-port]
"""
import http.cookiejar
import http.server
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

BIN = Path(sys.argv[1] if len(sys.argv) > 1 else "./build/uvnvr").resolve()
PORT = int(sys.argv[2] if len(sys.argv) > 2 else 18810)
RTSP_PORT = PORT + 100
MOCK_PORT = PORT + 200


class MockOnvif(http.server.BaseHTTPRequestHandler):
    pulls = 0
    time_updates = 0
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        if "GetCapabilities" in body:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
                'xmlns:tt="http://www.onvif.org/ver10/schema">'
                "<s:Body><tds:GetCapabilitiesResponse><tds:Capabilities><tt:Events>"
                f"<tt:XAddr>http://127.0.0.1:{MOCK_PORT}/onvif/events</tt:XAddr>"
                "</tt:Events></tds:Capabilities></tds:GetCapabilitiesResponse></s:Body>"
                "</s:Envelope>"
            )
        elif "CreatePullPointSubscription" in body:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tev="http://www.onvif.org/ver10/events/wsdl" '
                'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" '
                'xmlns:wsa5="http://www.w3.org/2005/08/addressing">'
                "<s:Body><tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
                f"<wsa5:Address>http://127.0.0.1:{MOCK_PORT}/pull1</wsa5:Address>"
                "</tev:SubscriptionReference></tev:CreatePullPointSubscriptionResponse>"
                "</s:Body></s:Envelope>"
            )
        elif "SetSystemDateAndTime" in body:
            with MockOnvif.lock:
                MockOnvif.time_updates += 1
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl">'
                "<s:Body><tds:SetSystemDateAndTimeResponse/></s:Body></s:Envelope>"
            )
        elif "PullMessages" in body:
            with MockOnvif.lock:
                MockOnvif.pulls += 1
                first = MockOnvif.pulls == 1
            notification = ""
            if first:
                notification = (
                    '<wsnt:NotificationMessage xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2">'
                    '<wsnt:Topic Dialect="http://www.onvif.org/ver10/tev/topicExpression/'
                    'ConcreteSet">tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>'
                    "<wsnt:Message><tt:Message "
                    'xmlns:tt="http://www.onvif.org/ver10/schema">'
                    '<tt:Data><tt:SimpleItem Name="IsMotion" Value="true"/></tt:Data>'
                    "</tt:Message></wsnt:Message></wsnt:NotificationMessage>"
                )
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tev="http://www.onvif.org/ver10/events/wsdl">'
                f"<s:Body><tev:PullMessagesResponse>{notification}"
                "</tev:PullMessagesResponse></s:Body></s:Envelope>"
            )
        else:
            response = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body/>'
                "</s:Envelope>"
            )
        data = response.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/soap+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockOnvif)
    threading.Thread(target=mock.serve_forever, daemon=True).start()

    temp = Path(tempfile.mkdtemp(prefix="uvnvr-onvif-"))
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

        status, channel = call(
            "POST",
            "/api/v1/channels",
            {
                "name": "mock onvif",
                "protocol": "onvif",
                "host": "127.0.0.1",
                "port": MOCK_PORT,
                "mainUrl": "rtsp://127.0.0.1:1/none",
                "enabled": True,
                "eventsEnabled": True,
            },
        )
        assert status == 201, (status, channel)

        alert = None
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            status, alerts = call("GET", "/api/v1/alerts?pageSize=50")
            if status == 200:
                alert = next(
                    (
                        item
                        for item in alerts
                        if item.get("channelId") == channel["id"]
                        and item.get("type") == "motion"
                    ),
                    None,
                )
                if alert:
                    break
            time.sleep(0.5)
        assert alert is not None, "no ONVIF motion alert was created"
        print(f"ONVIF motion alert: {alert['message']}")
        assert "motion" in alert["message"].lower()

        status, sync = call(
            "POST",
            "/api/v1/system/time/sync",
            {"epochMs": int(time.time() * 1000)},
        )
        assert status == 200, (status, sync)
        assert sync["cameras"]["synced"] >= 1, sync
        assert MockOnvif.time_updates >= 1, MockOnvif.time_updates
        print(f"camera time sync: {sync['cameras']}")
        print("ONVIF event integration passed")
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
