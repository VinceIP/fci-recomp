"""A fake internet host list for local testing (tools/hostlist/lobby.js without Cloudflare).

Stdlib only: ThreadingHTTPServer on 127.0.0.1. Implements the same endpoints and JSON as lobby.js:
the token-derived id (sha256(token), first 16 bytes, hex), the id-only listing, the connect reply
(the host's endpoint and MAC), the held long-poll, the leaving-host unregister, the bans, and the
admin routes. It skips the public-address check so 127.0.0.1 registers and connects work, and it
keeps the bans and the rate limits in memory (lobby.js keeps the bans in the object's SQLite).

Usage: python tools/fake_hostlist.py [port] [--break-punch] [--secret S]
       [--turn-user U --turn-pass P --turn-url URL]   (default port 47801)

--break-punch rewrites the endpoint in every direct connect it forwards to the host (port + 1000),
so the host punches a dead port and ignores the guest's punches (endpoint mismatch): both 3 s
windows fail and both sides fall back to the relay. A relayed connect (relay: true, the guest's
TURN address) is forwarded untouched: the host punches that address for real.
--secret sets the admin secret (default $FAKE_HOSTLIST_SECRET, else none: admin is off).
--turn-user / --turn-pass / --turn-url configure /v1/turn (the mint gate, the mint rate limits and
the relay connect behave as in lobby.js); without a user and a password, /v1/turn is 503 "no relay".
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
NONCE_RE = re.compile(r"^[0-9a-f]{32}$")

TURN_WINDOW_SECONDS = 600  # a mint (and a relayed connect) is valid for 10 minutes after the connect
MINTS_PER_WINDOW = 6  # mints per guest and per address, per 10-minute window
RELAY_CONNECTS_PER_NONCE = 3  # re-POSTs of a relayed address (ICE restarts)
DEFAULT_TURN_URL = "turn:127.0.0.1:3478?transport=udp"
HOSTS_MAX = 1024  # the global live-host cap: the per-address caps bound one client, this bounds a flood of them
LIMITS = {
    "hostsPerIp": 16,
    "listsPerMinute": 240,
    "connectsPerMinute": 120,
    "registersPerMinute": 40,
    "unregistersPerMinute": 10,  # a leaving host removes its own listing (one per session, in practice)
}


def local_ok(endpoint):
    """A private IPv4 (10/8, 172.16/12, 192.168/16) with a valid port.
    The card's LAN address: only forwarded when both sides are behind the same NAT.
    100.64/10 (CGNAT) is excluded: two phones behind the same carrier share no network."""
    m = re.match(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$", str(endpoint))
    if not m:
        return False
    a, b, c, d, port = (int(m[i]) for i in range(1, 6))
    if a < 1 or a > 255 or b > 255 or c > 255 or d > 255:
        return False
    if port <= 0 or port > 65535:
        return False
    return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)


def same_nat(ip_a, ip_b, ep_a, ep_b):
    """Same NAT (lobby.js sameNat): the same caller IP, or the same IP in both STUN endpoints.
    With IPv6 each device has its own caller IP (seen 2026-11-09)."""
    if ip_a and ip_a == ip_b:
        return True
    a = str(ep_a or "").rpartition(":")[0]
    return a != "" and a == str(ep_b or "").rpartition(":")[0]


def _entries(q, host_ip="", host_ep=""):
    # The host sees {endpoint, nonce, mac} per connect (plus relay: true for a relayed
    # address, and local: the guest's LAN address when both sides are behind the same NAT);
    # the guest's id is the object's business. The local is forwarded only when same_nat holds.
    out = []
    for c in q:
        e = {"endpoint": c["endpoint"], "nonce": c["nonce"], "mac": c["mac"]}
        if c.get("relay"):
            e["relay"] = True
        if c.get("local") and same_nat(c.get("ip"), host_ip, c["endpoint"], host_ep):
            e["local"] = c["local"]
        out.append(e)
    return out


def derive_id(token):
    """The player's id from the identity token: the first 16 bytes of SHA-256(token), hex."""
    return hashlib.sha256(token.encode()).digest()[:16].hex()


