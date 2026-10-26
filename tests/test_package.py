import json
from pathlib import Path
import re
import sys
import zipfile

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import package  # noqa: E402


def test_release_zip_has_the_build_and_not_the_developer_files(tmp_path, capsys):
    assert package.main(["HEAD", "--out", str(tmp_path)]) == 0
    (zip_path,) = tmp_path.glob("fci-recomp-*.zip")
    assert zip_path.name == f"fci-recomp-{package.version('HEAD')}.zip"
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        assert {"BuildGame.cmd", "BuildEditor.cmd", "fci-recomp.py", "tools/package.py", "LICENSE"} <= set(names)
        assert not [n for n in names if n.split("/")[0] in package.EXCLUDE]
        assert package.LAUNCHER not in names
        assert z.read("BuildGame.cmd").startswith(b"@echo off\r\n")  # cmd.exe wants CRLF
    assert "sha256 " in capsys.readouterr().out


def test_release_zip_carries_the_launcher(tmp_path):
    exe = tmp_path / "fci-launcher.exe"
    exe.write_bytes(b"MZ" + b"\0" * 64)
    assert package.main(["HEAD", "--out", str(tmp_path / "out"), "--launcher", str(exe)]) == 0
    (zip_path,) = (tmp_path / "out").glob("fci-recomp-*.zip")
    with zipfile.ZipFile(zip_path) as z:
        assert z.read(package.LAUNCHER) == exe.read_bytes()
    exe.write_bytes(b"not an exe")
    with pytest.raises(SystemExit, match="not a Windows executable"):
        package.main(["HEAD", "--out", str(tmp_path / "out"), "--launcher", str(exe)])


def test_the_launcher_has_the_release_version():
    # the launcher updates itself from the release zip it comes in, and says it is newer by this number
    release = package.version("HEAD")
    tauri = REPO / "launcher" / "src-tauri"
    assert package.launcher_version("HEAD") == release
    assert re.search(r'^version = "([^"]+)"', (tauri / "Cargo.toml").read_text(), re.M).group(1) == release
    assert json.loads((tauri / "tauri.conf.json").read_text())["version"] == release
    lock = (tauri / "Cargo.lock").read_text()
    assert re.search(r'name = "fci-launcher"\nversion = "([^"]+)"', lock).group(1) == release


def test_the_launcher_knows_what_the_zip_holds(tmp_path):
    """the launcher's move copies only these from beside it (launcher/src-tauri/src/paths.rs RELEASE_ENTRIES)"""
    paths_rs = (REPO / "launcher/src-tauri/src/paths.rs").read_text(encoding="utf-8")
    listed = re.search(r"RELEASE_ENTRIES: \[&str; \d+\] = \[(.*?)\];", paths_rs, re.S).group(1)
    assert package.main(["HEAD", "--out", str(tmp_path)]) == 0
    (zip_path,) = tmp_path.glob("fci-recomp-*.zip")
    with zipfile.ZipFile(zip_path) as z:
        top = {n.split("/")[0] for n in z.namelist()}
    assert sorted(re.findall(r'"([^"]+)"', listed)) == sorted(top)
