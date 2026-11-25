#!/usr/bin/env python3
"""Minimal fake GB/T 28181 SIP registrar for sip_reg_cli testing.

It answers REGISTER with a 401 Digest challenge and then 200 OK, and answers
the keepalive MESSAGE with 200 OK. It exists to exercise the client's
registration, authentication, heartbeat and reconnect paths without a real
platform.

  python3 fake_sip_server.py --transport udp --port 5060 \
      --realm 3402000000 --password 12345678

Supported options:
  --transport udp|tcp   default udp
  --host HOST           default 127.0.0.1
  --port PORT           default 5060
  --realm REALM         default 3402000000
  --username USER       require this SIP user (default: any)
  --password PASSWORD   Digest password (default 12345678)
  --server-id ID        platform ID used in Contact (default 34020000002000000001)
  --expires SECONDS     Expires returned on success (default 3600)
  --nonce NONCE         fixed nonce (default generated once at startup)
  --duration SECONDS    stop automatically (default: run until interrupted)
  --quiet               log only authentication/registration results
"""

import argparse
import hashlib
import re
import signal
import socket
import socketserver
import sys
import threading
import time

COMPACT = {
    "v": "via",
    "f": "from",
    "t": "to",
    "i": "call-id",
    "m": "contact",
    "l": "content-length",
    "c": "content-type",
}


def now():
    return time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)


def log(message):
    print("[%s] %s" % (now(), message), flush=True)


def parse_message(data):
    """Returns (start_line, [(name, value)], body) or None."""
    header_end = data.find(b"\r\n\r\n")
    separator = 4
    if header_end < 0:
        header_end = data.find(b"\n\n")
        separator = 2
    if header_end < 0:
        return None
    head = data[:header_end].decode("utf-8", "replace")
    rest = data[header_end + separator :]
    lines = head.replace("\r\n", "\n").split("\n")
    if not lines or not lines[0].strip():
        return None
    headers = []
    for line in lines[1:]:
        if not line:
            continue
        if line[0] in " \t" and headers:
            headers[-1] = (headers[-1][0], headers[-1][1] + " " + line.strip())
            continue
        if ":" not in line:
            return None
        name, value = line.split(":", 1)
        headers.append((name.strip(), value.strip()))
    content_length = 0
    for name, value in headers:
        if COMPACT.get(name.lower(), name.lower()) == "content-length":
            try:
                content_length = int(value.strip())
            except ValueError:
                return None
            break
    if len(rest) < content_length:
        return None
    body = rest[:content_length]
    return lines[0].strip(), headers, body


def header(headers, name):
    wanted = COMPACT.get(name.lower(), name.lower())
    for key, value in headers:
        if COMPACT.get(key.lower(), key.lower()) == wanted:
            return value
    return ""


def headers_named(headers, name):
    wanted = COMPACT.get(name.lower(), name.lower())
    return [value for key, value in headers if COMPACT.get(key.lower(), key.lower()) == wanted]


def method_and_uri(start_line):
    parts = start_line.split()
    if len(parts) < 3 or parts[2].upper() != "SIP/2.0":
        return None, None
    return parts[0].upper(), parts[1]


def parse_auth_params(value):
    if not value.lower().startswith("digest"):
        return {}
    text = value[len("digest") :].strip()
    params = {}
    for match in re.finditer(
        r'([A-Za-z0-9_-]+)\s*=\s*(?:"((?:\\.|[^"])*)"|([^,\s]+))', text
    ):
        name = match.group(1).lower()
        raw = match.group(2) if match.group(2) is not None else match.group(3)
        params[name] = raw.replace('\\"', '"').replace("\\\\", "\\")
    return params


def md5(text):
    return hashlib.md5(text.encode("utf-8")).hexdigest()


