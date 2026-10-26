"""What this PC lacks for a build, said before it starts: only what is missing or risky is printed.

  python -B tools/preflight.py [--work <folder>] [--install <folder>]

A warning names a missing tool the build works around, or a risk, and what would fix it; an error is
something the build cannot get past. Each finding is (level, text).
"""

import argparse
import ctypes
import os
from pathlib import Path
import platform
import shutil
import sys

GIT_URL = "https://git-scm.com/download/win"
VS_URL = "https://visualstudio.microsoft.com/downloads/"
GB = 2**30
# measured: the game's files 2.5 GB; per part (game, editor) about 1 GB of lift and build; a downloaded
# compiler and SDK about 2 GB while it unpacks
GAME_FILES, PER_PART, COMPILER = 2.5 * GB, 1.0 * GB, 2.0 * GB
# the deepest file a build writes is about 135 characters below the work folder's parent (the
# downloaded SDK's headers); compilers and msiexec do not all take paths past 260 characters
LONG_PATH = 100


def memory_gb():
    """installed memory in GB, 8 when it cannot be read"""

    class Status(ctypes.Structure):
        _fields_ = [
            ("length", ctypes.c_ulong),
            ("load", ctypes.c_ulong),
            ("total", ctypes.c_ulonglong),
            ("avail", ctypes.c_ulonglong),
            ("page_total", ctypes.c_ulonglong),
            ("page_avail", ctypes.c_ulonglong),
            ("virt_total", ctypes.c_ulonglong),
            ("virt_avail", ctypes.c_ulonglong),
            ("ext", ctypes.c_ulonglong),
        ]

    st = Status(ctypes.sizeof(Status))
    return st.total / GB if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)) else 8


def check_git():
    if shutil.which("git"):
        return []
    return [
        (
            "Warning",
            "git not found on this machine. The recompiler toolkit will be downloaded as source zips "
            f"from GitHub instead. Consider installing Git if errors occur: {GIT_URL}",
        )
    ]


def check_compiler(work, download=False):
    """the installed Visual Studio, or why it is not used (a finished download needs nothing said)"""
    import toolchain

    if download:
        return []
    try:
        toolchain.find_msvc()
        toolchain.find_sdk()
        return []
    except toolchain.ToolchainError as e:
        if toolchain.downloaded(Path(work) / "toolchain"):
            return []
        return [
            (
                "Warning",
                f"Visual Studio is not used: {e} Microsoft's compiler and the Windows SDK will be "
                f'downloaded instead (about 400 MB). Visual Studio with "Desktop development with C++" '
                f"avoids that: {VS_URL}",
            )
        ]


def existing(path):
    """the folder itself or its nearest parent that exists"""
    path = Path(path).resolve()
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def check_space(work, install, parts, download_compiler):
    need = {}
    if not (Path(install) / "game" / "default.xbe").exists():
        need[existing(install).anchor] = need.get(existing(install).anchor, 0) + GAME_FILES
    work_need = PER_PART * parts + (COMPILER if download_compiler else 0)
    need[existing(work).anchor] = need.get(existing(work).anchor, 0) + work_need
    found = []
    for drive, n in need.items():
        free = shutil.disk_usage(drive).free
        if free < n + GB:  # a gigabyte to spare
            found.append(("Warning", f"{drive} has {free / GB:.1f} GB free. Build requires at least {n / GB:.0f} GB."))
    return found


def check_folder(path, what):
    found = []
    path = Path(path).resolve()
    if len(str(path)) > LONG_PATH:
        found.append(
            (
                "Warning",
                f"the {what} folder's path is {len(str(path))} characters long ({path}); files within "
                "can pass Windows' 260 character limit. Use a shorter folder path if errors occur.",
            )
        )
    for var in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        root = os.environ.get(var)
        if root and path.is_relative_to(Path(root).resolve()):
            found.append(
                (
                    "Warning",
                    f"the {what} folder is in OneDrive ({path}); syncing can lock or offload files during the "
                    "build. Consider moving fci-recomp outside of OneDrive.",
                )
            )
            break
    # the folder itself, made now (the build makes it anyway): a new folder at a drive root is writable
    # where the root itself is not (#40)
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".fci-write-test"
        probe.write_bytes(b"")
        probe.unlink()
    except OSError:
        found.append(
            (
                "Error",
                f"Invalid write permissions in {path}.",
            )
        )
    return found


def check_system():
    found = []
    machine = platform.machine().upper()
    if machine not in ("AMD64", "X86_64", "ARM64"):
        found.append(("Error", f"64-bit Windows is required. (detected {machine or 'unknown'})"))
    elif machine == "ARM64":
        found.append(("Warning", "Detected ARM64. This architecture is untested and may cause issues."))
    if sys.getwindowsversion().major < 10:
        found.append(("Error", "Windows 10 or 11 is required."))
    memory = memory_gb()
    if memory < 7.5:
        found.append(("Warning", f"{memory:.0f} GB of memory: the build runs one compiler at a time and takes longer"))
    return found


def checks(work, install, parts=1, download_compiler=False):
    found = check_system() + check_git()
    compiler = check_compiler(work, download_compiler)
    found += compiler
    found += check_space(work, install, parts, bool(compiler) or download_compiler)
    found += check_folder(work, "build")
    if Path(install).resolve() != Path(work).resolve():
        found += check_folder(install, "install")
    return found


def report(found):
    """print the findings; True when the build can go on"""
    import events

    for level, text in found:
        print(f"{level}: {text}")
        events.emit(level.lower(), text=text)
    return not any(level == "Error" for level, _ in found)


def main():
    repo = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", default=str(repo / "build"))
    ap.add_argument("--install", default=str(repo / "FarCryInstincts"))
    a = ap.parse_args()
    found = checks(a.work, a.install)
    if not found:
        print("nothing missing")
    return 0 if report(found) else 1


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.exit(main())
