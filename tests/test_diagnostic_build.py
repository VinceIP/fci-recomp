"""Diagnostic builds (FCI_PROBE, FCI_CALL_PROFILE) are never taken for a plain build, and a probe that names
no function is reported (tools/build.py)."""

from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build  # noqa: E402

TC = {"release": "test", "sha256": "0" * 64}


@pytest.fixture
def lift(tmp_path):
    (tmp_path / "gen").mkdir()
    (tmp_path / "lift.json").write_text("{}")
    (tmp_path / "gen" / "recomp_funcs.h").write_text("void sub_0051C5EA(void);\n")
    return tmp_path


def test_digest_follows_diagnostic_env(lift, monkeypatch):
    monkeypatch.delenv("FCI_PROBE", raising=False)
    monkeypatch.delenv("FCI_CALL_PROFILE", raising=False)
    plain = build.inputs_digest(lift, TC)
    monkeypatch.setenv("FCI_PROBE", "0051C5EA")
    probe = build.inputs_digest(lift, TC)
    monkeypatch.delenv("FCI_PROBE")
    monkeypatch.setenv("FCI_CALL_PROFILE", "1")
    assert len({plain, probe, build.inputs_digest(lift, TC)}) == 3


def test_probe_addresses_checked(lift, monkeypatch, capsys):
    monkeypatch.setenv("FCI_PROBE", "0x0051C5EA")
    with pytest.raises(SystemExit, match="8 hex digits"):
        build.check_probes(lift)
    monkeypatch.setenv("FCI_PROBE", "0051c5ea,00400000")
    build.check_probes(lift)
    out = capsys.readouterr().out
    assert "00400000" in out and "0051c5ea" not in out
