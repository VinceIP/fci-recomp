"""Unit test for the frame sanity check (Q24): the card's own C code, run natively.

Compiles the shipping source (src/fci_frame.c) with the host compiler against the test driver
(tests/frame_sanity_test.c) and runs it: the driver builds the frames this game's XNET is known
to send (the recorded run's 342-byte DHCP, 443-byte System Link broadcast, 66-byte unicast game
frame, a 60-byte padded minimum) and hostile frames (truncated, oversized, non-IPv4 EtherTypes,
IP fragments, inconsistent length fields) and asserts fci_frame_sane accepts exactly the
legitimate ones. The test fails if the check is reverted or a rule changes.

    pytest tests/test_frame_sanity.py
"""

from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[1]
CC = shutil.which("cc") or shutil.which("gcc")

VECTORS = [
    "dhcp-342",
    "slp-443",
    "game-66",
    "padded-60",
    "max-1514",
    "ip-options-df",
    "truncated-10",
    "truncated-13",
    "oversized-1515",
    "arp-0806",
    "ipv6-86dd",
    "8023-length",
    "ip-version-6",
    "ihl-too-big",
    "totlen-oversize",
    "totlen-short",
    "too-short-for-ip",
    "fragment-mf",
    "fragment-offset",
    "icmp",
    "udp-len-long",
    "udp-len-short",
    "overpad-100",
]


@pytest.mark.skipif(not CC, reason="no host C compiler (cc/gcc) on this machine")
def test_frame_sanity(tmp_path: Path) -> None:
    exe = tmp_path / "frame_sanity_test"
    cc = [
        CC,
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Werror",
        "-I",
        str(REPO / "src"),
        str(REPO / "src" / "fci_frame.c"),
        str(REPO / "tests" / "frame_sanity_test.c"),
        "-o",
        str(exe),
    ]
    build = subprocess.run(cc, capture_output=True, text=True)
    assert build.returncode == 0, "the frame sanity check failed to compile:\n" + build.stderr

    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, "the frame sanity check failed its vectors:\n" + run.stdout + run.stderr

    # every vector ran and was judged (a vector missing from the output is a silently skipped check)
    for name in VECTORS:
        found = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok   ", "FAIL ")) and f" {name}:" in ln]
        line = found[0] if found else None
        assert line is not None, f"no result line for the {name} vector:\n" + run.stdout
        assert line.startswith("ok"), line

    assert "23 checks, 0 failed" in run.stdout
