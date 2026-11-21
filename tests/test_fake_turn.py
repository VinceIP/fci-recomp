"""tools/fake_turn.py: a UDP TURN server (RFC 8656) standing in for Cloudflare.

A small in-file TURN client drives a real server (run in a thread) through the
protocol: long-term auth, allocation, permissions, channels, and the relay of UDP
datagrams between two peers.
"""

import os
import socket
import struct
import threading
import time

import pytest

import fake_turn as ft

REALM = "turn.cloudflare.com"


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, user="u", password="p", realm=REALM, lifetime=600, nonce_life=600):
        self.port = free_port()
        self.server = ft.TurnServer(self.port, "127.0.0.1", user, password, realm, lifetime, nonce_life, log=False)
        self.addr = ("127.0.0.1", self.port)
        self.thread = threading.Thread(target=self.server.run, daemon=True)
        self.thread.start()

    def stop(self):
        self.server.close()
        self.thread.join(timeout=2)


@pytest.fixture
def turn():
    s = Server()
    yield s
    s.stop()


@pytest.fixture
def turn_short():
    s = Server(nonce_life=1)
    yield s
    s.stop()


class Client:
    """A minimal TURN client over one UDP socket (a stable 5-tuple)."""

    def __init__(self, addr, user, password, realm):
        self.addr = addr
        self.user = user
        self.password = password
        self.realm = realm
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(5.0)
        self.relay = None  # (ip, port) once allocated
        self.last_nonce = None
        self._pending = []  # buffered (kind, peer, payload) indications

    def close(self):
        self.sock.close()

    # -- plumbing -------------------------------------------------------------

    def _recv(self):
        return self.sock.recvfrom(65535)[0]

    def _drain_for(self, txn):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            data = self._recv()
            self._maybe_buffer(data)
            parsed = ft.parse_msg(data)
            if parsed is not None and parsed[1] == txn:
                return parsed
        raise AssertionError("no response for transaction")

    def _maybe_buffer(self, data):
        if len(data) >= 2:
            num = struct.unpack("!H", data[:2])[0]
            if ft.CHANNEL_MIN <= num <= ft.CHANNEL_MAX:
                length = struct.unpack("!H", data[2:4])[0]
                self._pending.append(("channel", num, data[4 : 4 + length]))
                return
        parsed = ft.parse_msg(data)
        if parsed is not None and parsed[0] == ft.T_DATA_IND:
            _, txn, attrs = parsed
            peer = ft.dec_addr(ft.get1(attrs, ft.A_XOR_PEER), txn)
            self._pending.append(("data", peer, ft.get1(attrs, ft.A_DATA)))

    @staticmethod
    def _errcode(attrs):
        v = ft.get1(attrs, ft.A_ERROR_CODE)
        return None if v is None else v[2] * 100 + v[3]

    def _auth_msg(self, mtype, txn, attrs, nonce, realm):
        key = ft.cred_key(self.user, realm, self.password)
        auth_attrs = list(attrs) + [
            (ft.A_USERNAME, self.user.encode()),
            (ft.A_REALM, realm.encode()),
            (ft.A_NONCE, nonce.encode()),
        ]
        return ft.add_integrity(ft.build_msg(mtype, txn, auth_attrs), key)

    def _auth_msg_fp(self, mtype, txn, attrs, nonce, realm):
        """Authenticated request carrying MESSAGE-INTEGRITY then FINGERPRINT."""
        key = ft.cred_key(self.user, realm, self.password)
        auth_attrs = list(attrs) + [
            (ft.A_USERNAME, self.user.encode()),
            (ft.A_REALM, realm.encode()),
            (ft.A_NONCE, nonce.encode()),
        ]
        return ft.add_fingerprint(ft.add_integrity(ft.build_msg(mtype, txn, auth_attrs), key))

    def _send_auth(self, mtype, make_attrs, nonce, realm):
        """Send a request already authenticated with the given nonce (no 401 round-trip)."""
        txn = os.urandom(12)
        self.sock.sendto(self._auth_msg(mtype, txn, make_attrs(txn), nonce, realm), self.addr)
        return self._drain_for(txn)

    def _get_nonce(self):
        """Send a bare Allocate (no auth) and return the nonce from the 401 challenge."""
        txn = os.urandom(12)
        self.sock.sendto(
            ft.build_msg(ft.T_ALLOCATE_REQ, txn, [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))]),
            self.addr,
        )
        _, _, rattrs = self._drain_for(txn)
        return ft.get1(rattrs, ft.A_NONCE).decode()

    def _auth_request(self, mtype, make_attrs, mtype_error):
        """Send a request; on 401/438 retry once with the offered nonce + integrity."""
        txn = os.urandom(12)
        self.sock.sendto(ft.build_msg(mtype, txn, make_attrs(txn)), self.addr)
        mt, txn, rattrs = self._drain_for(txn)
        if mt == mtype_error and self._errcode(rattrs) in (401, 438):
            nonce = ft.get1(rattrs, ft.A_NONCE).decode()
            realm_val = ft.get1(rattrs, ft.A_REALM)
            realm = self.realm if realm_val is None else realm_val.decode()
            self.last_nonce = nonce
            txn = os.urandom(12)  # the retry is a new transaction (RFC 8489 9.2.4)
            self.sock.sendto(self._auth_msg(mtype, txn, make_attrs(txn), nonce, realm), self.addr)
            mt, txn, rattrs = self._drain_for(txn)
        return mt, txn, rattrs

    # -- methods --------------------------------------------------------------

    def binding(self):
        txn = os.urandom(12)
        self.sock.sendto(ft.build_msg(ft.T_BINDING_REQ, txn, []), self.addr)
        return self._drain_for(txn)

    def allocate(self, transport=ft.TRANSPORT_UDP):
        def make(txn):
            return [(ft.A_REQUESTED_TRANSPORT, bytes([transport]))]

        mt, txn, rattrs = self._auth_request(ft.T_ALLOCATE_REQ, make, ft.T_ALLOCATE_ERROR)
        if mt == ft.T_ALLOCATE_SUCCESS:
            self.relay = ft.dec_addr(ft.get1(rattrs, ft.A_XOR_RELAYED), txn)
        return mt, rattrs

    def create_permission(self, peer):
        def make(txn):
            return [(ft.A_XOR_PEER, ft.enc_addr(1, peer[0], peer[1], txn))]

        mt, _, rattrs = self._auth_request(ft.T_CREATE_PERM_REQ, make, ft.T_CREATE_PERM_ERROR)
        return mt, rattrs

    def channel_bind(self, chan, peer):
        def make(txn):
            return [
                (ft.A_CHANNEL_NUMBER, struct.pack("!HH", chan, 0)),
                (ft.A_XOR_PEER, ft.enc_addr(1, peer[0], peer[1], txn)),
            ]

        mt, _, rattrs = self._auth_request(ft.T_CHANNEL_BIND_REQ, make, ft.T_CHANNEL_BIND_ERROR)
        return mt, rattrs

    def send(self, peer, data):
        txn = os.urandom(12)
        attrs = [(ft.A_XOR_PEER, ft.enc_addr(1, peer[0], peer[1], txn)), (ft.A_DATA, data)]
        self.sock.sendto(ft.build_msg(ft.T_SEND_IND, txn, attrs), self.addr)

    def channel_data(self, chan, data):
        self.sock.sendto(struct.pack("!HH", chan, len(data)) + data, self.addr)

    def wait_data(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        self.sock.settimeout(0.2)
        while time.monotonic() < deadline:
            if self._pending:
                return self._pending.pop(0)
            try:
                self._maybe_buffer(self._recv())
            except socket.timeout:
                continue
        raise AssertionError("no data indication received")


def test_binding(turn):
    c = Client(turn.addr, "u", "p", REALM)
    mt, txn, rattrs = c.binding()
    assert mt == ft.T_BINDING_SUCCESS
    ip, port = ft.dec_addr(ft.get1(rattrs, ft.A_XOR_MAPPED), txn)
    assert (ip, port) == c.sock.getsockname()
    c.close()


def test_allocate_long_term_auth(turn):
    c = Client(turn.addr, "u", "p", REALM)
    mt, rattrs = c.allocate()
    assert mt == ft.T_ALLOCATE_SUCCESS
    assert c.relay is not None and c.relay[0] == "127.0.0.1"
    lifetime = struct.unpack("!I", ft.get1(rattrs, ft.A_LIFETIME))[0]
    assert lifetime == 600
    c.close()


def test_allocate_wrong_password(turn):
    c = Client(turn.addr, "u", "wrong-password", REALM)
    mt, rattrs = c.allocate()
    assert mt == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 401  # re-challenged, never succeeds
    c.close()


def test_second_allocate_mismatch(turn):
    c = Client(turn.addr, "u", "p", REALM)
    mt1, _ = c.allocate()
    assert mt1 == ft.T_ALLOCATE_SUCCESS
    mt2, rattrs = c.allocate()  # new transaction id -> 437
    assert mt2 == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 437
    c.close()


def test_nonce_reuse(turn):
    """One nonce, reused across Allocate, CreatePermission, ChannelBind, Refresh: no 438."""
    c = Client(turn.addr, "u", "p", REALM)
    mt, _ = c.allocate()  # 401 -> auth with the challenge nonce -> success
    assert mt == ft.T_ALLOCATE_SUCCESS
    n = c.last_nonce
    cp = lambda txn: [(ft.A_XOR_PEER, ft.enc_addr(1, "127.0.0.1", 1, txn))]
    cb = lambda txn: [
        (ft.A_CHANNEL_NUMBER, struct.pack("!HH", 0x4001, 0)),
        (ft.A_XOR_PEER, ft.enc_addr(1, "127.0.0.1", 1, txn)),
    ]
    rf = lambda txn: [(ft.A_LIFETIME, struct.pack("!I", 600))]
    assert c._send_auth(ft.T_CREATE_PERM_REQ, cp, n, REALM)[0] == ft.T_CREATE_PERM_SUCCESS
    assert c._send_auth(ft.T_CHANNEL_BIND_REQ, cb, n, REALM)[0] == ft.T_CHANNEL_BIND_SUCCESS
    assert c._send_auth(ft.T_REFRESH_REQ, rf, n, REALM)[0] == ft.T_REFRESH_SUCCESS
    c.close()


def test_stale_nonce_expires(turn_short):
    """A nonce older than --nonce-life is 438 carrying REALM and NONCE; the retry succeeds."""
    c = Client(turn_short.addr, "u", "p", REALM)
    c.allocate()
    stale = c.last_nonce
    time.sleep(1.5)  # let the nonce (nonce-life=1s) expire; the allocation (600s) survives
    rf = lambda txn: [(ft.A_LIFETIME, struct.pack("!I", 600))]
    txn = os.urandom(12)
    c.sock.sendto(c._auth_msg(ft.T_REFRESH_REQ, txn, rf(txn), stale, REALM), turn_short.addr)
    mt, _, rattrs = c._drain_for(txn)
    assert mt == ft.T_REFRESH_ERROR
    assert c._errcode(rattrs) == 438
    assert ft.get1(rattrs, ft.A_REALM) is not None  # 438 carries REALM
    fresh = ft.get1(rattrs, ft.A_NONCE).decode()
    assert c._send_auth(ft.T_REFRESH_REQ, rf, fresh, REALM)[0] == ft.T_REFRESH_SUCCESS
    c.close()


def test_fingerprint_auth(turn):
    """A request carrying MESSAGE-INTEGRITY then FINGERPRINT authenticates."""
    c = Client(turn.addr, "u", "p", REALM)
    n = c._get_nonce()
    make = lambda txn: [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))]
    txn = os.urandom(12)
    c.sock.sendto(c._auth_msg_fp(ft.T_ALLOCATE_REQ, txn, make(txn), n, REALM), turn.addr)
    mt, txn, rattrs = c._drain_for(txn)
    assert mt == ft.T_ALLOCATE_SUCCESS
    c.close()


