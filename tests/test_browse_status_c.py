"""Unit test for the browse status logic (Q29 round 4): the "NO MATCHES FOUND" relabel cause.

Compiles the shipping source (src/fci_browse_status.c) with the host compiler against the test
driver (tests/browse_status_test.c) and runs it: the driver computes the browse status from
various NIC/worker states and asserts the expected enum. The test fails if the precedence
changes or a branch is removed.

    pytest tests/test_browse_status_c.py
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
    "none-not-in-list",
    "none-in-list-off",
    "none-never-fetched",
    "version",
    "version-over-relay",
    "version-over-fetch-fail",
    "version-never-fetched",
    "offline",
    "offline-pending",
    "offline-dropped",
    "offline-relay",
    "searching-never",
    "searching-never-dropped",
    "searching-never-relay",
    "empty",
    "empty-relay",
    "pending",
    "pending-1",
    "dropped",
    "dropped-only",
    "relay",
    "relay-dropped",
    "relay-pending",
    "all-connected",
    "prec-relay-dropped-pending",
    "prec-dropped-pending",
    "prec-pending-only",
]


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_browse_status_c(tmp_path: Path) -> None:
    exe = tmp_path / "browse_status_test.exe"
    cc = [
        CC,
        "-std=gnu11",
        "-O2",
        "-Wall",
        "-Werror",
        "-I",
        str(REPO / "src"),
        str(REPO / "src" / "fci_browse_status.c"),
        str(REPO / "tests" / "browse_status_test.c"),
        "-o",
        str(exe),
    ]
    build = subprocess.run(cc, capture_output=True, text=True)
    assert build.returncode == 0, "the browse status check failed to compile:\n" + build.stderr

    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, "the browse status check failed its vectors:\n" + run.stdout + run.stderr

    # every vector ran and was judged (a vector missing from the output is a silently skipped check)
    for name in VECTORS:
        found = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok   ", "FAIL ")) and f" {name}:" in ln]
        line = found[0] if found else None
        assert line is not None, f"no result line for the {name} vector:\n" + run.stdout
        assert line.startswith("ok"), line

    assert "27 checks, 0 failed" in run.stdout
