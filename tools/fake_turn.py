#!/usr/bin/env python3
"""A single-threaded UDP TURN server (RFC 8656 over RFC 8489 STUN).

Standard library only. Takes one user + password, enforces long-term auth
(401 challenge, then HMAC-SHA1 with key MD5(user:realm:pass)), and relays UDP
datagrams between the relayed addresses its clients allocate.

Supported methods:
  Binding (RFC 8489)          - address discovery, unauthenticated
  Allocate  (RFC 8656 4.4.1)  - mint a relayed address on --relay-ip
  Refresh   (RFC 8656 4.4.5)  - extend / release an allocation
  CreatePermission (4.4.4)    - grant permission to send to a peer IP
  ChannelBind (4.4.6)         - bind a channel number (0x4000-0x4FFF) to a peer
  ChannelData (4.4.7)         - data to a peer over a channel (4-byte header)
  Send (4.4.8) / Data (4.4.9) - data indications (no response, not authenticated)

Error responses use the right STUN code: 400 Bad Request, 401 Unauthenticated,
437 Allocation Mismatch, 438 Stale Nonce, 441 Wrong Credentials,
442 Unsupported Transport Protocol.

See research/qwen/notes/q17b-fake-turn.md for the design and the list of
places where Cloudflare's behaviour is guessed at.
"""

import argparse
import hashlib
import hmac
import os
import select
import socket
import struct
import sys
import time
import zlib

MAGIC = 0x2112A442
MAGIC_BYTES = struct.pack("!I", MAGIC)

# STUN message classes (the two interleaved class bits of the 16-bit message type).
CLS_REQ, CLS_IND, CLS_SUCCESS, CLS_ERROR = 0, 1, 2, 3

# STUN methods (RFC 8489 2, RFC 8656).
M_BINDING = 0x001
M_ALLOCATE = 0x003
M_REFRESH = 0x004
M_SEND = 0x006
M_DATA = 0x007
M_CREATE_PERMISSION = 0x008
M_CHANNEL_BIND = 0x009


def mtype(method, cls):
    """STUN message type (RFC 8489 5): the two class bits are interleaved with the method
    bits, so Allocate is 0x0003/0x0103/0x0113 and the Data indication is 0x0017."""
    return ((method & 0x0F80) << 2) | ((method & 0x0070) << 1) | (method & 0x000F) | ((cls & 2) << 7) | ((cls & 1) << 4)


def type_method(mtype_val):
    """The method of a STUN message type (RFC 8489 5)."""
    return (((mtype_val & 0x3E00) >> 9) << 7) | (((mtype_val & 0x0060) >> 5) << 4) | (mtype_val & 0x000F)


def type_class(mtype_val):
    """The class of a STUN message type (RFC 8489 5)."""
    return ((mtype_val & 0x0100) >> 7) | ((mtype_val & 0x0010) >> 4)


T_BINDING_REQ = mtype(M_BINDING, CLS_REQ)
T_BINDING_SUCCESS = mtype(M_BINDING, CLS_SUCCESS)
T_ALLOCATE_REQ = mtype(M_ALLOCATE, CLS_REQ)
T_ALLOCATE_SUCCESS = mtype(M_ALLOCATE, CLS_SUCCESS)
T_ALLOCATE_ERROR = mtype(M_ALLOCATE, CLS_ERROR)
T_REFRESH_REQ = mtype(M_REFRESH, CLS_REQ)
T_REFRESH_SUCCESS = mtype(M_REFRESH, CLS_SUCCESS)
T_REFRESH_ERROR = mtype(M_REFRESH, CLS_ERROR)
T_SEND_IND = mtype(M_SEND, CLS_IND)
T_DATA_IND = mtype(M_DATA, CLS_IND)
T_CREATE_PERM_REQ = mtype(M_CREATE_PERMISSION, CLS_REQ)
T_CREATE_PERM_SUCCESS = mtype(M_CREATE_PERMISSION, CLS_SUCCESS)
T_CREATE_PERM_ERROR = mtype(M_CREATE_PERMISSION, CLS_ERROR)
T_CHANNEL_BIND_REQ = mtype(M_CHANNEL_BIND, CLS_REQ)
T_CHANNEL_BIND_SUCCESS = mtype(M_CHANNEL_BIND, CLS_SUCCESS)
T_CHANNEL_BIND_ERROR = mtype(M_CHANNEL_BIND, CLS_ERROR)