def test_bad_fingerprint(turn):
    """A corrupt FINGERPRINT is a 400 Bad Request."""
    c = Client(turn.addr, "u", "p", REALM)
    n = c._get_nonce()
    make = lambda txn: [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))]
    txn = os.urandom(12)
    msg = c._auth_msg_fp(ft.T_ALLOCATE_REQ, txn, make(txn), n, REALM)
    c.sock.sendto(msg[:-4] + b"\xde\xad\xbe\xef", turn.addr)  # corrupt the FINGERPRINT value
    mt, _, rattrs = c._drain_for(txn)
    assert mt == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 400
    c.close()


def test_attr_after_integrity(turn):
    """An attribute after MESSAGE-INTEGRITY other than FINGERPRINT is a 400."""
    c = Client(turn.addr, "u", "p", REALM)
    n = c._get_nonce()
    make = lambda txn: [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))]
    txn = os.urandom(12)
    key = ft.cred_key("u", REALM, "p")
    auth_attrs = make(txn) + [
        (ft.A_USERNAME, b"u"),
        (ft.A_REALM, REALM.encode()),
        (ft.A_NONCE, n.encode()),
    ]
    msg = ft.build_msg(ft.T_ALLOCATE_REQ, txn, auth_attrs)
    msg = ft.add_integrity(msg, key)
    # Append a LIFETIME after MI and extend the declared length so the server sees it.
    lf = ft.enc_attr(ft.A_LIFETIME, struct.pack("!I", 600))
    new_len = struct.unpack_from("!H", msg, 2)[0] + len(lf)
    msg = msg[:2] + struct.pack("!H", new_len) + msg[4:] + lf
    c.sock.sendto(msg, turn.addr)
    mt, _, rattrs = c._drain_for(txn)
    assert mt == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 400
    c.close()