def _minute(now):
    return int(now // 60)


class RateLimit:
    def __init__(self, span=60):
        self.span = span
        self.lock = threading.Lock()
        self.m = {}  # key -> (window, n)

    def take(self, key, max_, now):
        minute = int(now // self.span)
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

    def __init__(self, break_punch=False, secret=None, turn_user=None, turn_pass=None, turn_url=DEFAULT_TURN_URL):
        self.lock = threading.Lock()
        self.hosts = {}  # id -> {id, ip, proto, title, version, mac, endpoint, expires}
        self.connects = {}  # id -> [{endpoint, nonce, mac, gid, relay}]
        self.waiters = {}  # id -> {event, result}
        self.host_conn = {}  # id -> (minute, n)
        self.pairs = {}  # gid+":"+nonce -> {at, hosts: {host: at}}: recent non-relay connects
        self.mints = {}  # gid+":"+nonce -> {host, at}: recent mints (the relay gate's exact-nonce path)
        self.gid_mints = {}  # gid -> {at}: the guest's newest successful mint, any nonce (the relay gate)
        self.relay_counts = {}  # gid+":"+nonce -> {at, n}: accepted relay connects per nonce
        self.bans = {}  # id -> {id, reason, date}
        self.limits = RateLimit()
        self.mint_limits = RateLimit(TURN_WINDOW_SECONDS)
        self.abuse = {"byIp": {}, "ring": []}
        self.break_punch = break_punch  # rewrite the endpoint in direct connects (port + 1000)
        self.secret = secret
        self.turn_user = turn_user
        self.turn_pass = turn_pass
        self.turn_url = turn_url

    # -- abuse counters (memory only, like lobby.js) -----------------------------------------
    def note(self, kind, ip, id_=None):
        with self.lock:
            self._note(kind, ip, id_)

    def _note(self, kind, ip, id_=None):  # the lock is already held
        c = self.abuse["byIp"].setdefault(ip, {"ip": ip, "rate": 0, "bad": 0, "nohost": 0, "banned": 0, "norelay": 0})
        c[kind] += 1
        self.abuse["ring"].append({"ip": ip, "kind": kind, "id": id_, "at": int(time.time() * 1000)})
        if len(self.abuse["ring"]) > 1000:
            self.abuse["ring"].pop(0)

    def _prune_turn_state(self, now):  # the lock is already held
        # The connect queue is drained by the host's poll, so the mint and relay-connect gates
        # keep their own recent (gid, nonce) records, pruned by age (10 minutes).
        for m in (self.pairs, self.mints, self.gid_mints, self.relay_counts):
            for k in [k for k, v in m.items() if now - v["at"] > TURN_WINDOW_SECONDS]:
                del m[k]

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
        if proto != 2 or title not in ("instincts", "evolution") or not re.match(r"^[0-9]{1,5}$", version):
            self.note("bad", ip)
            return 400, {"error": "bad registration"}
        if not MAC_RE.match(mac) or not endpoint:
            self.note("bad", ip)
            return 400, {"error": "bad registration"}
        local = body.get("local", "")
        local = local if local_ok(local) else None
        with self.lock:
            live = [h for h in self.hosts.values() if h["ip"] == ip]
            if len(live) >= LIMITS["hostsPerIp"]:
                self._note("rate", ip)
                return 429, {"error": "too many hosts from this address"}
            if id_ not in self.hosts and len(self.hosts) >= HOSTS_MAX:
                self._note("rate", ip)
                return 503, {"error": "host list full"}  # a re-register of a live host never grows the map
            self.hosts[id_] = {
                "id": id_,
                "ip": ip,
                "proto": proto,
                "title": title,
                "version": version,
                "mac": mac,
                "endpoint": endpoint,
                "local": local,
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
                if h["title"] == title and h["version"] == version and h["proto"] == 2 and not self.is_banned(h["id"])
            ]
            found.sort(key=lambda x: x["id"])
            body = {"proto": 2, "hosts": found[:max_]}
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
                return 200, {"connects": _entries(q, h["ip"], h["endpoint"])}
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
        relay = body.get("relay") is True
        # skip the public-address check on the guest's endpoint: 127.0.0.1 is fine locally
        if not ID_RE.match(id_) or not endpoint or not NONCE_RE.match(nonce) or not MAC_RE.match(mac):
            self.note("bad", ip)
            return 400, {"error": "bad request"}
        if not TOKEN_RE.match(token):
            self.note("bad", ip)
            return 400, {"error": "bad token"}
        gid = derive_id(token)
        if self.is_banned(gid):
            self.note("banned", ip, gid)
            return 403, {"error": "banned"}
        if self.break_punch and not relay:
            # the relayed address is the guest's real TURN address: the host punches it for real
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
            # A relayed address: the guest's punch failed, it ALLOCATEd through TURN, and this is
            # its second connect with the same nonce. One mint (a TURN allocation) covers every
            # host the guest reached directly: accepted with a mint for this gid within the window
            # (any nonce, any host) and a non-relay connect of this (gid, nonce) to this host
            # within the window; a mint for this exact (gid, nonce, host) still qualifies. At most
            # RELAY_CONNECTS_PER_NONCE times per nonce (ICE restarts). It keeps every other check
            # and budget above.
            key = gid + ":" + nonce
            if relay:
                pair = self.pairs.get(key)
                gid_mint = self.gid_mints.get(gid)
                exact = self.mints.get(key)
                direct = pair is not None and id_ in pair["hosts"] and now - pair["hosts"][id_] <= TURN_WINDOW_SECONDS
                gated = (gid_mint is not None and now - gid_mint["at"] <= TURN_WINDOW_SECONDS and direct) or (
                    exact is not None and exact["host"] == id_ and now - exact["at"] <= TURN_WINDOW_SECONDS
                )
                if not gated:
                    self._note("bad", ip, gid)
                    return 403, {"error": "no relay"}
                relays = self.relay_counts.get(key, {"at": now, "n": 0})
                if relays["n"] >= RELAY_CONNECTS_PER_NONCE:
                    self._note("rate", ip, gid)
                    return 429, {"error": "too many relays"}
                relays["n"] += 1
                relays["at"] = now
                self.relay_counts[key] = relays
            if not relay:
                # the mint and relay gates: this guest reached this host directly
                pair = self.pairs.get(key) or {"at": now, "hosts": {}}
                pair["hosts"][id_] = now
                pair["at"] = now
                self.pairs[key] = pair
            self._prune_turn_state(now)
            glocal = body.get("local", "")
            glocal = glocal if local_ok(glocal) else None
            q = self.connects.setdefault(id_, [])
            i = next((j for j, c in enumerate(q) if c["gid"] == gid), -1)
            if i >= 0:
                q[i] = {
                    "endpoint": endpoint,
                    "nonce": nonce,
                    "mac": mac,
                    "gid": gid,
                    "relay": relay,
                    "local": glocal,
                    "ip": ip,
                }
            else:
                if len(q) >= CONNECTS_MAX:
                    q.pop(0)
                q.append(
                    {
                        "endpoint": endpoint,
                        "nonce": nonce,
                        "mac": mac,
                        "gid": gid,
                        "relay": relay,
                        "local": glocal,
                        "ip": ip,
                    }
                )
            w = self.waiters.get(id_)
            if w:
                self.waiters.pop(id_, None)
                del self.connects[id_]
                w["result"] = (200, {"connects": _entries(q, h["ip"], h["endpoint"])})
                w["event"].set()
        # The host's endpoint and MAC are the reply; the host's LAN address is forwarded only
        # when the guest and the host are behind the same NAT (same_nat).
        reply = {"endpoint": h["endpoint"], "mac": h["mac"]}
        if h.get("local") and same_nat(h["ip"], ip, h["endpoint"], endpoint):
            reply["local"] = h["local"]
        return 200, reply

    # Mint a TURN credential for a guest, gated like the Worker: a recent connect from this
    # guest to this host with this nonce (the connect queue is drained by the poll, so the
    # recent pairs live in their own map), and 6 mints per guest and per address in 10 minutes.
    # The stand-in returns its configured credential (--turn-user / --turn-pass at --turn-url)
    # instead of calling Cloudflare; unset means 503 "no relay".
    def turn(self, body, ip):
        now = time.time()
        id_ = body.get("id", "")
        nonce = body.get("nonce", "")
        token = body.get("token", "")
        if not ID_RE.match(id_) or not NONCE_RE.match(nonce):
            self.note("bad", ip)
            return 400, {"error": "bad request"}
        if not TOKEN_RE.match(token):
            self.note("bad", ip)
            return 400, {"error": "bad token"}
        gid = derive_id(token)
        with self.lock:
            if self.is_banned(gid):
                self._note("banned", ip, gid)
                return 403, {"error": "banned"}
            pair = self.pairs.get(gid + ":" + nonce)
            direct = pair is not None and id_ in pair["hosts"] and now - pair["hosts"][id_] <= TURN_WINDOW_SECONDS
            if not direct:
                self._note("bad", ip, gid)
                return 403, {"error": "no connect"}
            if not self.mint_limits.take("gid:" + gid, MINTS_PER_WINDOW, now) or not self.mint_limits.take(
                "ip:" + ip, MINTS_PER_WINDOW, now
            ):
                self._note("rate", ip, gid)
                return 429, {"error": "slow down"}
            if not self.turn_user or not self.turn_pass:
                self._note("norelay", ip, gid)
                return 503, {"error": "no relay"}
            self.mints[gid + ":" + nonce] = {"host": id_, "at": now}
            self.gid_mints[gid] = {"at": now}  # the relay gate: this guest holds a fresh TURN allocation
            self._prune_turn_state(now)
            return 200, {"username": self.turn_user, "credential": self.turn_pass, "urls": [self.turn_url]}

    # A host that is leaving (its process is shutting down) removes its own listing here, so the
    # next instance does not connect to a dead host; without it the listing stays joinable until its
    # TTL (a poll is the renewal). The token must derive to the id: a 403, not a 404, so a wrong
    # token is not mistaken for "already gone". An unknown id is a 200, so a second unregister, or
    # one after a TTL expiry, is a no-op. The long-poll waiter, if any, is released with an empty,
    # successful poll (the host is leaving, not merely idle).
    def unregister(self, body, ip):
        now = time.time()
        if not self.limits.take("unregister:" + ip, LIMITS["unregistersPerMinute"], now):
            self.note("rate", ip)
            return 429, {"error": "slow down"}
        id_ = body.get("id", "")
        token = body.get("token", "")
        if not ID_RE.match(id_) or not TOKEN_RE.match(token) or derive_id(token) != id_:
            self.note("bad", ip, id_)
            return 403, {"error": "bad token"}
        with self.lock:
            h = self.hosts.get(id_)
            if not h:
                return 200, {"ok": True}  # already gone (TTL, a second call): idempotent
            del self.hosts[id_]
            self.connects.pop(id_, None)
            self.host_conn.pop(id_, None)
            w = self.waiters.pop(id_, None)
            if w:
                w["result"] = (200, {"connects": []})  # the host is leaving: an empty, successful poll
                w["event"].set()
        return 200, {"ok": True}

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
                    key=lambda c: c["rate"] + c["bad"] + c["nohost"] + c["banned"] + c["norelay"],
                    reverse=True,
                )
                return 200, {"byIp": by_ip[:100], "ring": self.abuse["ring"]}
        return 404, {"error": "not found"}


def make_handler(lobby):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            # quiet, unless FAKE_HOSTLIST_LOG=1: one line a request, to count a client's requests
            if os.environ.get("FAKE_HOSTLIST_LOG") == "1":
                print(f"{time.time():.1f} {fmt % args}", flush=True)

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
            if "application/json" not in (self.headers.get("content-type") or ""):
                self._send(415, {"error": "JSON only"})  # as the Worker (lobby.js) answers
                return
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
            elif url.path == "/v1/turn":
                status, resp = lobby.turn(body, ip)
            elif url.path == "/v1/unregister":
                status, resp = lobby.unregister(body, ip)
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
                wait = min(60, max(1, int(params.get("wait", ["20"])[0])))  # the Worker's bound
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
        help="rewrite the endpoint in every direct connect to the host (port + 1000); relayed connects are untouched",
    )
    ap.add_argument("--secret", default=None, help="admin secret (default $FAKE_HOSTLIST_SECRET, else admin off)")
    ap.add_argument(
        "--turn-user",
        default=None,
        help="username /v1/turn returns (default $FAKE_HOSTLIST_TURN_USER; unset: 503 no relay)",
    )
    ap.add_argument(
        "--turn-pass", default=None, help="credential password /v1/turn returns (default $FAKE_HOSTLIST_TURN_PASS)"
    )
    ap.add_argument("--turn-url", default=DEFAULT_TURN_URL, help="the turn: URL /v1/turn returns (default %(default)s)")
    args = ap.parse_args()
    port = args.port if args.port is not None else int(os.environ.get("FAKE_HOSTLIST_PORT", "47801"))
    secret = args.secret if args.secret is not None else os.environ.get("FAKE_HOSTLIST_SECRET")
    lobby = Lobby(
        break_punch=args.break_punch,
        secret=secret,
        turn_user=args.turn_user if args.turn_user is not None else os.environ.get("FAKE_HOSTLIST_TURN_USER"),
        turn_pass=args.turn_pass if args.turn_pass is not None else os.environ.get("FAKE_HOSTLIST_TURN_PASS"),
        turn_url=args.turn_url,
    )
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(lobby))
    server.daemon_threads = True
    print(f"fake host list on http://127.0.0.1:{port}" + (f" (admin secret set, relay={RELAY})" if secret else ""))
    server.serve_forever()


if __name__ == "__main__":
    main()
