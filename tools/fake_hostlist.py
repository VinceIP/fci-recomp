"""A fake internet host list for local testing (tools/hostlist/lobby.js without Cloudflare).

Stdlib only: ThreadingHTTPServer on 127.0.0.1. Implements the four endpoints with the same JSON as
lobby.js, including the held long-poll and the token. Skips the public-address check so 127.0.0.1
registers and connects work.

Usage: python tools/fake_hostlist.py [port] [--break-punch]   (default 47801)

--break-punch rewrites the endpoint in every connect it forwards to the host (port + 1000), so the
host punches a dead port and ignores the guest's punches (endpoint mismatch): both 3 s windows fail
and both sides fall back to the relay (RECOMP_FCI_NET_RELAY).
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import secrets
import threading
import time
from urllib.parse import parse_qs, urlparse

TTL_SECONDS = 60
CONNECTS_MAX = 8
LIST_MAX = 64

MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


class Lobby:
    """In-memory host list, the same shape as lobby.js's Durable Object."""

    def __init__(self, break_punch=False):
        self.lock = threading.Lock()
        self.hosts = {}  # id -> {id, token, ip, proto, title, version, mac, endpoint, expires}
        self.connects = {}  # id -> [{endpoint, nonce, mac}]
        self.waiters = {}  # id -> {event, result}
        self.break_punch = break_punch  # rewrite the endpoint in every connect (port + 1000)

    def _expire(self, id_):
        self.hosts.pop(id_, None)
        self.connects.pop(id_, None)
        w = self.waiters.pop(id_, None)
        if w:
            w["result"] = (204, None)
            w["event"].set()

    def _refresh(self, id_):
        h = self.hosts.get(id_)
        if h:
            h["expires"] = time.time() + TTL_SECONDS

    def register(self, body, ip):
        proto = body.get("proto")
        title = body.get("title")
        version = str(body.get("version", ""))
        mac = body.get("mac", "")
        endpoint = body.get("endpoint", "")
        # skip the public-address check: 127.0.0.1 is fine for local test
        if proto != 1 or title not in ("instincts", "evolution"):
            return 400, {"error": "bad registration"}
        if not MAC_RE.match(mac):
            return 400, {"error": "bad MAC (want lowercase xx:xx:xx:xx:xx:xx)"}
        if not endpoint:
            return 400, {"error": "bad registration"}
        id_ = secrets.token_hex(12)
        token = secrets.token_hex(16)
        with self.lock:
            # cap live hosts per address
            live = [h for h in self.hosts.values() if h["ip"] == ip]
            if len(live) >= 4:
                return 429, {"error": "too many hosts from this address"}
            self.hosts[id_] = {
                "id": id_,
                "token": token,
                "ip": ip,
                "proto": proto,
                "title": title,
                "version": version,
                "mac": mac,
                "endpoint": endpoint,
                "expires": time.time() + TTL_SECONDS,
            }
        return 200, {"id": id_, "token": token}

    def hosts_list(self, title, version, max_):
        with self.lock:
            now = time.time()
            expired = [h["id"] for h in self.hosts.values() if h["expires"] <= now]
            for id_ in expired:
                self._expire(id_)
            found = [
                {"id": h["id"], "endpoint": h["endpoint"], "mac": h["mac"]}
                for h in self.hosts.values()
                if h["title"] == title and h["version"] == version and h["proto"] == 1
            ]
            found.sort(key=lambda x: x["id"])
            return 200, {"proto": 1, "hosts": found[:max_]}

    def poll(self, id_, token, wait, ip):
        with self.lock:
            h = self.hosts.get(id_)
            if not h or h["token"] != token or h["ip"] != ip:
                return 404, {"error": "no such host"}
            self._refresh(id_)
            q = self.connects.get(id_)
            if q:
                del self.connects[id_]
                return 200, {"connects": q}
            # hold the request
            event = threading.Event()
            waiter = {"event": event, "result": None}
            # replace any existing waiter
            old = self.waiters.pop(id_, None)
            if old:
                old["result"] = (204, None)
                old["event"].set()
            self.waiters[id_] = waiter
        # wait outside the lock
        event.wait(timeout=wait)
        with self.lock:
            self.waiters.pop(id_, None)
        result = waiter["result"] or (204, None)
        return result

    def connect(self, body, ip):
        id_ = body.get("id", "")
        endpoint = body.get("endpoint", "")
        nonce = body.get("nonce", "")
        mac = body.get("mac", "")
        if not endpoint or not nonce:
            return 400, {"error": "bad request"}
        if not MAC_RE.match(mac):
            return 400, {"error": "bad MAC (want lowercase xx:xx:xx:xx:xx:xx)"}
        if self.break_punch:
            # the host will punch a dead port and ignore the guest's punches (endpoint mismatch)
            host, port = endpoint.rsplit(":", 1)
            endpoint = f"{host}:{int(port) + 1000}"
        with self.lock:
            if id_ not in self.hosts:
                return 404, {"error": "no such host"}
            q = self.connects.setdefault(id_, [])
            if len(q) >= CONNECTS_MAX:
                q.pop(0)
            q.append({"endpoint": endpoint, "nonce": nonce, "mac": mac})
            w = self.waiters.get(id_)
            if w:
                self.waiters.pop(id_, None)
                w["result"] = (200, {"connects": q})
                # clear the queue (it was delivered)
                del self.connects[id_]
                w["event"].set()
        return 204, None


def make_handler(lobby):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass  # quiet

        def _send(self, status, body):
            payload = json.dumps(body).encode() if body is not None else b""
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            if payload:
                self.wfile.write(payload)

        def do_POST(self):
            url = urlparse(self.path)
            if "application/json" not in (self.headers.get("content-type") or ""):
                self._send(415, {"error": "JSON only"})  # as the Worker (lobby.js) answers
                return
            length = int(self.headers.get("content-length", 0))
            raw = self.rfile.read(length) if length else b"{}"
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                self._send(400, {"error": "not JSON"})
                return
            ip = self.client_address[0]
            if url.path == "/v1/register":
                status, resp = lobby.register(body, ip)
            elif url.path == "/v1/connect":
                status, resp = lobby.connect(body, ip)
            else:
                status, resp = 404, {"error": "not found"}
            self._send(status, resp)

        def do_GET(self):
            url = urlparse(self.path)
            params = parse_qs(url.query)
            ip = self.client_address[0]
            if url.path == "/v1/hosts":
                title = params.get("title", [""])[0]
                version = params.get("version", [""])[0]
                max_ = min(int(params.get("max", [str(LIST_MAX)])[0]), LIST_MAX)
                status, resp = lobby.hosts_list(title, version, max_)
            elif url.path == "/v1/connects":
                id_ = params.get("id", [""])[0]
                token = params.get("token", [""])[0]
                wait = min(30, max(1, int(params.get("wait", ["20"])[0])))
                status, resp = lobby.poll(id_, token, wait, ip)
            else:
                status, resp = 404, {"error": "not found"}
            self._send(status, resp)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "port",
        nargs="?",
        type=int,
        default=None,
        help="port (default $FAKE_HOSTLIST_PORT, else 47801)",
    )
    ap.add_argument(
        "--break-punch",
        action="store_true",
        help="rewrite the endpoint in every connect to the host (port + 1000)",
    )
    args = ap.parse_args()
    port = args.port if args.port is not None else int(os.environ.get("FAKE_HOSTLIST_PORT", "47801"))
    lobby = Lobby(break_punch=args.break_punch)
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(lobby))
    server.daemon_threads = True
    print(f"fake host list on http://127.0.0.1:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
