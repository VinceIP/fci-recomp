"""A fake internet host list for local testing (tools/hostlist/lobby.js without Cloudflare).

Stdlib only: ThreadingHTTPServer on 127.0.0.1. Implements the same endpoints and JSON as lobby.js:
the token-derived id (sha256(token), first 16 bytes, hex), the id-only listing, the connect reply
(the host's endpoint and MAC), the held long-poll, the bans, and the admin routes. It skips the
public-address check so 127.0.0.1 registers and connects work, and it keeps the bans and the rate
limits in memory (lobby.js keeps the bans in the object's SQLite).

Usage: python tools/fake_hostlist.py [port] [--break-punch] [--secret S]   (default 47801)

--break-punch rewrites the endpoint in every connect it forwards to the host (port + 1000), so the
host punches a dead port and ignores the guest's punches (endpoint mismatch): both 3 s windows fail
and both sides fall back to the relay (RECOMP_FCI_NET_RELAY).
--secret sets the admin secret (default $FAKE_HOSTLIST_SECRET, else none: admin is off).
"""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import threading
import time
from urllib.parse import parse_qs, urlparse

TTL_SECONDS = 60
CONNECTS_MAX = 8  # the NIC's pending-peer cap
LIST_MAX = 64
HOST_CONNECTS_PER_MINUTE = 24  # the per-host flood budget
MESSAGE_MAX = 512
RELAY = os.environ.get("FAKE_HOSTLIST_RELAY", "")  # the canonical hub, if set

MAC_RE = re.compile(r"^02:46:43(:[0-9a-f]{2}){3}$")  # the cards' own prefix (fci_nic.c)
ID_RE = re.compile(r"^[0-9a-f]{32}$")  # 16 bytes of sha256(token), hex
TOKEN_RE = re.compile(r"^[0-9a-f]{64}$")  # 32 random bytes, hex

LIMITS = {"hostsPerIp": 4, "listsPerMinute": 30, "connectsPerMinute": 60, "registersPerMinute": 10}


def derive_id(token):
    """The player's id from the identity token: the first 16 bytes of SHA-256(token), hex."""
    return hashlib.sha256(token.encode()).digest()[:16].hex()


