"""tools/xbox_import.py on synthetic saves (no game data): reading zips and folders, signing, installing"""

import hashlib
import hmac
from pathlib import Path
import sys
import zipfile

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import xbox_import  # noqa: E402

OTHER_KEY = bytes(range(16))  # stands in for whatever signed the files on the console
MAP = "715769829f4c5374151005165025996"


def mac(key, data):
    return hmac.new(key, data, hashlib.sha1).digest()


def signed(body, key=OTHER_KEY):
    return body + mac(key, body)


def sig(body, tail=b"", key=OTHER_KEY):
    rest = mac(key, body) + tail
    return mac(key, rest) + rest


def meta(name):
    return ("﻿Name=" + name + "\r\n").encode("utf-16-le")


def exported():
    """an Action Replay style export: a profile with a checkpoint, a published map, the device's own files"""
    dat, fat = b"CRY\x01" + bytes(60), b"DAT2" + bytes(28)
    u = "UDATA/55530008/"
    return {
        u + "TitleMeta.xbx": meta("Far Cry Instincts"),
        u + "datelinfo.xbx": b"Savename=JACK",
        u + "9FBDF33E7A0C/SaveMeta.xbx": meta("JACK CARVER"),
        u + "9FBDF33E7A0C/profile.sav": signed(b"profile" * 10),
        u + "9FBDF33E7A0C/Checkpoint_Bossfight.fcs": signed(b"checkpoint" * 10),
        u + "06FDB545E74F/SaveMeta.xbx": meta("SECLUSION".ljust(40) + "1000" + MAP),
        u + f"06FDB545E74F/{MAP}/{MAP}.dat": dat,
        u + f"06FDB545E74F/{MAP}/{MAP}.dat.sig": sig(dat),
        u + f"06FDB545E74F/{MAP}/{MAP}.fat": fat,
        u + f"06FDB545E74F/{MAP}/{MAP}.fat.sig": sig(fat, tail=b"\x01\x00\x01\x00" + bytes(96)),
    }


def write_zip(path, files):
    with zipfile.ZipFile(path, "w") as zf:
        for name, data in files.items():
            zf.writestr(name, data)
    return path


def test_zip_import_resigns_for_the_runtime(tmp_path):
    src = write_zip(tmp_path / "export.zip", exported())
    save = tmp_path / "save"
    assert xbox_import.main([str(src), "--save-dir", str(save)]) == 0
    root = save / "UDATA" / "55530008"
    key = xbox_import.RUNTIME_KEY
    for name in ("profile.sav", "Checkpoint_Bossfight.fcs"):
        data = (root / "9FBDF33E7A0C" / name).read_bytes()
        assert data[-20:] == mac(key, data[:-20])
    for ext in ("dat", "fat"):
        body = (root / "06FDB545E74F" / MAP / f"{MAP}.{ext}").read_bytes()
        s = (root / "06FDB545E74F" / MAP / f"{MAP}.{ext}.sig").read_bytes()
        assert s[20:40] == mac(key, body) and s[:20] == mac(key, s[20:])
    tail = (root / "06FDB545E74F" / MAP / f"{MAP}.fat.sig").read_bytes()[40:]
    assert tail == b"\x01\x00\x01\x00" + bytes(96)  # kept as it came
    assert (root / "06FDB545E74F" / "SaveMeta.xbx").read_bytes() == exported()[
        "UDATA/55530008/06FDB545E74F/SaveMeta.xbx"
    ]
    assert (root / "TitleMeta.xbx").exists() and not (root / "datelinfo.xbx").exists()


def test_folder_import_and_names(tmp_path):
    src = tmp_path / "copied"
    for name, data in exported().items():
        (src / name).parent.mkdir(parents=True, exist_ok=True)
        (src / name).write_bytes(data)
    files = xbox_import.resign(xbox_import.read_source(src / "UDATA"))
    report = xbox_import.install(files, tmp_path / "save")
    assert sorted((f, n) for f, n, _ in report) == [("06FDB545E74F", "SECLUSION"), ("9FBDF33E7A0C", "JACK CARVER")]


def test_existing_save_is_kept_unless_replaced(tmp_path):
    src = write_zip(tmp_path / "export.zip", exported())
    save = tmp_path / "save"
    mine = save / "UDATA" / "55530008" / "9FBDF33E7A0C"
    mine.mkdir(parents=True)
    (mine / "profile.sav").write_bytes(b"my own profile")
    files = xbox_import.resign(xbox_import.read_source(src))
    report = dict((f, w) for f, _, w in xbox_import.install(files, save))
    assert report["9FBDF33E7A0C"].startswith("already there")
    assert (mine / "profile.sav").read_bytes() == b"my own profile"
    report = dict((f, w) for f, _, w in xbox_import.install(files, save, replace=True))
    assert report["9FBDF33E7A0C"] == "replaced"
    backups = list((save / "import-backup").glob("*/9FBDF33E7A0C/profile.sav"))
    assert [b.read_bytes() for b in backups] == [b"my own profile"]


def test_dry_run_writes_nothing(tmp_path):
    src = write_zip(tmp_path / "export.zip", exported())
    files = xbox_import.resign(xbox_import.read_source(src))
    assert len(xbox_import.install(files, tmp_path / "save", dry_run=True)) == 2
    assert not (tmp_path / "save").exists()


def test_rejects_unsafe_paths_and_other_titles(tmp_path):
    with pytest.raises(xbox_import.SaveImportError):
        xbox_import.read_source(write_zip(tmp_path / "bad.zip", {"UDATA/55530008/../../evil.sav": b"x" * 40}))
    for name in ("SAVE/C:\\Users\\x\\pwn.txt", "SAVE/..\\..\\pwn.txt", "SAVE/D:pwn.txt"):
        with pytest.raises(xbox_import.SaveImportError):
            xbox_import.read_source(write_zip(tmp_path / "win.zip", {"UDATA/55530008/" + name: b"x"}))
    with pytest.raises(xbox_import.SaveImportError):
        xbox_import.read_source(write_zip(tmp_path / "other.zip", {"UDATA/4D530004/ABC/x.sav": b"x" * 40}))
    # install checks every destination too, whatever built the file list
    with pytest.raises(xbox_import.SaveImportError):
        xbox_import.install({("SAVE", "..", "..", "pwn.txt"): b"x"}, tmp_path / "save")
    assert not (tmp_path / "pwn.txt").exists()
