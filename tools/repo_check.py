"""Refuse files that must never be committed: game files and anything large.

  python -B tools/repo_check.py [file ...]     the given files, or every file git tracks

A file fails if it is over 1 MB, has a game file's extension, or has the size and SHA-256 of a file on the
disc of any game (every data/**/disc.json). The pre-commit hook and CI run it.
"""

import hashlib
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
LIMIT = 1 << 20
GAME_EXTENSIONS = {".iso", ".xiso", ".xbe", ".xex", ".dat", ".fat", ".bik", ".xwb", ".xpr", ".fcs"}


def problems(paths):
    game = {
        (f["size"], f["sha256"])
        for manifest in sorted((REPO / "data").rglob("disc.json"))
        for f in json.loads(manifest.read_text(encoding="utf-8"))["files"].values()
    }
    sizes = {size for size, _ in game}
    out = []
    for path in paths:
        p = REPO / path
        if not p.is_file():
            continue
        size = p.stat().st_size
        if p.suffix.lower() in GAME_EXTENSIONS:
            out.append(f"{path}: a game file type ({p.suffix})")
        elif size > LIMIT:
            out.append(f"{path}: {size:,} bytes, over the 1 MB limit")
        elif size in sizes and (size, hashlib.sha256(p.read_bytes()).hexdigest()) in game:
            out.append(f"{path}: identical to a file on the game disc")
    return out


def main(argv):
    paths = (
        argv
        or subprocess.run(
            ["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True, check=True
        ).stdout.splitlines()
    )
    found = problems(paths)
    for p in found:
        print(p)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
