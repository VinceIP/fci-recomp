"""tools/fci_hub.py with fake instances: routing, the public hub's rules and the status page."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request

import pytest

HUB = Path(__file__).resolve().parents[1] / "tools" / "fci_hub.py"


def free_port(kind):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_hub(*extra):
    port, status = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    proc = subprocess.Popen(
        [
            sys.executable,
            "-B",
            str(HUB),
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--status-port",
            str(status),
            "--status-rooms",
            *extra,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{status}/", timeout=0.5)
            break
        except OSError:
            time.sleep(0.1)
    return proc, ("127.0.0.1", port), status


@pytest.fixture
def hub():
    proc, addr, status = start_hub("--room-limit", "3", "--rate", "50", "--broadcast-rate", "10")
    yield addr, status
    proc.kill()
    proc.wait()


class Card:
    def __init__(self, hub, mac, room="r"):
        self.hub, self.mac, self.room, self.cookie = hub, bytes.fromhex(mac), room, b"\0" * 8
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.3)
        self.hello()  # the hub answers with this address's cookie, as the card does (fci_nic.c)
        reply = self.sock.recvfrom(64)[0]
        assert reply[:5] == b"FCN1C" and len(reply) == 13
        self.cookie = reply[5:]
        self.hello()

    def hello(self, mac=None, cookie=None):
        self.sock.sendto(b"FCN1H" + (mac or self.mac) + (cookie or self.cookie) + self.room.encode(), self.hub)

    def send(self, dest, payload=b"x" * 50, src=None):
        self.sock.sendto(b"FCN1F" + bytes.fromhex(dest) + (src or self.mac) + b"\x08\x00" + payload, self.hub)

    def received(self):
        out = []
        try:
            while True:
                out.append(self.sock.recvfrom(2048)[0])
        except socket.timeout:
            return out


def rooms(status):
    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{status}/", timeout=2))["by_room"]


def test_unicast_broadcast_and_rooms(hub):
    addr, _ = hub
    a, b, c = Card(addr, "024600000001"), Card(addr, "024600000002"), Card(addr, "024600000003")
    other = Card(addr, "024600000004", room="elsewhere")
    time.sleep(0.2)
    a.send("ffffffffffff")
    assert len(b.received()) == 1 and len(c.received()) == 1 and other.received() == []
    a.send("024600000002")
    assert len(b.received()) == 1 and c.received() == []
    a.send("024600000001")  # to itself: delivered nowhere, not flooded
    assert b.received() == [] and c.received() == []


def test_spoofing_is_refused(hub):
    addr, status = hub
    a, b = Card(addr, "024600000001"), Card(addr, "024600000002")
    thief = Card(addr, "024600000009")
    time.sleep(0.2)
    thief.hello(bytes.fromhex("024600000002"))  # claims b's card: ignored while b is there
    time.sleep(0.1)
    a.send("024600000002")
    assert len(b.received()) == 1 and thief.received() == []
    thief.send("ffffffffffff", src=bytes.fromhex("024600000001"))  # a frame not from its own card
    assert a.received() == [] and b.received() == []
    # a hello with another address's cookie (as from a forged source) gets a cookie back, not a place
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.3)
    s.sendto(b"FCN1H" + bytes.fromhex("02460000000a") + a.cookie + b"x", addr)
    assert s.recvfrom(64)[0][:5] == b"FCN1C"
    time.sleep(0.1)
    assert "x" not in rooms(status)


def test_frame_sizes(hub):
    addr, _ = hub
    a, b = Card(addr, "024600000001"), Card(addr, "024600000002")
    time.sleep(0.2)
    a.send("024600000002", payload=b"x" * 1500)  # 1514 bytes: the largest frame
    a.send("024600000002", payload=b"x" * 1501)
    got = b.received()
    assert len(got) == 1 and len(got[0]) == 5 + 1514


def test_room_limit_rates_and_status(hub):
    addr, status = hub
    cards = [Card(addr, f"0246000000{i:02x}") for i in range(1, 5)]
    time.sleep(0.2)
    assert rooms(status) == {"r": 3}  # the fourth is refused
    for _ in range(200):
        cards[0].send("024600000002")
    got = len(cards[1].received())
    assert 40 <= got <= 80  # --rate 50 with a full bucket to start
    time.sleep(1.5)  # refill both budgets
    for _ in range(60):
        cards[1].send("ffffffffffff")
    got = len(cards[2].received())
    assert 8 <= got <= 25  # --broadcast-rate 10


def scoped_room(room, triple_hex):
    # the card's room name (fci_nic.c room_scope): the room cut to fit + '~' + the 12 hex chars
    return room[: 32 - 1 - len(triple_hex)] + "~" + triple_hex


def test_room_scoped_by_the_version(hub):
    # the room name carries the version triple: two cards with the same triple share the room, a
    # card with an old net protocol lands in a different room and never sees the others
    addr, status = hub
    same = scoped_room("lobby", "000300000002")
    old = scoped_room("lobby", "000300000001")
    a = Card(addr, "024600000001", room=same)
    b = Card(addr, "024600000002", room=same)
    o = Card(addr, "024600000003", room=old)
    time.sleep(0.2)
    a.send("ffffffffffff")
    assert len(b.received()) == 1 and o.received() == []
    assert rooms(status) == {same: 2, old: 1}


def test_per_address_limit():
    proc, addr, status = start_hub("--per-ip", "2")
    try:
        for i in range(1, 4):
            try:
                Card(addr, f"0246000000{i:02x}", room=f"r{i}")
            except socket.timeout:
                pass
        time.sleep(0.2)
        assert sum(rooms(status).values()) == 2
    finally:
        proc.kill()
        proc.wait()
