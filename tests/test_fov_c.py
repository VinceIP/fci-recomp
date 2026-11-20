"""Unit test for display-fov (Q71): the camera-update and perspective-builder prehooks in fci_fov.c,
run natively. The vectors and what they check are listed in tests/fov_test.c's header: the camera's
FOV field (radians, pi/2 by default) is scaled by target / 67.5 only on Camera_BuildViews' calls,
once per value, clamped; the builder scales only the display aspect.

    pytest tests/test_fov_c.py
"""

import os
from pathlib import Path

import pytest

import cc_helper

REPO = cc_helper.REPO
CC = cc_helper.find_cc()

# name: (RECOMP_FCI_FOV, FCI_TEST_EXPECT, FCI_TEST_SEED, RECOMP_FCI_TEST_ASPECT)
# FCI_TEST_EXPECT is the camera field (radians) after the run; FCI_TEST_SEED the field seeded first
# (default pi/2). 'cam-target' and 'aspect' run a full scenario in the C driver.
VECTORS = {
    "cam-unset": (None, "1.5707964", None, None),
    "cam-zero": ("0", "1.5707964", None, None),
    "cam-target": ("60", None, None, None),
    "cam-clamp-low": ("20", "0.9308423", None, None),
    "cam-clamp-high": ("200", "2.5598166", None, None),
    "cam-clamp-result": ("110", "2.97", "2.0", None),
    "cam-nonpos": ("60", "0", "0", None),
    "cam-toolarge": ("60", "3.1", "3.1", None),
    "cam-other": ("60", "1.5707964", None, None),
    "aspect": (None, None, None, "1.6"),
}


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_fov_c(tmp_path: Path) -> None:
    exe = tmp_path / "fov_test.exe"
    sources = [REPO / "src" / "fci_fov.c", REPO / "tests" / "fov_test.c"]
    includes = [REPO / "tests" / "fov_mock", REPO / "src"]
    evo = ["-DFCI_TEST_EVO", '-DFCI_TITLE_NAME="Far Cry Instincts Evolution"']
    for title, extra in (("instincts", []), ("evolution", evo)):
        build, _ = cc_helper.build_run(sources, exe, cc=CC, includes=includes, flags=cc_helper.FLAGS + extra)
        assert build.returncode == 0, f"the display-fov check failed to compile ({title}):\n" + build.stderr
        run_vectors(exe, title)

    print(f"{len(VECTORS)} checks per title, 0 failed")


def run_vectors(exe: Path, title: str) -> None:
    for name, (fov, expect, seed, aspect) in VECTORS.items():
        env = dict(os.environ)
        env.pop("RECOMP_FCI_FOV", None)
        env.pop("FCI_TEST_EXPECT", None)
        env.pop("FCI_TEST_SEED", None)
        env.pop("RECOMP_FCI_TEST_ASPECT", None)
        if fov is not None:
            env["RECOMP_FCI_FOV"] = fov
        if expect is not None:
            env["FCI_TEST_EXPECT"] = expect
        if seed is not None:
            env["FCI_TEST_SEED"] = seed
        if aspect is not None:
            env["RECOMP_FCI_TEST_ASPECT"] = aspect
        run = __import__("subprocess").run([str(exe), name], env=env, capture_output=True, text=True)
        line = [ln for ln in run.stdout.splitlines() if ln.startswith(("ok ", "FAIL "))]
        line = line[0] if line else "(no result line)\n" + run.stdout + run.stderr
        assert run.returncode == 0 and line.startswith("ok"), f"{title} {name} failed:\n{line}"


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