# STUN attribute types (RFC 8489 18.3, RFC 8656 Table 5).
A_MAPPED = 0x0001
A_FINGERPRINT = 0x8028
A_USERNAME = 0x0006
A_MESSAGE_INTEGRITY = 0x0008
A_ERROR_CODE = 0x0009
A_CHANNEL_NUMBER = 0x000C
A_LIFETIME = 0x000D
A_REALM = 0x0014
A_NONCE = 0x0015
A_XOR_PEER = 0x0012
A_DATA = 0x0013
A_XOR_RELAYED = 0x0016
A_REQUESTED_TRANSPORT = 0x0019
A_XOR_MAPPED = 0x0020

TRANSPORT_UDP = 0x11
TRANSPORT_TCP = 0x06

CHANNEL_MIN, CHANNEL_MAX = 0x4000, 0x4FFF
PERMISSION_LIFETIME = 300
CHANNEL_LIFETIME = 600
MAX_DATAGRAM = 65535


def attr_value(code, phrase=""):
    """Build an ERROR-CODE attribute value (RFC 8489 14.8): two zero bytes, then the class
    (hundreds) and the number, then the reason phrase."""
    return bytes([0, 0, code // 100, code % 100]) + phrase.encode()


def enc_attr(atype, value):
    pad = (4 - (len(value) % 4)) % 4
    return struct.pack("!HH", atype, len(value)) + value + b"\x00" * pad


def enc_addr(family, ip, port, txn=b"", xor=True):
    """Encode an [ADDRESS] attribute value. family is 1 (IPv4) or 2 (IPv6)."""
    raw = socket.inet_aton(ip) if family == 1 else socket.inet_pton(socket.AF_INET6, ip)
    if xor:
        port ^= MAGIC >> 16  # most significant 16 bits of the magic cookie (RFC 8489 14.2)
        key = MAGIC_BYTES + txn
        raw = bytes(a ^ b for a, b in zip(raw, key))
    return struct.pack("!BBH", 0, family, port) + raw


def dec_addr(value, txn=b"", xor=True):
    """Decode an [ADDRESS] attribute value. Returns (ip, port) or None."""
    family = value[1]
    port = struct.unpack_from("!H", value, 2)[0]
    raw = value[4:]
    if family == 1:
        if xor:
            port ^= MAGIC >> 16
            raw = bytes(a ^ b for a, b in zip(raw, MAGIC_BYTES))
        ip = socket.inet_ntoa(raw)
    elif family == 2:
        if xor:
            port ^= MAGIC >> 16
            key = MAGIC_BYTES + txn
            raw = bytes(a ^ b for a, b in zip(raw, key))
        ip = socket.inet_ntop(socket.AF_INET6, raw)
    else:
        return None
    return ip, port


def parse_msg(data):
    """Parse a STUN message. Returns (type, txn_id, {attr: [values]}) or None."""
    if len(data) < 20:
        return None
    mtype_val, length = struct.unpack_from("!HH", data, 0)
    if struct.unpack_from("!I", data, 4)[0] != MAGIC:
        return None
    if len(data) < 20 + length:
        return None
    txn = data[8:20]
    attrs = {}
    off, end = 20, 20 + length
    while off + 4 <= end:
        atype, alen = struct.unpack_from("!HH", data, off)
        off += 4
        if off + alen > end:
            return None
        attrs.setdefault(atype, []).append(data[off : off + alen])
        off += alen + (4 - (alen % 4)) % 4
    if off != end:
        return None
    return mtype_val, txn, attrs


def build_msg(mtype_val, txn, attrs):
    body = b"".join(enc_attr(t, v) for t, v in attrs)
    return struct.pack("!HH", mtype_val, len(body)) + MAGIC_BYTES + txn + body


def get1(attrs, atype):
    vals = attrs.get(atype)
    return vals[0] if vals else None


# --- long-term credential auth (RFC 8489 9.2) ---------------------------------


def cred_key(user, realm, password):
    return hashlib.md5(f"{user}:{realm}:{password}".encode()).digest()


def add_integrity(msg, key):
    """Append a MESSAGE-INTEGRITY attribute (RFC 8489 14.5).

    The HMAC is over the message up to (not including) this attribute, with the header
    Length field set to the value that would end at it; the attribute is appended last.
    """
    mtype_val = struct.unpack_from("!H", msg, 0)[0]
    new_length = (len(msg) - 20) + 24  # Length that would end at MESSAGE-INTEGRITY
    check = msg[0:2] + struct.pack("!H", new_length) + msg[4:]
    mi = enc_attr(A_MESSAGE_INTEGRITY, hmac.new(key, check, hashlib.sha1).digest())
    return struct.pack("!HH", mtype_val, new_length) + msg[4:] + mi


def add_fingerprint(msg):
    """Append a FINGERPRINT attribute (RFC 8489 14.7).

    CRC-32 of the message up to (not including) this attribute, header Length set to
    end at it, XOR 0x5354554E. msg must already carry its MESSAGE-INTEGRITY value.
    """
    mtype_val = struct.unpack_from("!H", msg, 0)[0]
    new_length = (len(msg) - 20) + 8  # Length that would end at FINGERPRINT
    check = msg[0:2] + struct.pack("!H", new_length) + msg[4:]
    crc = (zlib.crc32(check) & 0xFFFFFFFF) ^ 0x5354554E
    fp = enc_attr(A_FINGERPRINT, struct.pack("!I", crc))
    return struct.pack("!HH", mtype_val, new_length) + msg[4:] + fp


def verify_integrity(key, data):
    """Walk the attributes and verify a MESSAGE-INTEGRITY.

    Returns (ok, bad_after_mi, has_fp): ok is True when MESSAGE-INTEGRITY is present
    and its HMAC (over the bytes before it, header length set to end at it, the value
    zeroed, per RFC 8489 14.5) matches; bad_after_mi is True when a non-FINGERPRINT
    attribute follows it; has_fp is True when a FINGERPRINT attribute is present.
    """
    if len(data) < 40 or struct.unpack_from("!I", data, 4)[0] != MAGIC:
        return False, False, False
    end = 20 + struct.unpack_from("!H", data, 2)[0]
    off, mi_value, mi_start, has_fp, bad_after = 20, None, 0, False, False
    while off + 4 <= end:
        atype, alen = struct.unpack_from("!HH", data, off)
        val_start, val_end = off + 4, off + 4 + alen
        if val_end > end:
            return False, False, False
        if mi_value is None:
            if atype == A_MESSAGE_INTEGRITY and alen == 20:
                mi_value, mi_start = data[val_start:val_end], val_start
        elif atype == A_FINGERPRINT:
            has_fp = True
        else:
            bad_after = True
        off = val_end + (4 - (alen % 4)) % 4
    if mi_value is None:
        return False, bad_after, has_fp
    mtype_val = struct.unpack_from("!H", data, 0)[0]
    check = struct.pack("!HH", mtype_val, mi_start) + data[4 : mi_start - 4]
    ok = hmac.compare_digest(hmac.new(key, check, hashlib.sha1).digest(), mi_value)
    return ok, bad_after, has_fp


def verify_fingerprint(data):
    """Verify a FINGERPRINT that is the last attribute (RFC 8489 14.7).

    CRC-32 of the message up to (not including) the FINGERPRINT, header Length set to
    end at it, XOR 0x5354554E.
    """
    if len(data) < 28:
        return False
    atype, alen = struct.unpack_from("!HH", data, -8)
    if atype != A_FINGERPRINT or alen != 4:
        return False
    fp_value = struct.unpack_from("!I", data, -4)[0]
    check = data[0:2] + struct.pack("!H", len(data) - 20) + data[4 : len(data) - 8]
    return (zlib.crc32(check) & 0xFFFFFFFF) ^ 0x5354554E == fp_value


# --- server -------------------------------------------------------------------


class Allocation:
    __slots__ = (
        "five_tuple",
        "relay_ip",
        "relay_port",
        "deadline",
        "username",
        "permissions",
        "perm_deadlines",
        "channels",
        "chan_deadlines",
        "allocate_txn",
        "allocate_success",
        "sock",
    )

    def __init__(self, five_tuple, relay_ip, relay_port, deadline, username, sock):
        self.five_tuple = five_tuple
        self.relay_ip = relay_ip
        self.relay_port = relay_port
        self.deadline = deadline
        self.username = username
        self.permissions = set()
        self.perm_deadlines = {}
        self.channels = {}
        self.chan_deadlines = {}
        self.allocate_txn = None
        self.allocate_success = None
        self.sock = sock


class TurnServer:
    def __init__(self, port, relay_ip, user, password, realm, lifetime, nonce_life, log):
        self.port = port
        self.relay_ip = relay_ip
        self.user = user
        self.password = password
        self.realm = realm
        self.lifetime = lifetime
        self.nonce_life = nonce_life
        self.log = log
        self.log_lines = []  # every note(), for the --log assertions in the tests
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.allocations = {}  # 5-tuple -> Allocation
        self.by_relay_port = {}  # relay port -> Allocation
        self.relay_socks = {}  # relay port -> socket
        self.nonces = {}  # nonce -> expiry (monotonic); reusable until it expires
        self._stop = False

    def note(self, msg):
        line = f"fake_turn: {msg}"
        self.log_lines.append(line)
        if self.log:
            print(line, flush=True)

    def mint_nonce(self):
        n = os.urandom(16).hex()
        self.nonces[n] = time.monotonic() + self.nonce_life
        return n

    def close(self):
        self._stop = True
        self.sock.close()
        for s in self.relay_socks.values():
            s.close()

    # -- allocation lifecycle -------------------------------------------------

    def new_relay_sock(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((self.relay_ip, 0))
        port = s.getsockname()[1]
        self.relay_socks[port] = s
        return s, port

    def release(self, alloc):
        self.allocations.pop(alloc.five_tuple, None)
        self.by_relay_port.pop(alloc.relay_port, None)
        self.relay_socks.pop(alloc.relay_port, None)
        if alloc.sock:
            alloc.sock.close()
        self.note(f"released {alloc.five_tuple[0]}:{alloc.five_tuple[1]} (relay :{alloc.relay_port})")

    # -- auth -----------------------------------------------------------------

    def auth_challenge(self):
        return [(A_REALM, self.realm.encode()), (A_NONCE, self.mint_nonce().encode())]

    def check_auth(self, attrs, data, alloc):
        """Return (True, key) on success, or (False, (code, extra_attrs)) on failure."""
        user = get1(attrs, A_USERNAME)
        if user is None:
            return False, (401, self.auth_challenge())
        user = user.decode("utf-8", "replace")
        realm = get1(attrs, A_REALM)
        nonce = get1(attrs, A_NONCE)
        if realm is None or nonce is None:
            return False, (401, self.auth_challenge())
        realm = realm.decode("utf-8", "replace")
        nonce = nonce.decode("utf-8", "replace")
        if realm != self.realm:
            return False, (401, self.auth_challenge())
        if nonce not in self.nonces or time.monotonic() >= self.nonces[nonce]:
            return False, (438, [(A_REALM, self.realm.encode()), (A_NONCE, self.mint_nonce().encode())])
        if user != self.user:
            return False, (401, self.auth_challenge())
        if alloc is not None and alloc.username is not None and user != alloc.username:
            return False, (441, [])
        key = cred_key(user, realm, self.password)
        ok, bad_after, has_fp = verify_integrity(key, data)
        if bad_after:
            return False, (400, [])
        if not ok:
            return False, (401, self.auth_challenge())
        if has_fp and not verify_fingerprint(data):
            return False, (400, [])
        return True, key

    # -- responses ------------------------------------------------------------

    def success(self, txn, mtype_success, attrs, key=None, fingerprint=False):
        msg = build_msg(mtype_success, txn, attrs)
        if key is not None:
            msg = add_integrity(msg, key)
            if fingerprint:
                msg = add_fingerprint(msg)
        return msg

    def error(self, txn, mtype_error, code, reason="", extra=()):
        return build_msg(mtype_error, txn, [(A_ERROR_CODE, attr_value(code, reason))] + list(extra))

    # -- method handlers (return bytes to send, or None) ----------------------

    def handle_binding(self, txn, src):
        ip, port = src
        family = 1 if ":" not in ip else 2
        return self.success(txn, T_BINDING_SUCCESS, [(A_XOR_MAPPED, enc_addr(family, ip, port, txn))])

    def handle_allocate(self, data, src, txn, attrs):
        transport = get1(attrs, A_REQUESTED_TRANSPORT)
        if transport is None:
            return self.error(txn, T_ALLOCATE_ERROR, 400, "REQUESTED-TRANSPORT required")
        if transport[0] != TRANSPORT_UDP:
            return self.error(txn, T_ALLOCATE_ERROR, 442, "Unsupported transport")
        ft = (src[0], src[1], self.relay_ip, self.port, TRANSPORT_UDP)
        existing = self.allocations.get(ft)
        if existing is not None:
            if existing.allocate_txn == txn:
                return existing.allocate_success  # retransmit: idempotent
            return self.error(txn, T_ALLOCATE_ERROR, 437, "Allocation mismatch")
        ok, res = self.check_auth(attrs, data, None)
        if not ok:
            code, extra = res
            return self.error(txn, T_ALLOCATE_ERROR, code, extra=extra)
        key = res
        lifetime = get1(attrs, A_LIFETIME)
        lifetime = self.lifetime if lifetime is None else struct.unpack("!I", lifetime)[0]
        sock, rport = self.new_relay_sock()
        alloc = Allocation(ft, self.relay_ip, rport, time.monotonic() + lifetime, None, sock)
        alloc.username = get1(attrs, A_USERNAME).decode("utf-8", "replace")
        self.allocations[ft] = alloc
        self.by_relay_port[rport] = alloc
        resp_attrs = [
            (A_XOR_RELAYED, enc_addr(1, self.relay_ip, rport, txn)),
            (A_LIFETIME, struct.pack("!I", lifetime)),
        ]
        alloc.allocate_success = self.success(txn, T_ALLOCATE_SUCCESS, resp_attrs, key, A_FINGERPRINT in attrs)
        alloc.allocate_txn = txn
        self.note(f"allocated {src[0]}:{src[1]} -> relay {self.relay_ip}:{rport}")
        return alloc.allocate_success

    def _need_alloc(self, src, txn, mtype_error):
        ft = (src[0], src[1], self.relay_ip, self.port, TRANSPORT_UDP)
        alloc = self.allocations.get(ft)
        if alloc is None:
            return None, self.error(txn, mtype_error, 437, "No such allocation")
        return alloc, None

    def handle_refresh(self, data, src, txn, attrs):
        alloc, err = self._need_alloc(src, txn, T_REFRESH_ERROR)
        if err:
            return err
        ok, res = self.check_auth(attrs, data, alloc)
        if not ok:
            code, extra = res
            return self.error(txn, T_REFRESH_ERROR, code, extra=extra)
        lifetime = get1(attrs, A_LIFETIME)
        if lifetime is None:
            return self.error(txn, T_REFRESH_ERROR, 400, "LIFETIME required")
        secs = struct.unpack("!I", lifetime)[0]
        if secs == 0:
            self.release(alloc)
            return self.success(
                txn, T_REFRESH_SUCCESS, [(A_LIFETIME, struct.pack("!I", 0))], res, A_FINGERPRINT in attrs
            )
        alloc.deadline = time.monotonic() + secs
        return self.success(
            txn, T_REFRESH_SUCCESS, [(A_LIFETIME, struct.pack("!I", secs))], res, A_FINGERPRINT in attrs
        )

    def handle_create_permission(self, data, src, txn, attrs):
        alloc, err = self._need_alloc(src, txn, T_CREATE_PERM_ERROR)
        if err:
            return err
        ok, res = self.check_auth(attrs, data, alloc)
        if not ok:
            code, extra = res
            return self.error(txn, T_CREATE_PERM_ERROR, code, extra=extra)
        peer = get1(attrs, A_XOR_PEER)
        if peer is None:
            return self.error(txn, T_CREATE_PERM_ERROR, 400, "XOR-PEER-ADDRESS required")
        decoded = dec_addr(peer, txn)
        if decoded is None:
            return self.error(txn, T_CREATE_PERM_ERROR, 400, "Bad peer address")
        alloc.permissions.add(decoded[0])
        alloc.perm_deadlines[decoded[0]] = time.monotonic() + PERMISSION_LIFETIME
        self.note(f"CreatePermission from {src[0]}:{src[1]} -> peer {decoded[0]}:{decoded[1]}")
        return self.success(txn, T_CREATE_PERM_SUCCESS, [], res, A_FINGERPRINT in attrs)

    def handle_channel_bind(self, data, src, txn, attrs):
        alloc, err = self._need_alloc(src, txn, T_CHANNEL_BIND_ERROR)
        if err:
            return err
        ok, res = self.check_auth(attrs, data, alloc)
        if not ok:
            code, extra = res
            return self.error(txn, T_CHANNEL_BIND_ERROR, code, extra=extra)
        chan = get1(attrs, A_CHANNEL_NUMBER)
        peer = get1(attrs, A_XOR_PEER)
        if chan is None or peer is None:
            return self.error(txn, T_CHANNEL_BIND_ERROR, 400, "CHANNEL-NUMBER/XOR-PEER required")
        num = struct.unpack("!H", chan[:2])[0]
        if not (CHANNEL_MIN <= num <= CHANNEL_MAX):
            return self.error(txn, T_CHANNEL_BIND_ERROR, 400, "Channel out of range")
        decoded = dec_addr(peer, txn)
        if decoded is None:
            return self.error(txn, T_CHANNEL_BIND_ERROR, 400, "Bad peer address")
        if num in alloc.channels and alloc.channels[num] != decoded:
            return self.error(txn, T_CHANNEL_BIND_ERROR, 400, "Channel bound to a different address")
        alloc.channels[num] = decoded
        alloc.chan_deadlines[num] = time.monotonic() + CHANNEL_LIFETIME
        self.note(f"ChannelBind 0x{num:04x} from {src[0]}:{src[1]} -> peer {decoded[0]}:{decoded[1]}")
        return self.success(txn, T_CHANNEL_BIND_SUCCESS, [], res, A_FINGERPRINT in attrs)

    def _forward(self, alloc, peer, payload):
        alloc.sock.sendto(payload, peer)

    def handle_send(self, src, txn, attrs):
        ft = (src[0], src[1], self.relay_ip, self.port, TRANSPORT_UDP)
        alloc = self.allocations.get(ft)
        if alloc is None:
            self.note(f"drop Send from {src[0]}:{src[1]}: no allocation")
            return  # indication with no allocation: silently ignore
        peer = get1(attrs, A_XOR_PEER)
        data = get1(attrs, A_DATA)
        if peer is None or data is None:
            self.note(f"drop Send from {src[0]}:{src[1]}: missing XOR-PEER-ADDRESS or DATA")
            return
        decoded = dec_addr(peer, txn)
        if decoded is None:
            self.note(f"drop Send from {src[0]}:{src[1]}: bad peer address")
            return
        if decoded[0] not in alloc.permissions:
            self.note(f"drop Send to {decoded[0]}:{decoded[1]} from {src[0]}:{src[1]}: no permission")
            return  # no permission: drop (RFC 8656 Figure 3)
        self.note(f"Send from {src[0]}:{src[1]} -> {decoded[0]}:{decoded[1]} ({len(data)} bytes)")
        self._forward(alloc, decoded, data)

    def handle_channel_data(self, src, data):
        # ChannelData is not STUN-formatted: 2-byte channel + 2-byte length + payload.
        if len(data) < 4:
            self.note(f"drop ChannelData from {src[0]}:{src[1]}: shorter than the 4-byte header")
            return
        num = struct.unpack("!H", data[:2])[0]
        length = struct.unpack("!H", data[2:4])[0]
        payload = data[4 : 4 + length]
        ft = (src[0], src[1], self.relay_ip, self.port, TRANSPORT_UDP)
        alloc = self.allocations.get(ft)
        if alloc is None:
            self.note(f"drop ChannelData 0x{num:04x} from {src[0]}:{src[1]}: no allocation")
            return  # ChannelData with no allocation: silently ignore
        peer = alloc.channels.get(num)
        if peer is None:
            self.note(f"drop ChannelData 0x{num:04x} from {src[0]}:{src[1]}: channel not bound")
            return  # no such channel: silently ignore
        self.note(f"ChannelData 0x{num:04x} from {src[0]}:{src[1]} -> {peer[0]}:{peer[1]} ({len(payload)} bytes)")
        self._forward(alloc, peer, payload)

    # -- incoming relayed data -> Data indication / ChannelData ---------------

    def handle_relayed_in(self, rport, payload, from_addr):
        alloc = self.by_relay_port.get(rport)
        if alloc is None:
            return
        sender_ip, sender_port = from_addr
        dest = (alloc.five_tuple[0], alloc.five_tuple[1])
        for num, (cip, cport) in alloc.channels.items():
            if cip == sender_ip and cport == sender_port:
                self.sock.sendto(struct.pack("!HH", num, len(payload)) + payload, dest)
                return
        txn = os.urandom(12)
        attrs = [(A_XOR_PEER, enc_addr(1, sender_ip, sender_port, txn)), (A_DATA, payload)]
        self.sock.sendto(build_msg(T_DATA_IND, txn, attrs), dest)

    # -- main loop ------------------------------------------------------------

    def run(self):
        self.note(f"listening on :{self.port}, relay ip {self.relay_ip}, user {self.user!r}")
        while not self._stop:
            self._expire()
            try:
                ready = select.select([self.sock] + list(self.relay_socks.values()), [], [], 0.5)[0]
            except OSError:
                break
            for s in ready:
                if self._stop:
                    break
                try:
                    if s is self.sock:
                        self.on_control()
                    else:
                        payload, from_addr = s.recvfrom(MAX_DATAGRAM)
                        self.handle_relayed_in(s.getsockname()[1], payload, from_addr)
                except OSError:
                    break

    def _expire(self):
        now = time.monotonic()
        for n in list(self.nonces):
            if now >= self.nonces[n]:
                self.nonces.pop(n, None)
        for ft in list(self.allocations):
            a = self.allocations[ft]
            if now >= a.deadline:
                self.release(a)
                continue
            for p in list(a.perm_deadlines):
                if now >= a.perm_deadlines[p]:
                    a.permissions.discard(p)
                    a.perm_deadlines.pop(p, None)
            for c in list(a.chan_deadlines):
                if now >= a.chan_deadlines[c]:
                    a.channels.pop(c, None)
                    a.chan_deadlines.pop(c, None)

    def on_control(self):
        try:
            data, src = self.sock.recvfrom(MAX_DATAGRAM)
        except OSError:
            return
        if len(data) >= 2:
            num = struct.unpack("!H", data[:2])[0]
            if CHANNEL_MIN <= num <= CHANNEL_MAX:
                self.handle_channel_data(src, data)
                return
        parsed = parse_msg(data)
        if parsed is None:
            self.note(f"dropped unparseable packet from {src[0]}:{src[1]} ({len(data)} bytes)")
            return
        mtype_val, txn, attrs = parsed
        if mtype_val == T_BINDING_REQ:
            out = self.handle_binding(txn, src)
        elif mtype_val == T_ALLOCATE_REQ:
            out = self.handle_allocate(data, src, txn, attrs)
        elif mtype_val == T_REFRESH_REQ:
            out = self.handle_refresh(data, src, txn, attrs)
        elif mtype_val == T_CREATE_PERM_REQ:
            out = self.handle_create_permission(data, src, txn, attrs)
        elif mtype_val == T_CHANNEL_BIND_REQ:
            out = self.handle_channel_bind(data, src, txn, attrs)
        elif mtype_val == T_SEND_IND:
            self.handle_send(src, txn, attrs)
            return
        elif mtype_val == T_DATA_IND:
            return  # Data is server->client only
        else:
            self.note(f"dropped unknown message type 0x{mtype_val:04x} from {src[0]}:{src[1]}")
            return
        if out is not None:
            self.sock.sendto(out, src)


def main(argv=None):
    p = argparse.ArgumentParser(description="Single-threaded UDP TURN server (RFC 8656).")
    p.add_argument("--port", type=int, default=3478)
    p.add_argument("--relay-ip", default="127.0.0.1")
    p.add_argument("--user", required=True)
    p.add_argument("--pass", dest="password", required=True)
    p.add_argument("--realm", default="turn.cloudflare.com")
    p.add_argument("--lifetime", type=int, default=600)
    p.add_argument("--nonce-life", type=int, default=600)
    p.add_argument("--log", action="store_true")
    args = p.parse_args(argv)
    server = TurnServer(
        args.port, args.relay_ip, args.user, args.password, args.realm, args.lifetime, args.nonce_life, args.log
    )
    try:
        server.run()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
