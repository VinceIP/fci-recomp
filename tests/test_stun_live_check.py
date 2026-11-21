"""Loopback check for tools/stun_live_check.py (Q44): both halves against one stub STUN server.

A stub STUN server runs in a thread on 127.0.0.1 and answers every binding request with a fixed
XOR-MAPPED-ADDRESS (192.0.2.7:32768, from 192.0.2.0/24). The C driver (half A, the shipping parser)
and the minimal Python client (half B) are both pointed at it and must each report that exact
address. No internet: everything is on the loopback. If the C parser's XOR or the Python decode
drifts, the two disagree and the test fails.

    pytest tests/test_stun_live_check.py
"""

from pathlib import Path
import socket
import struct
import threading

import pytest

import cc_helper
import stun_live_check as slc

# the fixed answer the stub gives, from 192.0.2.0/24 (the TEST-NET-1 block, safe to print)
STUB_IP = "192.0.2.7"
STUB_PORT = 32768


def _response(txn: bytes, ip: str, port: int) -> bytes:
    """A binding response carrying one XOR-MAPPED-ADDRESS (RFC 8489 14.2) for ip:port."""
    ipb = socket.inet_aton(ip)
    value = (
        bytes([0x00, 0x01])
        + struct.pack("!H", port ^ (slc.MAGIC >> 16))
        + bytes(a ^ b for a, b in zip(ipb, slc.MAGIC_BYTES))
    )
    body = struct.pack("!HH", 0x0020, len(value)) + value
    return struct.pack("!HH", slc.BINDING_RESPONSE, len(body)) + slc.MAGIC_BYTES + txn + body


class StubStun:
    """A thread-driven STUN server on 127.0.0.1 that answers every binding request with a fixed
    XOR-MAPPED-ADDRESS."""

    def __init__(self, ip: str, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self._ip, self._port = ip, port
        self.sock.settimeout(0.2)
        self._stop = False

    def run(self):
        while not self._stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) >= 20 and data[0:2] == b"\x00\x01":
                self.sock.sendto(_response(data[8:20], self._ip, self._port), addr)

    def close(self):
        self._stop = True
        self.sock.close()


@pytest.fixture
def stub():
    s = StubStun(STUB_IP, STUB_PORT)
    t = threading.Thread(target=s.run, daemon=True)
    t.start()
    yield f"127.0.0.1:{s.port}"
    s.close()
    t.join(timeout=2)


@pytest.mark.skipif(not cc_helper.find_cc(), reason="no C compiler on this machine")
def test_both_halves_report_the_stub_address(tmp_path: Path, stub: str) -> None:
    exe = slc.build_driver(str(tmp_path), cc_helper.find_cc())
    assert slc.query_a(exe, stub) == (STUB_IP, STUB_PORT), "the C driver misread the stub's address"
    assert slc.query_b(stub) == (STUB_IP, STUB_PORT), "the Python client misread the stub's address"


@pytest.mark.skipif(not cc_helper.find_cc(), reason="no C compiler on this machine")
def test_main_passes_when_both_servers_agree(stub: str, capsys) -> None:
    rc = slc.main(["--a", stub, "--b", stub])
    out = capsys.readouterr().out
    assert rc == 0, "main should PASS (exit 0) when both servers report the same IP"
    assert "PASS" in out and STUB_IP in out
    # the address is printed, and that is the only place it goes (nothing is written to a file)
    assert f"{STUB_IP}:{STUB_PORT}" in out
