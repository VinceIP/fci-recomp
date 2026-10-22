from pathlib import Path
import sys
import zipfile

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
        assert z.read("BuildGame.cmd").startswith(b"@echo off\r\n")  # cmd.exe wants CRLF
    assert "sha256 " in capsys.readouterr().out