def test_send_relay(turn):
    a = Client(turn.addr, "u", "p", REALM)
    b = Client(turn.addr, "u", "p", REALM)
    a.allocate()
    b.allocate()
    mt, _ = a.create_permission(b.relay)
    assert mt == ft.T_CREATE_PERM_SUCCESS
    a.send(b.relay, b"hello")
    kind, peer, payload = b.wait_data()
    assert kind == "data"
    assert payload == b"hello"
    assert peer == a.relay  # the sender is A's relayed address
    a.close()
    b.close()


def test_channel_data_relay(turn):
    a = Client(turn.addr, "u", "p", REALM)
    b = Client(turn.addr, "u", "p", REALM)
    a.allocate()
    b.allocate()
    # Both sides bind channel 0x4001 to each other.
    assert a.channel_bind(0x4001, b.relay)[0] == ft.T_CHANNEL_BIND_SUCCESS
    assert b.channel_bind(0x4001, a.relay)[0] == ft.T_CHANNEL_BIND_SUCCESS
    a.channel_data(0x4001, b"ping")
    kind, num, payload = b.wait_data()
    assert kind == "channel"
    assert num == 0x4001
    assert payload == b"ping"
    a.close()
    b.close()


def test_tcp_transport_rejected(turn):
    c = Client(turn.addr, "u", "p", REALM)
    mt, rattrs = c.allocate(transport=ft.TRANSPORT_TCP)
    assert mt == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 442
    c.close()


