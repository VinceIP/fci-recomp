"""Import save games and user maps copied from an Xbox: Action Replay zips or a copied UDATA folder.

  python -B tools/xbox_import.py <zip | folder>... [--title instincts|evolution] [--save-dir <dir>] [--replace]
                                 [--dry-run]

Each save folder under UDATA/<title> (Far Cry Instincts 55530008, Evolution 55530060; tools/titles.py) is
copied into the folder that game keeps saves in (by default %APPDATA%\\fci-recomp\\profile for Instincts,
%APPDATA%\\fci-recomp\\evolution\\profile for Evolution, `save-dir` in its fci.ini), with its files signed
the way this runtime signs saves, so the game accepts them. A source with both games' saves needs --title.
A save folder already there is left alone unless --replace, which first moves the old one to
<save dir>/import-backup/<time>. Published user maps are save folders too:
the game lists them in Splitscreen and System Link.
"""

import argparse
import hashlib
import hmac
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import titles  # noqa: E402

TITLE = "55530008"  # Far Cry Instincts, the default
RUNTIME_KEY = bytes(16)  # the runtime's save signatures are HMAC-SHA1 under this key
SIGNED = {".sav", ".fcs", ".mt", ".ren"}  # signature in the last 20 bytes
NOT_COPIED = {"datelinfo.xbx", "gameid.ini"}  # the Action Replay's own files


class SaveImportError(Exception):
    pass


def mac(data):
    return hmac.new(RUNTIME_KEY, data, hashlib.sha1).digest()


def under_title(parts, title=TITLE):
    """the path below UDATA/<title>, or None for anything else"""
    low = [p.lower() for p in parts]
    for i in range(len(low) - 1):
        if low[i] == "udata" and low[i + 1] == title.lower():
            rest = parts[i + 2 :]
            # zip names split on "/" only: a "\" or drive in a name would act as a path on Windows
            if any(p in ("", ".", "..") or set(p) & set("\\:/") for p in rest):
                raise SaveImportError(f"unsafe path {'/'.join(parts)}")
            return tuple(rest)
    return None


def names_in(path):
    """every file's path parts in a zip or under a folder"""
    path = Path(path)
    if path.is_file():
        with zipfile.ZipFile(path) as zf:
            return [PurePosixPath(i.filename).parts for i in zf.infolist() if not i.is_dir()]
    if path.is_dir():
        return [p.relative_to(path.parent).parts for p in path.rglob("*") if p.is_file()]
    raise SaveImportError(f"{path} not found")


def titles_in(path):
    """the known games with saves in a zip or folder"""
    ids = {t.title_id: t for t in titles.TITLES.values()}
    found = set()
    for parts in names_in(path):
        low = [p.lower() for p in parts]
        found |= {low[i + 1] for i in range(len(low) - 1) if low[i] == "udata"}
    return [ids[i] for i in sorted(ids) if i.lower() in found]


def read_source(path, title=TITLE):
    """{(folder, ..., name): bytes} for every file under UDATA/<title> in a zip or a folder"""
    path = Path(path)
    files = {}
    if path.is_file():
        with zipfile.ZipFile(path) as zf:
            for info in zf.infolist():
                rest = under_title(PurePosixPath(info.filename).parts, title)
                if rest and not info.is_dir():
                    files[rest] = zf.read(info)
    elif path.is_dir():
        for p in path.rglob("*"):
            rest = under_title(p.relative_to(path.parent).parts, title)
            if rest and p.is_file():
                files[rest] = p.read_bytes()
    else:
        raise SaveImportError(f"{path} not found")
    if not files:
        raise SaveImportError(f"{path}: no UDATA/{title} folder in it")
    return {k: v for k, v in files.items() if k[-1].lower() not in NOT_COPIED}


def resign(files):
    """the same files signed for this runtime; files the game does not sign are kept as they are"""
    out = dict(files)
    for key, data in files.items():
        name = key[-1].lower()
        if name.endswith(".sig"):  # a map's .dat or .fat: [signature of the rest][signature of the file]...
            body = files.get(key[:-1] + (key[-1][:-4],))
            if body is None or len(data) < 40:
                raise SaveImportError(f"{'/'.join(key)}: not a signature this game writes")
            rest = mac(body) + data[40:]
            out[key] = mac(rest) + rest
        elif Path(name).suffix in SIGNED:
            if len(data) <= 20:
                raise SaveImportError(f"{'/'.join(key)}: too short for a signed file")
            out[key] = data[:-20] + mac(data[:-20])
    return out


def save_name(files, folder):
    meta = files.get((folder, "SaveMeta.xbx"))
    if not meta:
        return "?"
    text = meta.decode("utf-16", errors="replace")
    for line in text.splitlines():
        if line.startswith("Name="):
            return line[5:45].strip()  # user maps keep their id after the first 40 characters
    return "?"


def install(files, save_dir, replace=False, dry_run=False, title=TITLE):
    """copy the save folders in; returns [(folder, name, what happened)]"""
    root = Path(save_dir) / "UDATA" / title
    for key in files:
        if not root.joinpath(*key).resolve().is_relative_to(root.resolve()):
            raise SaveImportError(f"unsafe path {'/'.join(key)}")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    report = []
    for folder in sorted({k[0] for k in files if len(k) > 1}):
        target = root / folder
        name = save_name(files, folder)
        if target.exists() and not replace:
            report.append((folder, name, "already there, skipped (--replace to overwrite)"))
            continue
        what = "replaced" if target.exists() else "imported"
        if not dry_run:
            if target.exists():
                backup = Path(save_dir) / "import-backup" / stamp
                backup.mkdir(parents=True, exist_ok=True)
                shutil.move(str(target), str(backup / folder))
            for key, data in files.items():
                if key[0] == folder and len(key) > 1:
                    dest = root.joinpath(*key)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(data)
        report.append((folder, name, what))
    # the title's own files (TitleMeta.xbx, its images) only when the game has not written them yet
    for key, data in files.items():
        if len(key) == 1 and not (root / key[0]).exists() and not dry_run:
            root.mkdir(parents=True, exist_ok=True)
            (root / key[0]).write_bytes(data)
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sources", nargs="+", help="Action Replay zips or folders with UDATA/<title> in them")
    ap.add_argument("--title", choices=sorted(titles.TITLES), help="the game, when a source has both games' saves")
    ap.add_argument("--save-dir", help="default: the game's own save folder under %%APPDATA%%")
    ap.add_argument("--replace", action="store_true", help="overwrite save folders already there")
    ap.add_argument("--dry-run", action="store_true", help="only say what would be imported")
    a = ap.parse_args(argv)
    try:
        for src in a.sources:
            found = [titles.TITLES[a.title]] if a.title else titles_in(src)
            if len(found) != 1:
                raise SaveImportError(
                    f"{src}: saves of both games; choose one with --title"
                    if found
                    else f"{src}: no saves of Far Cry Instincts (UDATA/55530008) or Evolution (UDATA/55530060)"
                )
            game = found[0]
            save_dir = a.save_dir or os.path.expandvars(f"%APPDATA%\\{game.appdata}\\profile")
            files = resign(read_source(src, game.title_id))
            for folder, name, what in install(files, save_dir, a.replace, a.dry_run, game.title_id):
                print(f"{Path(src).name}: {game.name} {folder} {name!r}: {what}")
    except (SaveImportError, zipfile.BadZipFile) as e:
        raise SystemExit(f"error: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
