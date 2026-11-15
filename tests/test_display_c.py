# the display options (fci_display.c): the aspect normalisation and the HUD anchor in one process, and the
# cached float aspect one process per vector (RECOMP_FCI_ASPECT). The driver compiles fci_display.c
# natively against a mock of fci_host_aspect (the glue's, extern "C").
import os
from pathlib import Path
import subprocess

import pytest

import cc_helper

REPO = cc_helper.REPO
CC = cc_helper.find_cc()


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_display_c(tmp_path: Path) -> None:
    exe = tmp_path / "display_test"
    sources = [REPO / "src" / "fci_display.c", REPO / "tests" / "display_test.c"]
    build, _ = cc_helper.build_run(sources, exe, cc=CC, includes=[REPO / "src"])
    assert build.returncode == 0, "the display check failed to compile:\n" + build.stderr

    # the in-process tests (normalise + anchor) run with no aspect env: the defaults
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, "the in-process tests failed:\n" + run.stdout + run.stderr
    assert "ok" in run.stdout

    # the cached float aspect: one process per value (fci_display_aspect is static per process)
    aspects = {"4:3": 4 / 3, "16:9": 16 / 9, "16:10": 16 / 10, "21:9": 21 / 9, "32:9": 32 / 9, "auto": 0.0, "": 4 / 3}
    for value in aspects:
        env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
        if value:
            env["RECOMP_FCI_ASPECT"] = value
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
        assert run.returncode == 0, f"aspect {value!r} failed:\n" + run.stdout + run.stderr
        assert "ok" in run.stdout

    print(f"{len(aspects) + 1} checks, 0 failed")
