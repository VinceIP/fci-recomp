"""The C/C++ toolchain every build uses: llvm-mingw (clang, lld and the mingw-w64 headers and libraries), one
pinned release, bundled with the release zip.

  python -B tools/toolchain.py                     print the toolchain a build here would use
  python -B tools/toolchain.py fetch [<folder>]    developers: download the pinned release, keep what the
                                                   build uses (default folder: toolchain/ in the repository)
  python -B tools/toolchain.py trim <zip> <folder> the same from a zip already downloaded

data/llvm-mingw.json pins the release by its SHA-256. Only the x86_64 parts the build uses are kept: the
compiler and linker with the libraries they load, the headers, the x86_64 import and static libraries and
clang's builtins. Everything in it may be redistributed (LLVM: Apache-2.0 with LLVM exception; mingw-w64:
ZPL, public domain and permissive licences; see its LICENSE.TXT), so the release carries it and no build
needs Visual Studio, the Windows SDK or a download.
"""

import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[1]
PIN = json.loads((REPO / "data" / "llvm-mingw.json").read_text(encoding="utf-8"))
NAME = f"llvm-mingw-{PIN['release']}-ucrt-x86_64"
STAMP = "fci-toolchain.json"
# what the build runs: the driver (clang.exe is a wrapper that starts clang-<major>.exe with the target's
# .cfg), the linker, llvm-nm, and the DLLs they load
BIN = {
    "clang.exe",
    "clang++.exe",
    "clang-target-wrapper.exe",
    "x86_64-w64-mingw32-clang.exe",
    "x86_64-w64-mingw32-clang++.exe",
    "ld.lld.exe",
    "llvm-nm.exe",
    "libLLVM-{major}.dll",
    "libclang-cpp.dll",
    "libc++.dll",
    "libunwind.dll",
    "clang-{major}.exe",
    "mingw32-common.cfg",
    "x86_64-w64-windows-gnu.cfg",
    "x86_64-pc-windows-gnu.cfg",
}


class ToolchainError(Exception):
    pass


def major():
    return PIN["llvm"].split(".")[0]


def keep(path):
    """whether a file of the release (its path below the top folder) is one the build uses"""
    parts = path.split("/")
    if parts[0] == "bin":
        return len(parts) == 2 and parts[1] in {b.format(major=major()) for b in BIN}
    if parts[0] == "include":  # the headers shared by all targets
        return True
    if parts[0] == "x86_64-w64-mingw32":  # import and static libraries, the C runtime's objects
        return len(parts) > 1 and parts[1] == "lib"
    if parts[:4] == ["lib", "clang", major(), "include"]:
        return True
    if parts[:5] == ["lib", "clang", major(), "lib", "windows"]:
        return parts[-1] == "libclang_rt.builtins-x86_64.a"
    return path == "LICENSE.TXT"


def trim(zip_bytes, folder):
    """the kept files of the release zip in folder/<NAME>, with a stamp that names the release"""
    if hashlib.sha256(zip_bytes).hexdigest() != PIN["sha256"]:
        raise ToolchainError(f"the download is not llvm-mingw {PIN['release']} (its SHA-256 differs); not using it")
    dst = Path(folder) / NAME
    partial = dst.with_name(dst.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    kept = 0
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        for info in z.infolist():
            path = info.filename.split("/", 1)[1] if "/" in info.filename else ""
            if info.is_dir() or not path or not keep(path):
                continue
            target = partial / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(info))
            kept += 1
    (partial / STAMP).write_text(json.dumps({"release": PIN["release"], "sha256": PIN["sha256"], "files": kept}))
    if dst.exists():
        shutil.rmtree(dst)
    partial.rename(dst)
    return dst


def fetch(folder=REPO / "toolchain"):
    """developers: the pinned release, downloaded and trimmed into folder (kept when already there)"""
    found = root(folder)
    if found:
        return found
    print(f"Downloading llvm-mingw {PIN['release']} ({PIN['size'] // 1_000_000} MB)")
    with urllib.request.urlopen(PIN["url"], timeout=300) as r:
        data = r.read()
    return trim(data, folder)


def root(folder):
    """the toolchain in folder, or None when it is missing, incomplete or another release"""
    dst = Path(folder) / NAME
    try:
        stamp = json.loads((dst / STAMP).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    needed = [dst / "bin" / b.format(major=major()) for b in BIN if b.endswith((".exe", ".dll"))]
    if stamp.get("sha256") != PIN["sha256"] or not all(p.exists() for p in needed):
        return None  # an antivirus can remove a file after it was unpacked
    return dst


def discover(*folders):
    """the toolchain a build uses: the release's bundled copy (toolchain/ in the repository) or one in folders"""
    for folder in (REPO / "toolchain", *folders):
        found = root(folder)
        if found:
            return describe(found)
    raise ToolchainError(
        f"the bundled compiler (toolchain/{NAME}) is missing or incomplete. Extract the whole release zip "
        "again; an antivirus may have removed some of its files."
    )


def describe(found):
    return {
        "release": f"llvm-mingw {PIN['release']} (LLVM {PIN['llvm']})",
        "sha256": PIN["sha256"],
        "root": str(found),
        "bin": str(found / "bin"),
    }


def main(argv):
    try:
        if argv[:1] == ["fetch"]:
            print(fetch(Path(argv[1]) if len(argv) > 1 else REPO / "toolchain"))
        elif argv[:1] == ["trim"]:
            print(trim(Path(argv[1]).read_bytes(), Path(argv[2])))
        else:
            print(json.dumps(discover(), indent=2))
    except ToolchainError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
