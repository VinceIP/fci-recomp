"""Make the release zip: what a player needs to build the games, from a commit, without the developer files.

  python -B tools/package.py [ref] [--out <folder>] [--launcher <fci-launcher.exe>] [--cache <folder>]
                                                        default: HEAD into build/release

Writes fci-recomp-<VERSION>.zip (VERSION in fci-recomp.py at that commit), BuildGame.cmd and BuildEditor.cmd
at its top, and prints its SHA-256. It holds only tracked files, so no game files (tools/repo_check.py), with
the line endings .gitattributes gives them. The release attaches it beside GitHub's source archives.

Beside the commit's files it carries what a build would otherwise download, each checked against its pin,
so a build from it needs no network, git, Visual Studio or SDK:
  toolchain/   the compiler, llvm-mingw trimmed to what the build uses (tools/toolchain.py, data/llvm-mingw.json)
  deps/        the recompiler toolkit and the XDK signature list (tools/deps.py), with their licences
  python/      Python and capstone (data/python.json), which BuildGame.cmd and the launcher run
--cache keeps the downloads for the next run (default: build/release-cache).

--launcher adds the launcher built from the same commit (launcher/src-tauri: cargo build --release) at the
top as "FCI Launcher.exe"; its version must be the release's. The launcher updates itself from this zip.
"""

import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import deps  # noqa: E402
import toolchain  # noqa: E402

# for development only: tests, CI, lint settings, and the launcher's source (the zip carries it built)
EXCLUDE = (
    "tools/hostlist",
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
# parts of Python's package no build runs: headers, import libraries, pip, venv, the GUI and the docs
PYTHON_SKIP = (
    "include/",
    "libs/",
    "Lib/ensurepip/",
    "Lib/venv/",
    "Lib/idlelib/",
    "Lib/tkinter/",
    "Lib/turtledemo/",
    "Lib/test/",
    "Lib/pydoc_data/",
    "tcl/",
)


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


def download(url, check, cache):
    """url's bytes, kept in cache; check(bytes) is true for the pinned file"""
    path = Path(cache) / url.rsplit("/", 1)[1]
    if path.exists() and check(path.read_bytes()):
        return path.read_bytes()
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as r:
        data = r.read()
    if not check(data):
        raise SystemExit(f"error: {url} does not match its pin")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def python_bundle(cache):
    """Python's NuGet package (its tools/ folder) with capstone in its site-packages, checked to start"""
    pins = json.loads((REPO / "data" / "python.json").read_text(encoding="utf-8"))
    py, cs = pins["python"], pins["capstone"]
    nupkg = download(py["url"], lambda b: base64.b64encode(hashlib.sha512(b).digest()).decode() == py["sha512"], cache)
    wheel = download(cs["url"], lambda b: hashlib.sha256(b).hexdigest() == cs["sha256"], cache)
    out = Path(cache) / f"python-{py['version']}-capstone-{cs['version']}"
    if out.exists():
        shutil.rmtree(out)
    with zipfile.ZipFile(io.BytesIO(nupkg)) as z:
        for info in z.infolist():
            rel = info.filename.removeprefix("tools/")
            if info.filename.startswith("tools/") and not info.is_dir() and not rel.startswith(PYTHON_SKIP):
                (out / rel).parent.mkdir(parents=True, exist_ok=True)
                (out / rel).write_bytes(z.read(info))
    with zipfile.ZipFile(io.BytesIO(wheel)) as z:
        z.extractall(out / "Lib" / "site-packages")
    subprocess.run([str(out / "python.exe"), "-B", "-c", "import capstone, ssl, ctypes, winreg"], check=True)
    return out


def add_tree(z, folder, prefix):
    """every file under folder, as prefix/<path>; a git checkout's .git is left out"""
    for root, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d not in (".git", "__pycache__"))
        for f in sorted(files):
            path = Path(root) / f
            z.write(path, f"{prefix}/{path.relative_to(folder).as_posix()}")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ref", nargs="?", default="HEAD")
    ap.add_argument("--out", default=str(REPO / "build" / "release"))
    ap.add_argument("--launcher", help="the launcher built from ref, added as " + LAUNCHER)
    ap.add_argument("--cache", default=str(REPO / "build" / "release-cache"))
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
    cache = Path(a.cache)
    tc = toolchain.root(REPO / "toolchain") or toolchain.fetch(cache / "toolchain")
    dep_root = cache / "deps"
    for name in deps.DEPS:
        deps.fetch(name, dep_root)
    python = python_bundle(cache)
    git("archive", "--format=zip", "-o", str(zip_path), a.ref, "--", ".", *(f":!{p}" for p in EXCLUDE))
    with zipfile.ZipFile(zip_path, "a", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        add_tree(z, tc, f"toolchain/{tc.name}")
        for name in deps.DEPS:
            add_tree(z, dep_root / name, f"deps/{name}")
        add_tree(z, python, "python")
        if a.launcher:
            z.write(a.launcher, LAUNCHER)
    if not a.launcher:
        print(f"note: no --launcher; the zip has no {LAUNCHER}")
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    print(f"{zip_path} ({zip_path.stat().st_size:,} bytes)\nsha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
