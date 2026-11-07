"""fci-recomp.py keeps a finished lift or build only when it was made from the repository's present inputs"""

import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build  # noqa: E402
import lift  # noqa: E402
import titles  # noqa: E402

TC = {"release": "llvm-mingw 20261006 (LLVM 23.1.3)", "sha256": "a" * 64, "root": "C:/one", "bin": "C:/one/bin"}


def test_lift_is_current_only_with_the_same_inputs(tmp_path):
    fci, evo = titles.TITLES["instincts"], titles.TITLES["evolution"]
    assert not lift.current("default.xbe", fci, tmp_path)  # nothing there
    record = tmp_path / "lift.json"
    record.write_text(json.dumps({"inputs_sha256": lift.inputs_digest("default.xbe", fci)}))
    assert lift.current("default.xbe", fci, tmp_path)
    assert not lift.current("ige.xbe", fci, tmp_path)  # the editor's seeds and profile differ
    assert not lift.current("default.xbe", evo, tmp_path)  # another game's seeds and profile
    record.write_text(json.dumps({"inputs_sha256": "0" * 64}))  # made before an update
    assert not lift.current("default.xbe", fci, tmp_path)


def test_build_is_current_only_with_the_same_lift_and_compiler(tmp_path):
    lifted, built = tmp_path / "lift", tmp_path / "build"
    lifted.mkdir()
    (built / "bin").mkdir(parents=True)
    (lifted / "lift.json").write_text(json.dumps({"generated": {"recomp_0000.c": "a" * 64}}))
    (built / "build.json").write_text(json.dumps({"inputs_sha256": build.inputs_digest(lifted, TC)}))
    assert not build.current(built, lifted, TC)  # no executable
    (built / "bin" / "fci-whole.exe").write_bytes(b"MZ")
    assert build.current(built, lifted, TC)
    assert build.current(built, lifted, dict(TC, root="D:/moved", bin="D:/moved/bin"))  # the same compiler
    assert not build.current(built, lifted, dict(TC, sha256="b" * 64))
    (lifted / "lift.json").write_text(json.dumps({"generated": {"recomp_0000.c": "b" * 64}}))
    assert not build.current(built, lifted, TC)