def _minute(now):
    return int(now // 60)


class RateLimit:
    def __init__(self):
        self.lock = threading.Lock()
        self.m = {}  # key -> (minute, n)

    def take(self, key, max_, now):
        minute = _minute(now)
        with self.lock:
            m, n = self.m.get(key, (minute, 0))
            if m != minute:
                n = 0
            n += 1
            self.m[key] = (minute, n)
            if len(self.m) > 4000:
                self.m = {k: v for k, v in self.m.items() if v[0] == minute}
            return n <= max_


class Lobby:
    """In-memory host list, the same shape as lobby.js's Durable Object."""

    def __init__(self, break_punch=False, secret=None):
        self.lock = threading.Lock()
        self.hosts = {}  # id -> {id, ip, proto, title, version, mac, endpoint, expires}
        self.connects = {}  # id -> [{endpoint, nonce, mac, gid}]
        self.waiters = {}  # id -> {event, result}
        self.host_conn = {}  # id -> (minute, n)
        self.bans = {}  # id -> {id, reason, date}
        self.limits = RateLimit()
        self.abuse = {"byIp": {}, "ring": []}
        self.break_punch = break_punch  # rewrite the endpoint in every connect (port + 1000)
        self.secret = secret

    # -- abuse counters (memory only, like lobby.js) -----------------------------------------
    def note(self, kind, ip, id_=None):
        with self.lock:
            self._note(kind, ip, id_)

    def _note(self, kind, ip, id_=None):  # the lock is already held
        c = self.abuse["byIp"].setdefault(ip, {"ip": ip, "rate": 0, "bad": 0, "nohost": 0, "banned": 0})
        c[kind] += 1
        self.abuse["ring"].append({"ip": ip, "kind": kind, "id": id_, "at": int(time.time() * 1000)})
        if len(self.abuse["ring"]) > 1000:
            self.abuse["ring"].pop(0)

    # -- bans (lobby.js keeps these in SQLite; the fake keeps them in memory) ----------------
    def is_banned(self, id_):
        return id_ in self.bans

    def ban(self, id_, reason):
        with self.lock:
            self.bans[id_] = {"id": id_, "reason": reason, "date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}

    def unban(self, id_):
        with self.lock:
            return self.bans.pop(id_, None) is not None

    # -- lifecycle ---------------------------------------------------------------------------
    def _expire(self, id_):
        self.hosts.pop(id_, None)
        self.connects.pop(id_, None)
        self.host_conn.pop(id_, None)
        w = self.waiters.pop(id_, None)
        if w:
            w["result"] = (204, None)
            w["event"].set()

    def _refresh(self, id_):
        h = self.hosts.get(id_)
        if h:
            h["expires"] = time.time() + TTL_SECONDS

    def register(self, body, ip):
        now = time.time()
        if not self.limits.take("register:" + ip, LIMITS["registersPerMinute"], now):
            self.note("rate", ip)
            return 429, {"error": "slow down"}
        token = body.get("token", "")
        if not TOKEN_RE.match(token):
            self.note("bad", ip)
            return 400, {"error": "bad token"}
        id_ = derive_id(token)
        if self.is_banned(id_):
            self.note("banned", ip, id_)
            return 403, {"error": "banned"}
        proto = body.get("proto")
        title = body.get("title")
        version = str(body.get("version", ""))
        mac = body.get("mac", "")
        endpoint = body.get("endpoint", "")
        # skip the public-address check: 127.0.0.1 is fine for a local test
        if proto != 1 or title not in ("instincts", "evolution") or not re.match(r"^[0-9]{1,5}$", version):
            self.note("bad", ip)
            return 400, {"error": "bad registration"}
        if not MAC_RE.match(mac) or not endpoint:
            self.note("bad", ip)
            return 400, {"error": "bad registration"}
        with self.lock:
            live = [h for h in self.hosts.values() if h["ip"] == ip]
            if len(live) >= LIMITS["hostsPerIp"]:
                self._note("rate", ip)
                return 429, {"error": "too many hosts from this address"}
            self.hosts[id_] = {
                "id": id_,
                "ip": ip,
                "proto": proto,
                "title": title,
                "version": version,
                "mac": mac,
                "endpoint": endpoint,
                "expires": now + TTL_SECONDS,
            }
        return 200, {"id": id_}  # no token back: the runtime already has it

    def hosts_list(self, title, version, max_):
        with self.lock:
            now = time.time()
            for id_ in [h["id"] for h in self.hosts.values() if h["expires"] <= now]:
                self._expire(id_)
            found = [
                {"id": h["id"]}  # ids only: no endpoint, MAC or token in a listing
                for h in self.hosts.values()
                if h["title"] == title and h["version"] == version and h["proto"] == 1 and not self.is_banned(h["id"])
            ]
            found.sort(key=lambda x: x["id"])
            body = {"proto": 1, "hosts": found[:max_]}
            if RELAY:
                body["relay"] = RELAY
            return 200, body

    def poll(self, id_, token, wait, ip):
        if not ID_RE.match(id_) or not TOKEN_RE.match(token) or derive_id(token) != id_:
            return 404, {"error": "no such host"}
        with self.lock:
            h = self.hosts.get(id_)
            if not h or h["ip"] != ip:
                return 404, {"error": "no such host"}
            if self.is_banned(id_):
                self._expire(id_)
                return 404, {"error": "no such host"}
            self._refresh(id_)
            q = self.connects.get(id_)
            if q:
                del self.connects[id_]
                out = [{"endpoint": c["endpoint"], "nonce": c["nonce"], "mac": c["mac"]} for c in q]
                return 200, {"connects": out}
            event = threading.Event()
            waiter = {"event": event, "result": None}
            old = self.waiters.pop(id_, None)
            if old:
                old["result"] = (204, None)
                old["event"].set()
            self.waiters[id_] = waiter
        event.wait(timeout=wait)
        with self.lock:
            self.waiters.pop(id_, None)
        return waiter["result"] or (200, {"connects": []})

    def connect(self, body, ip):
        now = time.time()
        if not self.limits.take("connect:" + ip, LIMITS["connectsPerMinute"], now):
            self.note("rate", ip)
            return 429, {"error": "slow down"}
        id_ = body.get("id", "")
        endpoint = body.get("endpoint", "")
        nonce = body.get("nonce", "")
        mac = body.get("mac", "")
        token = body.get("token", "")
        # skip the public-address check on the guest's endpoint: 127.0.0.1 is fine locally
        if not ID_RE.match(id_) or not endpoint or not re.match(r"^[0-9a-f]{32}$", nonce) or not MAC_RE.match(mac):
            self.note("bad", ip)
            return 400, {"error": "bad request"}
        if not TOKEN_RE.match(token):
            self.note("bad", ip)
            return 400, {"error": "bad token"}
        gid = derive_id(token)
        if self.is_banned(gid):
            self.note("banned", ip, gid)
            return 403, {"error": "banned"}
        if self.break_punch:
            host, port = endpoint.rsplit(":", 1)
            endpoint = f"{host}:{int(port) + 1000}"
        with self.lock:
            h = self.hosts.get(id_)
            if not h or self.is_banned(id_):
                self._note("nohost", ip, id_)
                return 404, {"error": "no such host"}
            minute = _minute(now)
            m, n = self.host_conn.get(id_, (minute, 0))
            if m != minute:
                n = 0
            n += 1
            self.host_conn[id_] = (minute, n)
            if n > HOST_CONNECTS_PER_MINUTE:
                self._note("rate", ip, id_)
                return 429, {"error": "host busy"}
            q = self.connects.setdefault(id_, [])
            i = next((j for j, c in enumerate(q) if c["gid"] == gid), -1)
            if i >= 0:
                q[i] = {"endpoint": endpoint, "nonce": nonce, "mac": mac, "gid": gid}
            else:
                if len(q) >= CONNECTS_MAX:
                    q.pop(0)
                q.append({"endpoint": endpoint, "nonce": nonce, "mac": mac, "gid": gid})
            w = self.waiters.get(id_)
            if w:
                self.waiters.pop(id_, None)
                out = [{"endpoint": c["endpoint"], "nonce": c["nonce"], "mac": c["mac"]} for c in q]
                del self.connects[id_]
                w["result"] = (200, {"connects": out})
                w["event"].set()
        return 200, {"endpoint": h["endpoint"], "mac": h["mac"]}  # the host's endpoint and MAC

    # -- admin (the operator's secret is the only gate) ---------------------------------------
    def admin(self, path, method, body, query, ip):
        if not self.secret:
            return 503, {"error": "admin not configured"}
        if path == "/admin/hosts" and method == "GET":
            with self.lock:
                hosts = [
                    {
                        "id": h["id"],
                        "ip": h["ip"],
                        "title": h["title"],
                        "version": h["version"],
                        "mac": h["mac"],
                        "endpoint": h["endpoint"],
                        "pending": len(self.connects.get(h["id"], [])),
                        "expires": h["expires"],
                    }
                    for h in self.hosts.values()
                ]
            return 200, {"hosts": hosts}
        if path == "/admin/bans" and method == "GET":
            with self.lock:
                return 200, {"bans": sorted(self.bans.values(), key=lambda b: b["date"], reverse=True)}
        if path == "/admin/bans" and method == "POST":
            id_ = body.get("id", "")
            if not ID_RE.match(id_):
                return 400, {"error": "bad id"}
            self.ban(id_, str(body.get("reason", ""))[:200])
            return 200, {"ok": True}
        if path == "/admin/bans" and method == "DELETE":
            id_ = query.get("id", [""])[0]
            if not ID_RE.match(id_):
                return 400, {"error": "bad id"}
            return 200, {"removed": self.unban(id_)}
        if path == "/admin/abuse" and method == "GET":
            with self.lock:
                by_ip = sorted(
                    self.abuse["byIp"].values(),
                    key=lambda c: c["rate"] + c["bad"] + c["nohost"] + c["banned"],
                    reverse=True,
                )
                return 200, {"byIp": by_ip[:100], "ring": self.abuse["ring"]}
        return 404, {"error": "not found"}


def make_handler(lobby):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass  # quiet

        def _send(self, status, body):
            if status == 204 or body is None:
                self.send_response(status)
                self.send_header("content-length", "0")
                self.end_headers()
                return
            payload = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _authed(self):
            if not lobby.secret:
                return False
            got = (self.headers.get("authorization") or "").removeprefix("Bearer ").strip()
            if len(got) != len(lobby.secret):
                return False
            diff = 0
            for a, b in zip(got, lobby.secret):
                diff |= ord(a) ^ ord(b)
            return diff == 0

        def do_POST(self):
            url = urlparse(self.path)
            length = int(self.headers.get("content-length", 0))
            raw = self.rfile.read(length) if length else b"{}"
            if url.path.startswith("/admin/"):
                if not self._authed():
                    self._send(401, {"error": "unauthorized"})
                    return
                try:
                    body = json.loads(raw) if raw else {}
                except json.JSONDecodeError:
                    self._send(400, {"error": "not JSON"})
                    return
                if not isinstance(body, dict):
                    self._send(400, {"error": "not JSON"})
                    return
                status, resp = lobby.admin(url.path, "POST", body, parse_qs(url.query), self.client_address[0])
                self._send(status, resp)
                return
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                self._send(400, {"error": "not JSON"})
                return
            if not isinstance(body, dict):
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
            if url.path.startswith("/admin/"):
                if not self._authed():
                    self._send(401, {"error": "unauthorized"})
                    return
                status, resp = lobby.admin(url.path, "GET", {}, params, self.client_address[0])
                self._send(status, resp)
                return
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

        def do_DELETE(self):
            url = urlparse(self.path)
            if url.path.startswith("/admin/"):
                if not self._authed():
                    self._send(401, {"error": "unauthorized"})
                    return
                status, resp = lobby.admin(url.path, "DELETE", {}, parse_qs(url.query), self.client_address[0])
                self._send(status, resp)
                return
            self._send(404, {"error": "not found"})

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", type=int, default=None, help="port (default $FAKE_HOSTLIST_PORT, else 47801)")
    ap.add_argument(
        "--break-punch",
        action="store_true",
        help="rewrite the endpoint in every connect to the host (port + 1000)",
    )
    ap.add_argument("--secret", default=None, help="admin secret (default $FAKE_HOSTLIST_SECRET, else admin off)")
    args = ap.parse_args()
    port = args.port if args.port is not None else int(os.environ.get("FAKE_HOSTLIST_PORT", "47801"))
    secret = args.secret if args.secret is not None else os.environ.get("FAKE_HOSTLIST_SECRET")
    lobby = Lobby(break_punch=args.break_punch, secret=secret)
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(lobby))
    server.daemon_threads = True
    print(f"fake host list on http://127.0.0.1:{port}" + (f" (admin secret set, relay={RELAY})" if secret else ""))
    server.serve_forever()


if __name__ == "__main__":
    main()
