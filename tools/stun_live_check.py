#!/usr/bin/env python3
"""Live STUN check (Q44): is the card's public address the same from two independent servers?

Server A is answered by the shipping C driver (tools/stun_live_check.c, which compiles the game's
own parser, src/fci_stun.c, in) and server B by a minimal Python binding request, so a parse bug in
one of them is cross-checked by the other. The default servers are the game's own STUN server
(stun.cloudflare.com:3478, src/fci_nic.c) and Google's public STUN server (stun.l.google.com:19302).
It prints both answers and PASSes when the IPs match (the ports are reported but may differ behind
some NATs, so only the IP is judged). It writes the address nowhere on disk; run it on the machine
whose public endpoint you want to check.

    python -B tools/stun_live_check.py [--a HOST:PORT] [--b HOST:PORT]
"""

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
if str(REPO / "tests") not in sys.path:  # cc_helper lives with the tests
    sys.path.insert(0, str(REPO / "tests"))

import cc_helper  # noqa: E402

# RFC 8489: the 32-bit magic cookie, and the binding message type (RFC 8489 5, 6.2).
MAGIC = 0x2112A442
MAGIC_BYTES = struct.pack("!I", MAGIC)
BINDING_REQUEST = 0x0001
BINDING_RESPONSE = 0x0101

# the two servers, both public STUN (host:port). A is the game's own default (src/fci_nic.c);
# B is an independent, well-known one.
DEFAULT_A = "stun.cloudflare.com:3478"
DEFAULT_B = "stun.l.google.com:19302"


def build_driver(tmpdir: str, cc: str) -> Path:
    """Compile the C driver (with src/fci_stun.c, the shipping parser) into a temp exe."""
    exe = Path(tmpdir) / ("stun_live_check.exe" if os.name == "nt" else "stun_live_check")
    cmd = [
        cc,
        *cc_helper.FLAGS,
        "-I",
        str(REPO / "src"),
        str(REPO / "src" / "fci_stun.c"),
        str(REPO / "tools" / "stun_live_check.c"),
        "-o",
        str(exe),
    ]
    if os.name == "nt":
        cmd.append("-lws2_32")
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        raise SystemExit("the STUN driver failed to compile:\n" + build.stderr)
    return exe


def query_a(exe: Path, server: str):
    """Run the C driver against server A; return (ip, port) or None. Only the `c <ip>:<port>`
    line is read; the address is never written to a file."""
    r = subprocess.run([str(exe), server], capture_output=True, text=True, timeout=15)
    for line in r.stdout.splitlines():
        if line.startswith("c "):
            ip, _, port = line[2:].partition(":")
            if ip and port:
                return ip, int(port)
    return None


# --- the minimal Python STUN client for server B (written from RFC 8489, not fake_turn.py) ---


def build_binding_request(txn: bytes) -> bytes:
    """A 20-byte STUN binding request: type, zero length, magic cookie, 12-byte transaction id
    (RFC 8489 5-6). No attributes."""
    return struct.pack("!HH", BINDING_REQUEST, 0) + MAGIC_BYTES + txn


def decode_xor_mapped(value: bytes):
    """Decode an XOR-MAPPED-ADDRESS value (RFC 8489 14.2): the port and the address are XORed
    with the most significant 32 bits of the magic cookie. Returns (ip, port) for IPv4, else None."""
    if value[1] != 0x01:  # only the IPv4 family (0x01) is checked
        return None
    port = struct.unpack("!H", value[2:4])[0] ^ (MAGIC >> 16)
    ip = bytes(a ^ b for a, b in zip(value[4:8], MAGIC_BYTES))
    return socket.inet_ntoa(ip), port


def parse_binding_response(data: bytes, txn: bytes):
    """Walk a binding response's attributes (XOR-MAPPED-ADDRESS 0x0020 preferred, MAPPED-ADDRESS
    0x0001 fallback) and return (ip, port), or None when it is not a matching response."""
    if len(data) < 20:
        return None
    mtype, _ = struct.unpack("!HH", data[0:4])
    if mtype != BINDING_RESPONSE or data[4:8] != MAGIC_BYTES or data[8:20] != txn:
        return None
    off = 20
    while off + 4 <= len(data):
        atype, alen = struct.unpack("!HH", data[off : off + 4])
        val = data[off + 4 : off + 4 + alen]
        if atype == 0x0020 and alen == 8:
            return decode_xor_mapped(val)
        if atype == 0x0001 and alen == 8:
            return socket.inet_ntoa(val[4:8]), struct.unpack("!H", val[2:4])[0]
        off += 4 + (alen + 3) & ~3  # attributes are padded to 4 bytes
    return None


def query_b(server: str):
    """Send one binding request to server B and decode its answer. Returns (ip, port) or None."""
    host, _, port = server.rpartition(":")
    if not host or not port:
        return None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        txn = os.urandom(12)
        sock.sendto(build_binding_request(txn), (host, int(port)))
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            sock.settimeout(max(0.0, deadline - time.monotonic()))
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                break
            res = parse_binding_response(data, txn)
            if res:
                return res
        return None
    finally:
        sock.close()


def fmt(ans):
    return f"{ans[0]}:{ans[1]}" if ans else "no answer"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="check the public STUN address against two servers")
    p.add_argument("--a", default=DEFAULT_A, help=f"server A, host:port (default {DEFAULT_A})")
    p.add_argument("--b", default=DEFAULT_B, help=f"server B, host:port (default {DEFAULT_B})")
    args = p.parse_args(argv)

    cc = cc_helper.find_cc()
    if not cc:
        raise SystemExit("no C compiler (cc, gcc or the bundled clang) on this machine")

    with tempfile.TemporaryDirectory() as tmp:
        exe = build_driver(tmp, cc)
        a = query_a(exe, args.a)
        b = query_b(args.b)

    print(f"A {args.a}: {fmt(a)}")
    print(f"B {args.b}: {fmt(b)}")
    if a and b and a[0] == b[0]:
        print(f"PASS: both servers report the public IP {a[0]} (ports {a[1]} vs {b[1]})")
        return 0
    print("FAIL: the two servers disagree (or one did not answer); the IP is the address to judge")
    return 1


if __name__ == "__main__":
    sys.exit(main())
