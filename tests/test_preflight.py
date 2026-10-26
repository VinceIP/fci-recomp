"""tools/preflight.py and tools/deps.py's fetch: each missing requirement is said, with what fixes it"""

from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import deps  # noqa: E402
import preflight  # noqa: E402
import toolchain  # noqa: E402


def levels(found):
    return [level for level, _ in found]


def test_git_missing_is_a_warning_with_the_link(monkeypatch):
    monkeypatch.setattr(preflight.shutil, "which", lambda name: None)
    [(level, text)] = preflight.check_git()
    assert level == "Warning" and "git not found" in text and preflight.GIT_URL in text
    monkeypatch.setattr(preflight.shutil, "which", lambda name: "C:/git.exe")
    assert preflight.check_git() == []


def test_compiler_says_why_visual_studio_is_not_used(monkeypatch, tmp_path):
    def no_vs():
        raise toolchain.ToolchainError("No Visual Studio with the C++ x64 tools was found.")

    monkeypatch.setattr(toolchain, "find_msvc", no_vs)
    [(level, text)] = preflight.check_compiler(tmp_path)
    assert level == "Warning" and "No Visual Studio" in text and preflight.VS_URL in text
    # a finished download, or asking for one, needs nothing said
    monkeypatch.setattr(toolchain, "downloaded", lambda folder: {"msvc": "14.51"})
    assert preflight.check_compiler(tmp_path) == []
    assert preflight.check_compiler(tmp_path, download=True) == []


def test_space_counts_what_each_drive_needs(monkeypatch, tmp_path):
    free = {"value": 100 * preflight.GB}
    monkeypatch.setattr(preflight.shutil, "disk_usage", lambda drive: SimpleNamespace(free=free["value"]))
    assert preflight.check_space(tmp_path / "work", tmp_path / "install", 2, True) == []
    free["value"] = 3 * preflight.GB
    [(level, text)] = preflight.check_space(tmp_path / "work", tmp_path / "install", 2, True)
    # one drive for both: game files, two parts and the compiler
    assert level == "Warning" and "3.0 GB free" in text and "at least 6 GB" in text
    (tmp_path / "install" / "game").mkdir(parents=True)
    (tmp_path / "install" / "game" / "default.xbe").write_bytes(b"")
    free["value"] = 1.5 * preflight.GB
    [(_, text)] = preflight.check_space(tmp_path / "work", tmp_path / "install", 1, False)
    assert "at least 1 GB" in text  # the game's files are there already


def test_long_onedrive_and_unwritable_folders(monkeypatch, tmp_path):
    monkeypatch.delenv("OneDrive", raising=False)
    # measured from the temp folder, wherever it is (CI's is about 100 characters already)
    monkeypatch.setattr(preflight, "LONG_PATH", len(str(tmp_path)) + 10)
    assert preflight.check_folder(tmp_path / "w", "build") == []
    deep = tmp_path / ("x" * 20)
    assert "260 character" in preflight.check_folder(deep, "build")[0][1]
    monkeypatch.setenv("OneDrive", str(tmp_path))
    [(level, text)] = preflight.check_folder(tmp_path / "w", "build")
    assert level == "Warning" and "OneDrive" in text

    def refuse(self, data):
        raise PermissionError("denied")

    monkeypatch.delenv("OneDrive")
    monkeypatch.setattr(Path, "write_bytes", refuse)
    [(level, text)] = preflight.check_folder(tmp_path / "w", "build")
    assert level == "Error" and "write permissions" in text


def test_a_new_folder_is_tested_itself_not_its_parent(monkeypatch, tmp_path):
    # a drive root normal users can't write to, with a new folder in it (#40): the folder is made and tested
    real = Path.write_bytes

    def only_below(self, data):
        if self.parent == tmp_path:
            raise PermissionError("denied")
        return real(self, data)

    monkeypatch.setattr(Path, "write_bytes", only_below)
    assert "Error" not in levels(preflight.check_folder(tmp_path / "fci-build", "build"))
    assert (tmp_path / "fci-build").is_dir()


def test_a_held_checkout_is_moved_once_it_is_let_go(monkeypatch, tmp_path):
    # VS Code's Git opens the new checkout and holds it for a moment (#40)
    partial, dst = tmp_path / "dep.partial", tmp_path / "dep"
    partial.mkdir()
    real, tries = Path.rename, []

    def held(self, target):
        tries.append(1)
        if len(tries) < 3:
            raise PermissionError("Access is denied")
        return real(self, target)

    monkeypatch.setattr(Path, "rename", held)
    monkeypatch.setattr(deps.time, "sleep", lambda s: None)
    deps.move_into_place(partial, dst)
    assert dst.is_dir() and not partial.exists()

    # held for good: the error says what holds it
    def refuse(self, target):
        raise PermissionError("denied")

    partial.mkdir()
    monkeypatch.setattr(Path, "rename", refuse)
    with pytest.raises(SystemExit, match="VS Code"):
        deps.move_into_place(partial, tmp_path / "other", seconds=1)


def test_report_stops_only_on_errors(capsys):
    assert preflight.report([("Warning", "a")])
    assert not preflight.report([("Warning", "a"), ("Error", "b")])
    assert capsys.readouterr().out == "Warning: a\nWarning: a\nError: b\n"


@pytest.mark.skipif(not shutil.which("git"), reason="needs git")
def test_fetch_replaces_a_broken_copy(monkeypatch, tmp_path):
    # a local repository stands in for GitHub: two files, one with CRLF in the working tree
    src = tmp_path / "dep"
    src.mkdir()
    run = lambda *a: subprocess.run(["git", "-C", str(src), *a], check=True, capture_output=True)  # noqa: E731
    run("init", "-q")
    (src / "a.txt").write_bytes(b"one\ntwo\n")
    run("add", ".")
    run("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "x")
    commit = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], capture_output=True, text=True).stdout
    digest = deps.tree_digest(src)
    monkeypatch.setitem(deps.DEPS, "dep", {"repo": "x/dep", "commit": commit.strip(), "digest": digest})
    root = tmp_path / "deps"
    (root / "dep").mkdir(parents=True)
    (root / "dep" / "a.txt").write_bytes(b"something else")  # an interrupted or older fetch
    (root / "dep.partial").mkdir()
    assert deps.fetch("dep", root, local=tmp_path) == root / "dep"
    assert (root / "dep" / "a.txt").read_bytes() == b"one\ntwo\n"
    assert not (root / "dep.partial").exists()
    # CRLF and LF copies verify alike
    (root / "dep" / "a.txt").write_bytes(b"one\r\ntwo\r\n")
    assert deps.tree_digest(root / "dep") == digest
