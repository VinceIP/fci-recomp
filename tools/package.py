"""Make the release zip: what a player needs to build the games, from a commit, without the developer files.

  python -B tools/package.py [ref] [--out <folder>]     default: HEAD into build/release

Writes fci-recomp-<VERSION>.zip (VERSION in fci-recomp.py at that commit), BuildGame.cmd and BuildEditor.cmd
at its top, and prints its SHA-256. It holds only tracked files, so no game files (tools/repo_check.py), with
the line endings .gitattributes gives them. The release attaches it beside GitHub's source archives.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
# for development only: tests, CI, lint settings
EXCLUDE = (
    "tests",
    ".github",
    ".gitattributes",
    ".gitignore",
    ".pre-commit-config.yaml",
    ".clang-format",
    "pyproject.toml",
    "requirements-dev.txt",
)


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True).stdout


def version(ref):
    m = re.search(rb'^VERSION = "([^"]+)"', git("show", f"{ref}:fci-recomp.py"), re.M)
    if not m:
        raise SystemExit(f"error: no VERSION in fci-recomp.py at {ref}")
    return m.group(1).decode()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ref", nargs="?", default="HEAD")
    ap.add_argument("--out", default=str(REPO / "build" / "release"))
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / f"fci-recomp-{version(a.ref)}.zip"
    git("archive", "--format=zip", "-o", str(zip_path), a.ref, "--", ".", *(f":!{p}" for p in EXCLUDE))
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    print(f"{zip_path} ({zip_path.stat().st_size:,} bytes)\nsha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
