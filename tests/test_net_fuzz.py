"""Cross-check for tools/net_fuzz.py's frame builder (Q24): the Python-built datagram bodies must
classify exactly as the card's own C check (src/fci_frame.c) judges them.

The fuzzer builds its hostile vectors in Python; this test compiles the shipping C check natively
(with the same driver as tests/test_frame_sanity.py, in its stdin mode) and pipes every vector
through it, asserting the expected reason for each. If the builder drifts from the C rule set (a
field at the wrong offset, a length off by one), the test fails.

    pytest tests/test_net_fuzz.py
"""

from __future__ import annotations

from pathlib import Path
import socket
import subprocess

import pytest

import cc_helper
from net_fuzz import (
    ADMIT_RE,
    ETHER_MAX,
    FUZZ_MAC,
    GATE_EXPECT,
    MAGIC,
    Fuzzer,
    build_frame,
    knock,
    valid_frame,
)

# the reason names in fci_frame.h order (FCI_FRAME_SHORT .. FCI_FRAME_PAD), as the C's summary
# line prints them (src/fci_nic.c gate_summary iterates i = 1 .. FCI_FRAME_NREASONS-1)
REASON_ORDER = [
    "short",
    "long",
    "ethertype",
    "ip-version",
    "ip-header",
    "ip-total-length",
    "fragment",
    "not-udp",
    "udp-length",
    "padding",
]
# the admit line exactly as the card's C prints it (src/fci_nic.c: the MAC in upper case); the
# recorded instance is from build/qwen-in/q24/host-fuzz-stderr.log
ADMIT_LINE = "[FCI] network: LAN admit 02:46:43:FA:11:22 <- 192.168.1.144:59503"

REPO = cc_helper.REPO
CC = cc_helper.find_cc()

# the reason codes, as src/fci_frame.h numbers them (kept in sync by this test)
OK = 0  # FCI_FRAME_OK
SHORT = 1
LONG = 2
ETHER = 3
IPVER = 4
IPLEN = 5
IPTOT = 6
FRAG = 7
PROTO = 8
UDPLEN = 9
PAD = 10


