"""Unit test for the host list's JSON arrays: the shipping src/fci_json.c, run natively.

The guest reads the listing ({"hosts":[...]}) and the host its pending connects ({"connects":[...]})
with fci_json_array. Before 0.3.0-beta.1 the walker skipped every second object, so a guest saw every
other match and a host lost the second of two connects; and each object was a pointer into the whole
reply, so a lookup for a missing field ("relay") could read the next object's. The checks fail on both.

    pytest tests/test_json_c.py
"""

from pathlib import Path
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[1]
_BUNDLED = sorted(REPO.glob("toolchain/*/bin/clang.exe"))
CC = shutil.which("cc") or shutil.which("gcc") or (str(_BUNDLED[0]) if _BUNDLED else None)

CHECKS = ["three-hosts", "spaced-two-hosts", "connects-bounded", "empty", "no-array", "too-long-skipped", "cut-off"]


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_json_c(tmp_path: Path) -> None:
    exe = tmp_path / "json_test.exe"
    cc = [CC, "-std=gnu11", "-O2", "-Wall", "-Werror", "-I", str(REPO / "src")]
    cc += [str(REPO / "src" / "fci_json.c"), str(REPO / "tests" / "json_test.c"), "-o", str(exe)]
    build = subprocess.run(cc, capture_output=True, text=True)
    assert build.returncode == 0, "the JSON check failed to compile:\n" + build.stderr

    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, "the JSON check failed:\n" + run.stdout + run.stderr
    for name in CHECKS:
        line = next((ln for ln in run.stdout.splitlines() if ln.endswith(f" {name}:")), None)
        assert line is not None, f"no result line for {name}:\n" + run.stdout
        assert line.startswith("ok"), line
    assert f"{len(CHECKS)} checks, 0 failed" in run.stdout
