"""Integration test for the TURN relay client: turn_probe.exe against tools/fake_turn.py.

The probe is a Windows console exe built from src/fci_turn.c (the build line is in its header);
set FCI_TURN_PROBE to its path to run this test. It checks the RFC 5769 self-test, then a relay
echo: a plain UDP "host" socket sends a frame to the probe's relayed address and the probe
reports the frame; the host then closes and rebinds to a new port and sends again, and the
probe's Data-indication rebind must report the frame from the new port.

    FCI_TURN_PROBE=/path/to/turn_probe.exe pytest tests/test_turn_probe.py
"""

import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

import pytest

REPO = Path(__file__).resolve().parents[1]
PROBE = os.environ.get("FCI_TURN_PROBE", "")
FAKE_TURN = REPO / "tools" / "fake_turn.py"
# Cloudflare-length credentials (64 hex each): user:realm:pass is about 150 bytes, past one MD5
# block, which a short "probe"/"secret" pair never exercised (2026-11-09)
USER = "5f" * 32
PASS = "a3" * 32


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_probe(args, timeout=60):
    return subprocess.run([PROBE] + args, capture_output=True, text=True, timeout=timeout)


def test_probe_exists():
    if not PROBE:
        pytest.skip("FCI_TURN_PROBE is not set (build tools/turn_probe.c first)")
    assert Path(PROBE).is_file()


def test_selftest():
    if not PROBE:
        pytest.skip("FCI_TURN_PROBE is not set")
    r = run_probe(["--selftest"], timeout=30)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "selftest ok" in r.stdout


def test_relay_echo():
    if not PROBE:
        pytest.skip("FCI_TURN_PROBE is not set")
    port = free_port()
    server = subprocess.Popen(
        [sys.executable, str(FAKE_TURN), "--port", str(port), "--user", USER, "--pass", PASS],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    # the host: a plain UDP socket the server has no allocation for (its frames arrive as
    # Data indications, like a real host before it allocates)
    host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    host.bind(("127.0.0.1", 0))
    hport = host.getsockname()[1]
    probe_p = None
    try:
        # the probe's Allocate retransmits (500 ms x 7) cover the server's startup; the
        # "ready relayed=" line is the real signal that the relay is up
        probe_p = subprocess.Popen(
            [PROBE, f"127.0.0.1:{port}", USER, PASS, "25", f"127.0.0.1:{hport}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        lines = []
        threading.Thread(
            target=lambda: [lines.append(line.rstrip()) for line in probe_p.stdout],
            daemon=True,
        ).start()

        def wait_for(needle, timeout=15):
            deadline = time.time() + timeout
            while time.time() < deadline:
                if any(needle in line for line in lines):
                    return True
                time.sleep(0.05)
            return False

        assert wait_for("ready relayed="), "the probe never became ready: " + "\n".join(lines)
        relayed = [line for line in lines if "ready relayed=" in line][0].split("ready relayed=")[1]
        relport = int(relayed.split(":")[1])

        host.sendto(b"hello-relay", ("127.0.0.1", relport))
        assert wait_for("frame 1:"), "no frame arrived: " + "\n".join(lines)
        l1 = [line for line in lines if "frame 1:" in line][0]
        assert f"from 127.0.0.1:{hport}" in l1
        # the echo goes back through the relay: the host sees it from the relayed address
        host.settimeout(5)
        data, src = host.recvfrom(2048)
        assert (data, src) == (b"hello-relay", ("127.0.0.1", relport))

        # the host rebinds to a new port: the probe must rebind its channel and report the
        # frame from the new port
        host.close()
        # the OS may hand back the same port; the test needs a genuinely new one
        for _ in range(20):
            host2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            host2.bind(("127.0.0.1", 0))
            hport2 = host2.getsockname()[1]
            if hport2 != hport:
                break
            host2.close()
        assert hport2 != hport
        host2.sendto(b"hello-again", ("127.0.0.1", relport))
        assert wait_for("frame 2:"), "no rebound frame: " + "\n".join(lines)
        l2 = [line for line in lines if "frame 2:" in line][0]
        assert f"from 127.0.0.1:{hport2}" in l2
        # the rebound channel carries the echo to the new port
        host2.settimeout(5)
        data, src = host2.recvfrom(2048)
        assert (data, src) == (b"hello-again", ("127.0.0.1", relport))
        host2.close()
    finally:
        if probe_p:
            probe_p.kill()
            probe_p.wait(timeout=10)
        server.kill()
        server.wait(timeout=10)
        host.close()