@pytest.fixture(scope="module")
def judge(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Compile the C check (shipping source) with its stdin-mode driver; return the binary."""
    exe = tmp_path_factory.mktemp("fuzzjudge") / "frame_sanity_test"
    build, _ = cc_helper.build_run([REPO / "src" / "fci_frame.c", REPO / "tests" / "frame_sanity_test.c"], exe, cc=CC)
    assert build.returncode == 0, "the frame sanity check failed to compile:\n" + build.stderr
    return exe


def run_vectors(judge: Path, vectors: list[tuple[str, bytes, int]]) -> list[str]:
    """Pipe the vectors through the C check; return its output lines (one per vector)."""
    stdin = "\n".join(f"{name} {data.hex()}" for name, data, _ in vectors) + "\n"
    run = subprocess.run(
        [str(judge), "stdin"],
        input=stdin,
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert run.returncode == 0, run.stdout + run.stderr
    lines = [ln for ln in run.stdout.splitlines() if ln and not ln.endswith("judged")]
    assert len(lines) == len(vectors), f"{len(lines)} judged, {len(vectors)} sent:\n" + run.stdout
    return lines


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_knock_shape(judge: Path) -> None:
    """The LAN knock: 17 bytes, magic + 'H' + the fuzzer's MAC + a 6-byte triple."""
    k = knock(b"\x01\x00\x00\x00\x00\x02")
    assert len(k) == 17
    assert k[:4] == MAGIC and k[4] == ord("H")
    assert k[5:11] == FUZZ_MAC
    assert k[11:17] == b"\x01\x00\x00\x00\x00\x02"


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_valid_frame_is_accepted(judge: Path) -> None:
    """The fuzzer's 66-byte game frame is exactly the recorded shape: the C check accepts it."""
    f = valid_frame(socket.inet_aton("192.168.1.99"), socket.inet_aton("192.168.1.50"))
    assert len(f) == 66
    lines = run_vectors(judge, [("fuzz-valid-66", f, OK)])
    assert lines[0] == "fuzz-valid-66 0 ok", lines


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_gate_vectors_classify_as_expected(judge: Path) -> None:
    """Every hostile gate vector classifies as the fuzzer's phase_gate expects (its reason list)."""
    src = socket.inet_aton("192.168.1.99")
    dst = socket.inet_aton("192.168.1.50")
    vectors: list[tuple[str, bytes, int]] = [
        ("short-14", build_frame(FUZZ_MAC, src, dst, eth_len=14), IPTOT),  # ip-total-length
        ("short-20", build_frame(FUZZ_MAC, src, dst, eth_len=20), IPTOT),
        ("arp", build_frame(FUZZ_MAC, src, dst, eth_type=b"\x08\x06"), ETHER),  # ethertype
        ("ipv6-type", build_frame(FUZZ_MAC, src, dst, eth_type=b"\x86\xdd"), ETHER),
        ("ip-v6", build_frame(FUZZ_MAC, src, dst, version=6), IPVER),  # ip-version
        ("ihl-15", build_frame(FUZZ_MAC, src, dst, ihl=15), IPLEN),  # ip-header
        ("totlen-500", build_frame(FUZZ_MAC, src, dst, totlen=500), IPTOT),  # ip-total-length
        ("totlen-10", build_frame(FUZZ_MAC, src, dst, totlen=10), IPTOT),
        ("frag-mf", build_frame(FUZZ_MAC, src, dst, flags_frag=0x2000), FRAG),  # fragment (MF)
        ("frag-off", build_frame(FUZZ_MAC, src, dst, flags_frag=100), FRAG),
        ("icmp", build_frame(FUZZ_MAC, src, dst, proto=1), PROTO),  # not-udp
        ("udplen-100", build_frame(FUZZ_MAC, src, dst, totlen=52, udp_len=100), UDPLEN),  # udp-length
        ("udplen-4", build_frame(FUZZ_MAC, src, dst, totlen=52, udp_len=4), UDPLEN),
        ("overpad-120", build_frame(FUZZ_MAC, src, dst, eth_len=120), PAD),  # padding
    ]
    lines = run_vectors(judge, vectors)
    for (name, _, want), line in zip(vectors, lines):
        got_code = int(line.split()[1])
        assert got_code == want, f"{name}: want reason {want}, got {line}"


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_envelope_vectors_classify_as_expected(judge: Path) -> None:
    """The envelope's frame-size vectors: the C check's SHORT and LONG agree with the card's
    envelope bounds (n < 5+14 and n > 5+ETHER_MAX in src/fci_nic.c)."""
    vectors: list[tuple[str, bytes, int]] = [
        ("env-13", b"\x00" * 13, SHORT),
        ("env-1515", b"\x00" * (ETHER_MAX + 1), LONG),
        ("env-garbage-100", b"\x00" * 100, ETHER),  # zeros carry no IPv4 EtherType
    ]
    lines = run_vectors(judge, vectors)
    for (name, _, want), line in zip(vectors, lines):
        assert int(line.split()[1]) == want, f"{name}: want {want}, got {line}"


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_valid_sizes_are_accepted(judge: Path) -> None:
    """The recorded traffic sizes (66, 342, 443) all pass: the fuzzer's rules never drop the
    game's own frames (the net_check run is the live proof; this is the static half)."""
    src = socket.inet_aton("192.168.1.99")
    dst = socket.inet_aton("192.168.1.50")
    vectors: list[tuple[str, bytes, int]] = [
        ("size-66", valid_frame(src, dst), OK),
        ("size-342", build_frame(FUZZ_MAC, src, dst, ip_payload=b"\x00" * 300), OK),
        ("size-443", build_frame(FUZZ_MAC, src, dst, ip_payload=b"\x00" * 401), OK),
    ]
    lines = run_vectors(judge, vectors)
    for (name, _, _want), line in zip(vectors, lines):
        assert line == f"{name} 0 ok", line


def test_admit_regex_matches_recorded_line() -> None:
    """The admit-line matcher must match the card's exact line (the MAC in upper case, as the C
    prints it). A case mismatch (the Q24 review's rc-2 bug) fails here, not against a live card."""
    assert ADMIT_RE.search(ADMIT_LINE), f"ADMIT_RE does not match the recorded admit line: {ADMIT_LINE}"
    # and it must not match a lower-case MAC (the old, broken matcher)
    assert not ADMIT_RE.search(ADMIT_LINE.upper().replace("FA:11:22", "fa:11:22"))


def test_summary_line_parses_in_c_format() -> None:
    """The counters-line parser must read a line in the C's exact format (src/fci_nic.c
    gate_summary). The line is built the way the C prints it: envelope, rate cap, bad frame total,
    every reason by fci_frame_reason() name in fci_frame.h order, then stranger and passed."""
    counts = {r: GATE_EXPECT.get(r, 0) for r in REASON_ORDER}
    bad_total = sum(counts.values())
    line = (
        f"envelope 9, rate cap 100, bad frame {bad_total} ("
        + ", ".join(f"{r} {counts[r]}" for r in REASON_ORDER)
        + "), stranger 3, passed 205"
    )
    d = Fuzzer.parse_summary(line)
    assert d["envelope"] == 9, d
    assert d["rate cap"] == 100, d
    assert d["bad frame"] == bad_total == 14, d
    for r in REASON_ORDER:
        assert d[r] == counts[r], f"{r}: want {counts[r]}, got {d.get(r)} in {d}"
    assert d["stranger"] == 3, d
    assert d["passed"] == 205, d


def test_summary_reasons_cover_gates_expect() -> None:
    """GATE_EXPECT's reasons are exactly the fci_frame.h reason names (no typo, no stray key): a
    mistyped reason would silently read a 0 counter against a live card."""
    assert set(GATE_EXPECT) == set(REASON_ORDER) - {"short", "long"}, GATE_EXPECT
    assert sum(GATE_EXPECT.values()) == 14, sum(GATE_EXPECT.values())
