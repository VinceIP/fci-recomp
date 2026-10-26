"""tools/events.py and fci-recomp.py's launcher side: JSON lines only with --events, and status as JSON"""

import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import events  # noqa: E402


def lines(capsys):
    return [json.loads(line[5:]) for line in capsys.readouterr().out.splitlines() if line.startswith("@fci ")]


def test_nothing_without_events(capsys, monkeypatch):
    monkeypatch.setattr(events, "enabled", False)
    events.emit("step", key="disc")
    events.progress("disc", 1, 2)
    assert capsys.readouterr().out == ""


def test_progress_only_when_the_percent_changes(capsys, monkeypatch):
    monkeypatch.setattr(events, "enabled", True)
    monkeypatch.setattr(events, "_shown", {})
    for done in range(0, 2001):
        events.progress("build", done, 2000, part="default")
    got = lines(capsys)
    assert [e["percent"] for e in got] == list(range(101))
    assert got[0] == {"event": "progress", "key": "build", "percent": 0, "part": "default"}


def test_status_lists_the_games_set_up(tmp_path):
    install = tmp_path / "FarCryInstincts"
    (install / "game").mkdir(parents=True)
    (install / "game" / "default.xbe").write_bytes(b"not the game")  # another file: not a game set up here
    script = str(REPO / "fci-recomp.py")
    done = subprocess.run(
        [sys.executable, "-B", script, "status", "--install", str(install), "--work", str(tmp_path)],
        capture_output=True,
        text=True,
        check=True,
    )
    status = json.loads(done.stdout)
    assert status["protocol"] == events.PROTOCOL and status["titles"] == {}
    assert status["version"]


def test_an_error_says_what_to_do():
    import importlib.util

    spec = importlib.util.spec_from_file_location("fci_recomp", REPO / "fci-recomp.py")
    script = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(script)
    held = OSError(13, "Access is denied")
    held.winerror = 5
    full = OSError(28, "No space left on device")
    assert script.what_to_do(PermissionError(13, "denied")) == script.LOCKED_HINT
    assert script.what_to_do(full) == script.FULL_HINT
    assert script.what_to_do(held) == script.LOCKED_HINT
    assert script.what_to_do(KeyError("x")) == script.REPORT_HINT
