"""Fetch the two upstream projects the build uses, at the commits this project is tested with.

  python -B tools/deps.py <dir>                fetch into <dir>/<name> (keeps one already there and verified)
  python -B tools/deps.py <dir> --from <path>  clone from local checkouts in <path>/<name> instead (offline)

  xboxrecomp      sp00nz's recompiler toolkit and runtime: the lifter, the Xbox kernel, devices
  xbe-recomp      TGP482's Xbox XBE Recomp: its XDK 5849 signature list (the D3D and DirectSound
                  layer it also provides is vendored in src/xbr)

Each is checked against a digest of its files (SHA-256 over every path and file content, CRLF counted
as LF), so a zip download and a git clone are verified the same way. With git on the PATH the tool
clones; otherwise it downloads GitHub's zip of the commit. Line endings are kept as stored: the
lifter fixes and runtime patches match the files byte for byte.
"""

import argparse
import hashlib
import io
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time
import urllib.request
import zipfile

DEPS = {
    "xboxrecomp": {
        "repo": "sp00nznet/xboxrecomp",
        "commit": "193e2995ffaec871bb3be43a43bd66aa5fccfeb1",  # v0.13.1
        "digest": "77c7dcf7e05860df8b69be842f0e7fcbe20275c80b321f57539db77e725f04fb",
    },
    "xbe-recomp": {
        "repo": "TGP482/Xbox-XBERecomp",
        "commit": "65e959d4a5fd0c171dc7b17f3f9b44bab00928e6",
        "digest": "60ce07b297c7474bb8cdd9922143a4c10120546516b9dd09e4ed31312d06dd94",
    },
}


def tree_digest(root):
    """SHA-256 over the sorted relative paths and contents of every file under root, .git excluded.
    CRLF counts as LF: a repository with "text=auto" checks out with CRLF from git on Windows and LF
    from GitHub's zip, and copies fetched either way must verify."""
    h = hashlib.sha256()
    files = sorted(p for p in Path(root).rglob("*") if p.is_file() and ".git" not in p.relative_to(root).parts)
    for p in files:
        h.update(p.relative_to(root).as_posix().encode() + b"\0")
        h.update(hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).digest())
    return h.hexdigest()


def git_fetch(source, commit, dst):
    # files as stored (core.eol=lf for "text=auto"), the same bytes the zip download has
    run = lambda *a: subprocess.run(  # noqa: E731
        ["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", *a], check=True, capture_output=True
    )
    run("clone", "--quiet", "--no-checkout", source, str(dst))
    run("-C", str(dst), "checkout", "--quiet", "--detach", commit)


def zip_fetch(repo, commit, dst):
    url = f"https://codeload.github.com/{repo}/zip/{commit}"
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    unpack = dst.with_name(dst.name + ".unpack")
    remove_tree(unpack)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        top = z.namelist()[0].split("/")[0]  # <repo>-<commit>/
        z.extractall(unpack)
    shutil.move(str(unpack / top), str(dst))
    remove_tree(unpack)


def remove_tree(path):
    """a folder this tool made, .git's read-only files too"""

    def writable(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    if Path(path).exists():
        shutil.rmtree(path, **({"onexc": writable} if sys.version_info >= (3, 12) else {"onerror": writable}))


def fetch(name, root, local=None):
    dep = DEPS[name]
    dst = Path(root) / name
    if dst.exists():
        if tree_digest(dst) == dep["digest"]:
            print(f"{name}: present, verified")
            return dst
        # this folder is the tool's own: an interrupted or older fetch is replaced
        print(f"{name}: the copy in {dst} does not match {dep['commit'][:8]}; fetching it again")
        remove_tree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    # fetched beside it and moved into place once verified, so an interrupted fetch leaves nothing in the way
    partial = dst.with_name(dst.name + ".partial")
    remove_tree(partial)
    if local:
        git_fetch(str(Path(local) / name), dep["commit"], partial)
    elif shutil.which("git"):
        git_fetch(f"https://github.com/{dep['repo']}.git", dep["commit"], partial)
    else:
        print(f"{name}: git not found; downloading the source zip of {dep['repo']} from GitHub")
        zip_fetch(dep["repo"], dep["commit"], partial)
    got = tree_digest(partial)
    if got != dep["digest"]:
        remove_tree(partial)
        raise SystemExit(f"error: {name} at {dep['commit'][:8]} does not match its digest ({got}); not using it")
    move_into_place(partial, dst)
    print(f"{name}: fetched {dep['commit'][:8]}, verified")
    return dst


def move_into_place(partial, dst, seconds=10):
    """partial renamed to dst. A program that opened the new checkout (VS Code's Git, an antivirus scan) holds
    it for a moment and Windows refuses the rename meanwhile (#40), so it is tried again for a while."""
    for _ in range(seconds * 4):
        try:
            partial.rename(dst)
            return
        except PermissionError:
            time.sleep(0.25)
    try:
        partial.rename(dst)
    except PermissionError as e:
        raise SystemExit(
            f"error: {partial} can't be renamed to {dst.name} ({e}). Another program has it open: an editor such "
            "as VS Code with this folder open, or an antivirus scan. Close it and run the build again."
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir")
    ap.add_argument("--from", dest="local", help="a folder with local checkouts named like the dependencies")
    a = ap.parse_args()
    for name in DEPS:
        fetch(name, a.dir, a.local)


if __name__ == "__main__":
    sys.exit(main())
