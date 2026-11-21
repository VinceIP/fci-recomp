"""A virtual Ethernet switch for the game's network card: each instance sends its frames here over UDP and
gets back the frames of the others in its room, so System Link sees one LAN (src/fci_nic.c). The game's own
System Link menu then lists every match hosted in the room, and joining one works as on a LAN.

  python -B tools/fci_hub.py [--port 47800] [--bind 0.0.0.0] [--status-port <port>] [--status-bind 127.0.0.1]
                             [--status-rooms] [--room-limit 32] [--rooms 256] [--per-ip 16] [--players 512]
                             [--rate 1500] [--broadcast-rate 20]
                             [--pcap <file>] [--delay <ms>] [--jitter <ms>] [--loss <percent>]

An instance joins with RECOMP_FCI_NET=<host>:<port>[/<room>] (fci.ini network-hub). Datagrams are "FCN1",
a kind and a body: 'H' hello (MAC, an 8-byte cookie, then the room's name; every 2 s, so an instance quiet
for 10 s leaves), 'C' the hub's cookie for the sender's address, and 'F' an Ethernet frame. A hello joins
only with the cookie the hub gave that address, so a forged source address cannot join (and cannot turn
the room's traffic on someone else). A frame to a MAC the hub has seen goes to that instance only;
broadcasts, multicasts and frames to unknown MACs go to the rest of the room.

For a public hub: a MAC belongs to the first instance that says hello with it until that one leaves, a frame
must come from its sender's own MAC and fit an Ethernet frame, each instance may send --rate frames a second
(System Link play needs about ten; a host sending a user map bursts to about 500) of which --broadcast-rate
may go to the whole room (the game broadcasts under one a second), an address (one router) holds --per-ip
instances, rooms hold --room-limit and the hub --players (past what it can carry, a join is refused rather
than every match slowed). --status-port serves the totals as JSON over HTTP, on --status-bind
(this machine only, unless set); --status-rooms adds each room's name and size, which gives away room names
used as passwords. --delay, --jitter and --loss hold back, reorder or drop frames as an internet link would.
Standard library only, so it runs on any small Linux machine (tools/hub/: a container and a service).
"""

import argparse
import collections
import heapq
import hmac
import http.server
import json
import os
import random
import socket
import struct
import threading
import time

MAGIC = b"FCN1"
TIMEOUT = 10.0
ROOM_NAME_MAX = 32
COOKIE = 8
FRAME_MIN, FRAME_MAX = 14, 1514  # an Ethernet header; the largest frame without its checksum


def log(text):
    print(time.strftime("%Y-%m-%d %H:%M:%S ") + text, flush=True)


class Peer:
    __slots__ = ("room", "mac", "seen", "last", "tokens", "btokens", "frames", "dropped")

    def __init__(self, room, mac, now, args):
        self.room, self.mac, self.seen, self.last = room, mac, now, now  # last: the budgets' refill
        self.tokens, self.btokens, self.frames, self.dropped = args.rate, args.broadcast_rate, 0, 0


