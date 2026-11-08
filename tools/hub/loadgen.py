"""Load for a System Link hub: fake 16-player matches with the traffic the game makes, to see how many a hub
carries. Only for a hub you run yourself.

  python -B tools/hub/loadgen.py --hub <host>:<port> [--rooms 4] [--players 16] [--seconds 30]
                                 [--host-rate 8] [--client-rate 6] [--broadcast-every 2] [--size 150]

Per room, as measured in play (research multiplayer.md): the host sends --host-rate frames a second to each
client, each client --client-rate to the host, and every player broadcasts every --broadcast-every seconds.
Each fake card does the hub's cookie hello first. Prints one JSON line: frames sent, frames that should
have arrived, frames that did, and the delay through the hub (same clock: run it on the hub's machine or
read the delay as relative). Run several at once (different --tag) to load past one Python process.
"""

import argparse
import json
import os
import selectors
import socket
import struct
import time

MAGIC = b"FCN1"
BROADCAST = b"\xff" * 6


def card(hub, mac, room):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.connect(hub)
    s.settimeout(1)
    for _ in range(10):  # a busy hub may take a while, and a lost reply is a lost datagram
        s.send(MAGIC + b"H" + mac + b"\0" * 8 + room)
        try:
            reply = s.recv(64)
        except (socket.timeout, ConnectionResetError):
            continue
        if reply[:5] == MAGIC + b"C":
            break
    else:
        raise SystemExit(f"no cookie from the hub at {hub[0]}:{hub[1]}")
    s.send(MAGIC + b"H" + mac + reply[5:] + room)
    s.setblocking(False)
    return s, reply[5:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hub", required=True)
    ap.add_argument("--rooms", type=int, default=4)
    ap.add_argument("--players", type=int, default=16)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--host-rate", type=float, default=8)
    ap.add_argument("--client-rate", type=float, default=6)
    ap.add_argument("--broadcast-every", type=float, default=2)
    ap.add_argument("--size", type=int, default=150, help="frame payload bytes")
    ap.add_argument("--tag", default=str(os.getpid()))
    args = ap.parse_args()
    host, port = args.hub.rsplit(":", 1)
    hub = (socket.gethostbyname(host), int(port))
    tag = int(args.tag) & 0xFF

    peers = []  # (socket, mac, cookie, room, room index, player index)
    for r in range(args.rooms):
        room = f"load-{args.tag}-{r}".encode()
        for p in range(args.players):
            mac = bytes((0x02, 0x4C, tag, r >> 8, r & 0xFF, p))
            s, cookie = card(hub, mac, room)
            peers.append((s, mac, cookie, room, r, p))
    sel = selectors.DefaultSelector()
    for i, peer in enumerate(peers):
        sel.register(peer[0], selectors.EVENT_READ, i)
    time.sleep(0.5)  # every hello in before the first frame

    # what each card sends: (period, destination MAC or None for broadcast), phases spread out
    plan = []
    for i, (_s, _mac, _cookie, _room, r, p) in enumerate(peers):
        host_mac = bytes((0x02, 0x4C, tag, r >> 8, r & 0xFF, 0))
        if p == 0:
            for q in range(1, args.players):
                plan.append([i, 1 / args.host_rate, bytes((0x02, 0x4C, tag, r >> 8, r & 0xFF, q))])
        else:
            plan.append([i, 1 / args.client_rate, host_mac])
        plan.append([i, args.broadcast_every, None])
    start = time.perf_counter()
    for k, item in enumerate(plan):
        item.append(start + (k * 0.6180339 % 1) * item[1])  # next due
    pad = b"\0" * max(0, args.size - 16)
    sent = expected = received = 0
    delays = []
    last_hello = start
    end = start + args.seconds
    while True:
        now = time.perf_counter()
        if now >= end:
            break
        for item in plan:
            i, period, dest, due = item
            while due <= now:
                s, mac = peers[i][0], peers[i][1]
                frame = (dest or BROADCAST) + mac + b"\x08\x00" + struct.pack("<dQ", time.perf_counter(), sent) + pad
                try:
                    s.send(MAGIC + b"F" + frame)
                except OSError:
                    pass
                sent += 1
                expected += 1 if dest else args.players - 1
                due += period
            item[3] = due
        if now - last_hello >= 2:
            last_hello = now
            for s, mac, cookie, room, _r, _p in peers:
                s.send(MAGIC + b"H" + mac + cookie + room)
        for key, _ in sel.select(timeout=0.002):
            s = key.fileobj
            while True:
                try:
                    d = s.recv(2048)
                except (BlockingIOError, ConnectionResetError):
                    break
                if d[4:5] == b"F" and len(d) >= 5 + 14 + 16:
                    received += 1
                    delays.append(time.perf_counter() - struct.unpack_from("<d", d, 5 + 14)[0])
    time.sleep(0.5)  # stragglers
    for key, _ in sel.select(timeout=0.1):
        while True:
            try:
                d = key.fileobj.recv(2048)
            except (BlockingIOError, ConnectionResetError):
                break
            if d[4:5] == b"F":
                received += 1
    delays.sort()

    def pct(q):
        return round(delays[min(len(delays) - 1, int(q * len(delays)))] * 1000, 2) if delays else None

    print(
        json.dumps(
            {
                "tag": args.tag,
                "rooms": args.rooms,
                "players": args.players,
                "seconds": args.seconds,
                "sent": sent,
                "expected": expected,
                "received": received,
                "loss_pct": round(100 * (1 - received / expected), 2) if expected else None,
                "delay_ms": {"p50": pct(0.5), "p99": pct(0.99), "max": pct(1.0)},
            }
        )
    )


if __name__ == "__main__":
    main()