def test_bad_channel_number(turn):
    c = Client(turn.addr, "u", "p", REALM)
    c.allocate()
    mt, rattrs = c.channel_bind(0x5000, ("127.0.0.1", 1))  # 0x5000 is outside 0x4000-0x4FFF
    assert mt == ft.T_CHANNEL_BIND_ERROR
    assert c._errcode(rattrs) == 400
    c.close()


def test_chandata_before_bind_dropped(turn):
    """ChannelData on a channel that was never bound is dropped and logged (RFC 8656 12)."""
    c = Client(turn.addr, "u", "p", REALM)
    c.allocate()
    c.channel_data(0x4001, b"early")  # 0x4001 was never bound
    time.sleep(0.3)  # let the server process the drop
    drops = [line for line in turn.server.log_lines if "drop ChannelData" in line and "not bound" in line]
    assert drops, "no drop logged for ChannelData before ChannelBind:\n" + "\n".join(turn.server.log_lines)
    c.close()


def test_channel_bind_rebind_different_addr_400(turn):
    """Re-binding a channel number to a different address is a 400 (RFC 8656 12)."""
    c = Client(turn.addr, "u", "p", REALM)
    c.allocate()
    mt1, _ = c.channel_bind(0x4001, ("127.0.0.1", 1000))
    assert mt1 == ft.T_CHANNEL_BIND_SUCCESS
    mt2, rattrs = c.channel_bind(0x4001, ("127.0.0.1", 2000))  # same number, different address
    assert mt2 == ft.T_CHANNEL_BIND_ERROR
    assert c._errcode(rattrs) == 400
    c.close()


def test_retry_reusing_the_transaction_id_is_400(turn):
    """After a 401, an authenticated retry under the same transaction id is a 400 (Cloudflare
    does this; the relay client once reused the id, 2026-11-09). A byte-identical retransmit is fine."""
    c = Client(turn.addr, "u", "p", REALM)
    txn = os.urandom(12)
    first = ft.build_msg(ft.T_ALLOCATE_REQ, txn, [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))])
    c.sock.sendto(first, turn.addr)
    mt, _, rattrs = c._drain_for(txn)
    assert c._errcode(rattrs) == 401
    c.sock.sendto(first, turn.addr)  # a retransmit: answered as before
    mt, _, rattrs = c._drain_for(txn)
    assert c._errcode(rattrs) == 401
    n = ft.get1(rattrs, ft.A_NONCE).decode()
    make = lambda t: [(ft.A_REQUESTED_TRANSPORT, bytes([ft.TRANSPORT_UDP]))]
    c.sock.sendto(c._auth_msg(ft.T_ALLOCATE_REQ, txn, make(txn), n, REALM), turn.addr)
    mt, _, rattrs = c._drain_for(txn)
    assert mt == ft.T_ALLOCATE_ERROR
    assert c._errcode(rattrs) == 400
    c.close()


def test_channel_number_must_be_4_bytes(turn):
    """RFC 8656 18.1: a 16-bit number and 2 RFFU bytes; Cloudflare answered 400 to the 2-byte form."""
    c = Client(turn.addr, "u", "p", REALM)
    assert c.allocate()[0] == ft.T_ALLOCATE_SUCCESS
    cb = lambda txn: [
        (ft.A_CHANNEL_NUMBER, struct.pack("!H", 0x4002)),
        (ft.A_XOR_PEER, ft.enc_addr(1, "127.0.0.1", 1, txn)),
    ]
    assert c._send_auth(ft.T_CHANNEL_BIND_REQ, cb, c.last_nonce, REALM)[0] == ft.T_CHANNEL_BIND_ERROR
    c.close()
