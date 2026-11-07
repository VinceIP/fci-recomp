"""The runtime patches compile: xboxrecomp's runtime with patches/runtime, built by tools/compile.py"""

import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

from conftest import toolkit_dir
import patches

REPO = Path(__file__).resolve().parents[1]


def test_patched_runtime_compiles(compiler, tmp_path):
    toolkit = toolkit_dir()
    if not (toolkit / "src").is_dir():
        pytest.skip(f"no xboxrecomp toolkit at {toolkit}")
    inputs = tmp_path / "case" / "inputs"
    for part in ("src", "include"):
        shutil.copytree(toolkit / part, inputs / "toolkit" / part)
    patches.apply_series("runtime", inputs / "toolkit")
    (inputs / "toolchain.json").write_text(json.dumps(compiler), encoding="utf-8")
    shutil.copy2(REPO / "tools" / "compile.py", inputs / "compile.py")
    done = subprocess.run(
        [sys.executable, "-B", str(inputs / "compile.py"), "cc", "runtime"],
        capture_output=True,
        text=True,
        timeout=1800,
    )
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-4000:]
    objects = list((tmp_path / "case" / "work" / "obj").glob("*.obj"))
    assert len(objects) >= 40
