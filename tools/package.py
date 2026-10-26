"""Make the release zip: what a player needs to build the games, from a commit, without the developer files.

  python -B tools/package.py [ref] [--out <folder>] [--launcher <fci-launcher.exe>]
                                                        default: HEAD into build/release

Writes fci-recomp-<VERSION>.zip (VERSION in fci-recomp.py at that commit), BuildGame.cmd and BuildEditor.cmd
at its top, and prints its SHA-256. It holds only tracked files, so no game files (tools/repo_check.py), with
the line endings .gitattributes gives them. The release attaches it beside GitHub's source archives.

--launcher adds the launcher built from the same commit (launcher/src-tauri: cargo build --release) at the
top as "FCI Launcher.exe"; its version must be the release's. The launcher updates itself from this zip.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import zipfile

REPO = Path(__file__).resolve().parents[1]
# for development only: tests, CI, lint settings, and the launcher's source (the zip carries it built)
EXCLUDE = (
    "launcher",
    "tests",
    ".github",
    ".gitattributes",
    ".gitignore",
    ".pre-commit-config.yaml",
    ".clang-format",
    "pyproject.toml",
    "requirements-dev.txt",
)
LAUNCHER = "FCI Launcher.exe"


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True).stdout


def version(ref):
    m = re.search(rb'^VERSION = "([^"]+)"', git("show", f"{ref}:fci-recomp.py"), re.M)
    if not m:
        raise SystemExit(f"error: no VERSION in fci-recomp.py at {ref}")
    return m.group(1).decode()


def launcher_version(ref):
    m = re.search(rb'^version = "([^"]+)"', git("show", f"{ref}:launcher/src-tauri/Cargo.toml"), re.M)
    return m.group(1).decode() if m else None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ref", nargs="?", default="HEAD")
    ap.add_argument("--out", default=str(REPO / "build" / "release"))
    ap.add_argument("--launcher", help="the launcher built from ref, added as " + LAUNCHER)
    a = ap.parse_args(argv)
    release = version(a.ref)
    if a.launcher:
        exe = Path(a.launcher)
        if not exe.is_file() or exe.read_bytes()[:2] != b"MZ":
            raise SystemExit(f"error: {exe} is not a Windows executable")
        if launcher_version(a.ref) != release:
            raise SystemExit(f"error: the launcher at {a.ref} is version {launcher_version(a.ref)}, not {release}")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / f"fci-recomp-{release}.zip"
    git("archive", "--format=zip", "-o", str(zip_path), a.ref, "--", ".", *(f":!{p}" for p in EXCLUDE))
    if a.launcher:
        with zipfile.ZipFile(zip_path, "a", zipfile.ZIP_DEFLATED) as z:
            z.write(a.launcher, LAUNCHER)
    else:
        print(f"note: no --launcher; the zip has no {LAUNCHER}")
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    print(f"{zip_path} ({zip_path.stat().st_size:,} bytes)\nsha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