class Registrar:
    def __init__(self, args):
        self.args = args
        self.realm = args.realm
        self.password = args.password
        self.username = args.username
        self.server_id = args.server_id
        self.expires = args.expires
        self.nonce = args.nonce or hashlib.md5(str(time.time()).encode()).hexdigest()
        self.lock = threading.Lock()
        self.registrations = 0
        self.keepalives = 0

    def check_digest(self, method, uri, auth_value):
        params = parse_auth_params(auth_value)
        if not params:
            return False, "missing or non-Digest Authorization"
        user = params.get("username", "")
        if self.username and user != self.username:
            return False, "unexpected username %r" % user
        if params.get("realm") != self.realm:
            return False, "realm mismatch"
        if params.get("nonce") != self.nonce:
            return False, "nonce mismatch"
        ha1 = md5("%s:%s:%s" % (user, self.realm, self.password))
        ha2 = md5("%s:%s" % (method, uri))
        qop = params.get("qop")
        if qop:
            expected = md5(
                "%s:%s:%s:%s:%s:%s"
                % (
                    ha1,
                    params.get("nonce"),
                    params.get("nc", ""),
                    params.get("cnonce", ""),
                    qop,
                    ha2,
                )
            )
        else:
            expected = md5("%s:%s:%s" % (ha1, params.get("nonce"), ha2))
        if params.get("response") != expected:
            return False, "digest response mismatch"
        return True, "ok"

    def base_headers(self, headers, add_to_tag=True):
        lines = []
        for via in headers_named(headers, "Via"):
            lines.append("Via: " + via)
        lines.append("From: " + header(headers, "From"))
        to = header(headers, "To")
        if add_to_tag and "tag=" not in to:
            to += ";tag=srv-" + self.nonce[:8]
        lines.append("To: " + to)
        lines.append("Call-ID: " + header(headers, "Call-ID"))
        lines.append("CSeq: " + header(headers, "CSeq"))
        return lines

    def response(self, status, reason, headers, extra, body=b""):
        lines = ["SIP/2.0 %d %s" % (status, reason)]
        lines.extend(self.base_headers(headers))
        lines.extend(extra)
        lines.append("User-Agent: fake-gb28181-registrar")
        lines.append("Content-Length: %d" % len(body))
        return ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body

    def challenge(self, headers):
        return self.response(
            401,
            "Unauthorized",
            headers,
            [
                'WWW-Authenticate: Digest realm="%s", nonce="%s", algorithm=MD5, qop="auth"'
                % (self.realm, self.nonce)
            ],
        )

    def handle(self, data, peer):
        parsed = parse_message(data)
        if parsed is None:
            log("%s malformed SIP message (%d bytes)" % (peer, len(data)))
            return None
        start_line, headers, body = parsed
        method, uri = method_and_uri(start_line)
        if method is None:
            log("%s malformed start line: %s" % (peer, start_line))
            return None

        if method == "REGISTER":
            auth = header(headers, "Authorization") or header(headers, "Proxy-Authorization")
            if not auth:
                log("%s REGISTER -> 401 challenge" % peer)
                return self.challenge(headers)
            ok, reason = self.check_digest("REGISTER", uri, auth)
            if not ok:
                log("%s REGISTER -> 401 (%s)" % (peer, reason))
                return self.challenge(headers)
            with self.lock:
                self.registrations += 1
                count = self.registrations
            log(
                "%s REGISTER authenticated user=%s -> 200 OK expires=%d (registration #%d)"
                % (peer, parse_auth_params(auth).get("username", "?"), self.expires, count)
            )
            return self.response(
                200,
                "OK",
                headers,
                [
                    "Contact: <sip:%s@%s:%d>"
                    % (self.server_id, self.args.host, self.args.port),
                    "Expires: %d" % self.expires,
                ],
            )

        if method == "MESSAGE":
            auth = header(headers, "Authorization") or header(headers, "Proxy-Authorization")
            if auth:
                ok, reason = self.check_digest("MESSAGE", uri, auth)
                if not ok:
                    log("%s MESSAGE -> 401 (%s)" % (peer, reason))
                    return self.challenge(headers)
            match = re.search(rb"<CmdType>([^<]+)</CmdType>", body)
            cmd = match.group(1).decode("ascii", "replace") if match else "?"
            sn_match = re.search(rb"<SN>([^<]+)</SN>", body)
            sn = sn_match.group(1).decode("ascii", "replace") if sn_match else "?"
            with self.lock:
                self.keepalives += 1
                count = self.keepalives
            log("%s MESSAGE cmd=%s SN=%s -> 200 OK (keepalive #%d)" % (peer, cmd, sn, count))
            return self.response(200, "OK", headers, [])

        if method == "OPTIONS":
            log("%s OPTIONS -> 200 OK" % peer)
            return self.response(200, "OK", headers, [])

        log("%s %s -> 405 Method Not Allowed" % (peer, method))
        return self.response(405, "Method Not Allowed", headers, ["Allow: REGISTER, MESSAGE, OPTIONS"])


class TcpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        log("%s tcp connection opened" % peer)
        buffer = b""
        while True:
            try:
                chunk = self.request.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while True:
                complete = self.split_message(buffer)
                if complete is None:
                    break
                consumed, message = complete
                buffer = buffer[consumed:]
                reply = self.server.registrar.handle(message, peer)
                if reply is not None:
                    try:
                        self.request.sendall(reply)
                    except OSError:
                        return
        log("%s tcp connection closed" % peer)

    @staticmethod
    def split_message(buffer):
        header_end = buffer.find(b"\r\n\r\n")
        separator = 4
        if header_end < 0:
            header_end = buffer.find(b"\n\n")
            separator = 2
        if header_end < 0:
            return None
        head = buffer[:header_end].decode("utf-8", "replace")
        content_length = 0
        for line in head.replace("\r\n", "\n").split("\n")[1:]:
            if ":" not in line:
                continue
            name, value = line.split(":", 1)
            if COMPACT.get(name.strip().lower(), name.strip().lower()) == "content-length":
                try:
                    content_length = int(value.strip())
                except ValueError:
                    content_length = 0
                break
        total = header_end + separator + content_length
        if len(buffer) < total:
            return None
        return total, buffer[:total]


class ThreadingTcpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def run_udp(registrar, args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    log("udp registrar listening on %s:%d realm=%s" % (args.host, args.port, args.realm))
    deadline = time.monotonic() + args.duration if args.duration else None
    sock.settimeout(0.5)
    try:
        while deadline is None or time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            peer = "%s:%d" % addr[:2]
            reply = registrar.handle(data, peer)
            if reply is not None:
                sock.sendto(reply, addr)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
    log("udp registrar stopped")


def run_tcp(registrar, args):
    server = ThreadingTcpServer((args.host, args.port), TcpHandler)
    server.registrar = registrar
    log("tcp registrar listening on %s:%d realm=%s" % (args.host, args.port, args.realm))
    timer = None
    if args.duration:
        timer = threading.Timer(args.duration, server.shutdown)
        timer.daemon = True
        timer.start()
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if timer is not None:
            timer.cancel()
    log("tcp registrar stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transport", choices=["udp", "tcp"], default="udp")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5060)
    parser.add_argument("--realm", default="3402000000")
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="12345678")
    parser.add_argument("--server-id", default="34020000002000000001")
    parser.add_argument("--expires", type=int, default=3600)
    parser.add_argument("--nonce", default="")
    parser.add_argument("--duration", type=float, default=0)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.quiet:
        global log
        original = log

        def quiet_log(message):
            if "authenticated" in message or "-> 401" in message:
                original(message)

        log = quiet_log

    registrar = Registrar(args)
    if args.transport == "tcp":
        run_tcp(registrar, args)
    else:
        run_udp(registrar, args)


if __name__ == "__main__":
    main()
