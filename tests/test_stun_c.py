"""Unit test for the STUN wire protocol (Q28): the card's own C code, run natively.

Compiles the shipping source (src/fci_stun.c) with the host compiler against the test driver
(tests/stun_test.c) and runs it: the driver builds the binding request and parses binding
responses (the RFC 5769 2.2 sample byte for byte, a MAPPED-ADDRESS-only response, padding,
and hostile inputs: wrong cookie, wrong transaction ID, wrong type, truncated message,
attribute length past the end) and asserts fci_stun_parse_response accepts exactly the
legitimate ones. The test fails if the XOR is reverted (pre-ba69b03) or a check changes.

    pytest tests/test_stun_c.py
"""

from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[1]
# the host compiler, or the bundled clang of a Windows checkout (so the test runs there too)
_BUNDLED = sorted(REPO.glob("toolchain/*/bin/clang.exe"))
CC = shutil.which("cc") or shutil.which("gcc") or (str(_BUNDLED[0]) if _BUNDLED else None)

VECTORS = [
    "build-request",
    "rfc5769-2-2",
    "mapped-only",
    "padding-odd",
    "wrong-cookie",
    "wrong-txn",
    "wrong-type",
    "truncated-10",
    "attr-trunc",
    "no-address",
]


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_stun_c(tmp_path: Path) -> None:
    exe = tmp_path / "stun_test.exe"
    cc = [
        CC,
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Werror",
        "-I",
        str(REPO / "src"),
        str(REPO / "src" / "fci_stun.c"),
        str(REPO / "tests" / "stun_test.c"),
        "-o",
        str(exe),
    ]
    build = subprocess.run(cc, capture_output=True, text=True)
    assert build.returncode == 0, "the STUN wire check failed to compile:\n" + build.stderr

    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, "the STUN wire check failed its vectors:\n" + run.stdout + run.stderr

    # every vector ran and was judged (a vector missing from the output is a silently skipped check)
    for name in VECTORS:
        found = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok   ", "FAIL ")) and f" {name}:" in ln]
        line = found[0] if found else None
        assert line is not None, f"no result line for the {name} vector:\n" + run.stdout
        assert line.startswith("ok"), line

    assert "10 checks, 0 failed" in run.stdout
