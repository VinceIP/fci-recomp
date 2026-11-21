"""The internet host list Worker (tools/hostlist), through Node's own test runner; skipped without Node."""

from pathlib import Path
import shutil
import subprocess

import pytest

TESTS = Path(__file__).resolve().parents[1] / "tools" / "hostlist" / "lobby.test.mjs"


def test_hostlist_worker():
    node = shutil.which("node")
    if not node:
        pytest.skip("no node on PATH")
    run = subprocess.run([node, "--test", str(TESTS)], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "fail 0" in run.stdout and "pass 0" not in run.stdout, run.stdout  # the tests ran, and all passed