class Hub:
    def __init__(self, args):
        self.args = args
        self.secret = os.urandom(32)  # cookies stop working when the hub restarts; instances just ask again
        self.peers = {}  # address -> Peer
        self.rooms = {}  # room -> set of addresses
        self.macs = {}  # (room, MAC) -> address
        self.per_ip = collections.Counter()
        self.frames = 0
        self.started = time.time()
        self.lock = threading.Lock()  # the status thread reads while the loop writes

    def cookie(self, addr, minute):
        msg = f"{addr[0]}:{addr[1]}:{minute}".encode()
        return hmac.new(self.secret, msg, "sha256").digest()[:COOKIE]

    def cookie_ok(self, addr, got):
        minute = int(time.time() // 60)
        return any(hmac.compare_digest(got, self.cookie(addr, m)) for m in (minute, minute - 1))

    def leave(self, addr, why):
        p = self.peers.pop(addr)
        members = self.rooms[p.room]
        members.discard(addr)
        if not members:
            del self.rooms[p.room]
        if self.macs.get((p.room, p.mac)) == addr:
            del self.macs[(p.room, p.mac)]
        self.per_ip[addr[0]] -= 1
        if not self.per_ip[addr[0]]:
            del self.per_ip[addr[0]]
        log(f"left {addr[0]}:{addr[1]} room {p.room!r} ({why}; {p.frames} frames, {p.dropped} over a budget)")

    def hello(self, addr, body, now):
        """None, or the cookie to send back when the hello did not carry this address's"""
        mac, got = body[:6], body[6 : 6 + COOKIE]
        if not self.cookie_ok(addr, got):
            return self.cookie(addr, int(time.time() // 60))
        room = "".join(
            c for c in body[6 + COOKIE : 6 + COOKIE + ROOM_NAME_MAX].decode("utf-8", "replace") if c.isprintable()
        )
        if mac[0] & 1:
            return None  # a multicast address is no card's
        owner = self.macs.get((room, mac))
        if owner and owner != addr and owner in self.peers:
            return None  # someone else's card: ignored until it leaves
        p = self.peers.get(addr)
        if p and p.room == room and p.mac == mac:
            p.seen = now
            return None
        if p:
            self.leave(addr, "changed room or card")
        if room not in self.rooms and len(self.rooms) >= self.args.rooms:
            return None
        if (
            self.per_ip[addr[0]] >= self.args.per_ip
            or len(self.peers) >= self.args.players
            or len(self.rooms.get(room, ())) >= self.args.room_limit
        ):
            return None
        members = self.rooms.setdefault(room, set())
        members.add(addr)
        self.peers[addr] = Peer(room, mac, now, self.args)
        self.macs[(room, mac)] = addr
        self.per_ip[addr[0]] += 1
        log(f"joined {addr[0]}:{addr[1]} room {room!r} card {mac.hex(':')} ({len(members)} in the room)")
        return None

    def route(self, p, addr, body, now):
        """the addresses a frame goes to; a flooded frame spends the broadcast budget too"""
        p.tokens = min(self.args.rate, p.tokens + (now - p.last) * self.args.rate)
        p.btokens = min(self.args.broadcast_rate, p.btokens + (now - p.last) * self.args.broadcast_rate)
        p.seen = p.last = now
        dest = self.macs.get((p.room, body[:6]))
        flood = dest is None
        if p.tokens < 1 or (flood and p.btokens < 1):
            p.dropped += 1
            return ()
        p.tokens -= 1
        p.frames += 1
        self.frames += 1
        if dest == addr:
            return ()  # to itself: nothing to deliver
        if not flood:
            return (dest,)
        p.btokens -= 1
        return [a for a in self.rooms[p.room] if a != addr]

    def status(self):
        with self.lock:
            s = {
                "uptime_s": int(time.time() - self.started),
                "cpu_s": round(time.process_time(), 2),
                "frames": self.frames,
                "players": len(self.peers),
                "rooms": len(self.rooms),
                "public": len(self.rooms.get("", ())),
            }
            if self.args.status_rooms:
                s["by_room"] = {room: len(members) for room, members in sorted(self.rooms.items())}
            return s


def serve_status(hub, bind, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        timeout = 5  # a client that sends nothing does not hold a thread

        def do_GET(self):
            body = json.dumps(hub.status()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer((bind, port), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    log(f"status on http://{bind}:{port}/")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=47800)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--status-port", type=int, help="serve the totals as JSON over HTTP on this port")
    ap.add_argument("--status-bind", default="127.0.0.1", help="address the status page listens on")
    ap.add_argument("--status-rooms", action="store_true", help="list each room's name and size on the status page")
    ap.add_argument("--room-limit", type=int, default=32, help="instances a room holds (System Link: 16 a match)")
    ap.add_argument("--rooms", type=int, default=256, help="rooms the hub holds")
    ap.add_argument("--per-ip", type=int, default=16, help="instances one address (one router) may have")
    ap.add_argument("--players", type=int, default=512, help="instances the hub holds (one core carries about 500)")
    ap.add_argument("--rate", type=float, default=1500, help="frames a second an instance may send")
    ap.add_argument("--broadcast-rate", type=float, default=20, help="of those, frames a second to the whole room")
    ap.add_argument("--pcap", help="also write every frame to this capture file (Wireshark, tcpdump)")
    ap.add_argument("--delay", type=float, default=0, help="hold each frame this many milliseconds")
    ap.add_argument("--jitter", type=float, default=0, help="plus up to this many milliseconds more, at random")
    ap.add_argument("--loss", type=float, default=0, help="drop this percentage of frames")
    args = ap.parse_args()
    hub = Hub(args)
    held = []  # (due, sequence, datagram, address): frames waiting out --delay
    sequence = 0
    shaping = args.delay or args.jitter
    pcap = None
    if args.pcap:
        pcap = open(args.pcap, "wb")
        pcap.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))  # link type 1: Ethernet
    if args.status_port:
        serve_status(hub, args.status_bind, args.status_port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
        sock.setsockopt(socket.SOL_SOCKET, opt, 1 << 20)  # a burst queues; an overload drops, not seconds of lag
    sock.bind((args.bind, args.port))
    sock.settimeout(0.001 if shaping else 1.0)

    def send(d, a):
        try:
            sock.sendto(d, a)
        except OSError:  # an address gone unreachable: its timeout removes it
            pass

    log(f"hub on {args.bind}:{args.port}")
    last_sweep = time.monotonic()
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data, addr = b"", None
        except OSError:  # Windows reports an earlier send to a closed port here
            continue
        now = time.monotonic()
        while held and held[0][0] <= now:
            _, _, d, a = heapq.heappop(held)
            send(d, a)
        targets = ()
        with hub.lock:
            if now - last_sweep >= 1.0:
                last_sweep = now
                for a in [a for a, p in hub.peers.items() if now - p.seen > TIMEOUT]:
                    hub.leave(a, "quiet")
            if addr is None or len(data) < 5 or data[:4] != MAGIC:
                continue
            kind, body = data[4:5], data[5:]
            if kind == b"H" and len(body) >= 6 + COOKIE:
                reply = hub.hello(addr, body, now)
                if reply:
                    send(MAGIC + b"C" + reply, addr)  # smaller than the hello: nothing to amplify
                continue
            p = hub.peers.get(addr)
            if kind != b"F" or p is None or not FRAME_MIN <= len(body) <= FRAME_MAX or body[6:12] != p.mac:
                continue  # not a frame, not joined, not Ethernet-sized, or not from its own card
            targets = hub.route(p, addr, body, now)
        for a in targets:
            if args.loss and random.random() * 100 < args.loss:
                continue
            if shaping:
                sequence += 1
                heapq.heappush(held, (now + (args.delay + random.random() * args.jitter) / 1000, sequence, data, a))
            else:
                send(data, a)
        if pcap and targets:
            t = time.time()
            pcap.write(struct.pack("<IIII", int(t), int(t % 1 * 1e6), len(body), len(body)) + body)
            pcap.flush()
        if targets and (hub.frames in (1, 10, 100, 1000) or hub.frames % 100000 == 0):
            log(f"{hub.frames} frames")


if __name__ == "__main__":
    main()
