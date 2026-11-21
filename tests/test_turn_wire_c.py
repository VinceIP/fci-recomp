"""Unit test for the TURN wire protocol (Q43): the card's own C code, run natively.

Compiles the shipping source (src/fci_turn_wire.c) with the host compiler against the test
driver (tests/turn_wire_test.c) and runs it: the driver rebuilds the RFC 5769 2.1 request
byte for byte with fci_tw_build, parses the 2.2/2.3/2.4 samples with fci_tw_parse (verifying
MESSAGE-INTEGRITY with the keys the RFC documents), pins the embedded crypto to known
digests (MD5, SHA-1, the RFC 2202 HMAC-SHA1 case, CRC-32), round-trips the client's 401,
438 and Allocate-success messages with integrity, and rejects the hostile inputs
(truncated message, attribute past the end, wrong cookie, corrupted integrity). The test
fails if the wire code's byte order, the length-field convention of MESSAGE-INTEGRITY or
FINGERPRINT, or a digest changes.

    pytest tests/test_turn_wire_c.py
"""

from pathlib import Path

import pytest

import cc_helper

REPO = cc_helper.REPO
CC = cc_helper.find_cc()

VECTORS = [
    "crypto-md5",
    "crypto-sha1",
    "crypto-hmac",
    "crypto-crc32",
    "xaddr-vector",
    "xaddr-roundtrip",
    "rfc5769-2-1-build",
    "rfc5769-2-2-parse",
    "rfc5769-2-3-parse",
    "rfc5769-2-4-parse",
    "mi-corrupt",
    "err-401",
    "err-438",
    "allocate-ok",
    "trunc-10",
    "attr-trunc",
    "wrong-cookie",
]


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_turn_wire_c(tmp_path: Path) -> None:
    exe = tmp_path / "turn_wire_test.exe"
    build, run = cc_helper.build_run(
        [REPO / "src" / "fci_turn_wire.c", REPO / "tests" / "turn_wire_test.c"], exe, cc=CC
    )
    assert build.returncode == 0, "the TURN wire check failed to compile:\n" + build.stderr
    assert run.returncode == 0, "the TURN wire check failed its vectors:\n" + run.stdout + run.stderr

    # every vector ran and was judged (a vector missing from the output is a silently skipped check)
    for name in VECTORS:
        found = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok   ", "FAIL ")) and f" {name}:" in ln]
        line = found[0] if found else None
        assert line is not None, f"no result line for the {name} vector:\n" + run.stdout
        assert line.startswith("ok"), line

    assert "17 checks, 0 failed" in run.stdout
