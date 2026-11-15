"""Unit test for display-fov (Q71): the perspective-builder prehook in fci_fov.c, run natively.

Compiles the shipping source (src/fci_fov.c) against a minimal mock of recomp_types.h
(tests/fov_mock/recomp_types.h) and the driver (tests/fov_test.c), and runs one process per
vector. The hook scales the fovy stack parameter by a fixed ratio (target / 67.5, both
vertical degrees; 67.5 is the game's on-foot vfov, fci_enhance.cpp:450). The driver seeds
the fovy at 1.1781 rad (~67.5 deg) and checks it after the prehook. The 'target' vector
also verifies stability (3 calls, no compounding) and zoom pickup (a new fovy is scaled,
then reset scales back).

Each vector is a separate run because the prehook caches the target on first use:
  unset      RECOMP_FCI_FOV absent: the fovy is left at 1.1781 (the game's own)
  zero       RECOMP_FCI_FOV=0: the fovy is left at 1.1781 (the game's own)
  target     RECOMP_FCI_FOV=60: 1.1781*(60/67.5)=1.047, stable x3; zoom to 2.0
             -> 2.0*(60/67.5)=1.778; reset to 1.1781 -> 1.047
  clamp-low  RECOMP_FCI_FOV=20: 1.1781*(40/67.5)=0.698 (clamped up to 40)
  clamp-high RECOMP_FCI_FOV=200: 1.1781*(110/67.5)=1.911 (clamped down to 110)

    pytest tests/test_fov_c.py
"""

import os
from pathlib import Path

import pytest

import cc_helper

REPO = cc_helper.REPO
CC = cc_helper.find_cc()

# name: (RECOMP_FCI_FOV, FCI_TEST_EXPECT)
# FCI_TEST_EXPECT is the fovy value after the run. The driver seeds the fovy at 1.1781
# (the game's on-foot vfov in radians); the hook scales it by target/67.5.
# The 'target' vector runs a full stability+zoom scenario in the C driver.
# name: (RECOMP_FCI_FOV, FCI_TEST_EXPECT, RECOMP_FCI_TEST_ASPECT)
VECTORS = {
    "unset": (None, "1.1781", None),
    "zero": ("0", "1.1781", None),
    "target": ("60", None, None),  # the C driver handles the full scenario
    "clamp-low": ("20", "0.698133", None),
    "clamp-high": ("200", "1.919867", None),
    # the display aspect: the aspect argument (g_esp + 12) is scaled by A / (16/9) when A is not 16:9;
    # the C driver checks the scaling (A = 16/10) and that a square projection is left alone
    "aspect": (None, None, "1.6"),
}


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_fov_c(tmp_path: Path) -> None:
    exe = tmp_path / "fov_test.exe"
    sources = [REPO / "src" / "fci_fov.c", REPO / "tests" / "fov_test.c"]
    includes = [REPO / "tests" / "fov_mock", REPO / "src"]
    build, _ = cc_helper.build_run(sources, exe, cc=CC, includes=includes)
    assert build.returncode == 0, "the display-fov check failed to compile:\n" + build.stderr

    for name, (fov, expect, aspect) in VECTORS.items():
        env = dict(os.environ)
        env.pop("RECOMP_FCI_FOV", None)
        env.pop("RECOMP_FCI_TEST_ASPECT", None)
        if expect is not None:
            env["FCI_TEST_EXPECT"] = expect
        if fov is not None:
            env["RECOMP_FCI_FOV"] = fov
        if aspect is not None:
            env["RECOMP_FCI_TEST_ASPECT"] = aspect
        run = __import__("subprocess").run([str(exe), name], env=env, capture_output=True, text=True)
        line = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok ", "FAIL "))]
        line = line[0] if line else "(no result line)\n" + run.stdout + run.stderr
        assert run.returncode == 0 and line.startswith("ok"), f"{name} failed:\n{line}"

    print("6 checks, 0 failed")


def test_fov_config_key() -> None:
    """the fci.ini display-fov key maps to RECOMP_FCI_FOV in fci_config.c's keys table.

    fci_config.c is a Windows host file (it uses windows.h) and its translate() is static, so the
    mapping is checked at source level: the display-fov line in the keys[] table names RECOMP_FCI_FOV.
    """
    src = (REPO / "src" / "fci_config.c").read_text(encoding="utf-8")
    line = [ln for ln in src.splitlines() if "display-fov" in ln]
    assert line, "no display-fov key in fci_config.c's keys table"
    assert "RECOMP_FCI_FOV" in line[0], "display-fov does not map to RECOMP_FCI_FOV: " + line[0]


def test_fov_ini_template() -> None:
    """the fci.ini template documents display-fov in the Display section."""
    ini = (REPO / "src" / "fci.ini").read_text(encoding="utf-8")
    assert "display-fov" in ini, "no display-fov key in the fci.ini template"
    assert ";display-fov = 0" in ini, "display-fov is not shown commented at its 0 default"
