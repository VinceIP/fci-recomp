"""Build Far Cry Instincts for Windows from your own copy of the original Xbox disc.

  python fci-recomp.py setup <disc> [--install <folder>] [--editor] [--work <folder>] [--jobs N]
  python fci-recomp.py editor [--install <folder>] [--work <folder>] [--jobs N]

  setup        check the disc, copy its files, recompile and build the game, install it
  editor       build the map editor for a game set up earlier (the game's Map Editor menu starts it)

  <disc>       an Xbox disc image (XISO or full dump) or a folder with the disc's files
  --install    where the game goes: FarCryInstincts.exe, fci.ini and the game files (default:
               "Far Cry Instincts" in this folder)
  --editor     build the map editor too
  --work       where the toolkit, the compiler, the generated C and the build go (default: build/ in
               this folder); about 10 GB, and it can be deleted afterwards
  --jobs       compilers at once (default: by the processor's cores and the memory, about 2 GB each)
  --accept-msvc-license   accept Microsoft's Build Tools license terms without asking (only used when
               Visual Studio is not installed and its compiler is downloaded)
  --download-compiler     download Microsoft's compiler even when Visual Studio is installed (the
               pinned version this project is checked with)

Needs Windows 10/11 x64 and Python 3.11+ with the packages in requirements.txt (BuildGame.cmd provides
both). Visual Studio is used when it is installed with "Desktop development with C++"; otherwise
Microsoft's compiler and the Windows SDK are downloaded into the work folder (tools/msvc.py). Run it
again to resume after an interruption: finished steps are kept.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

VERSION = "0.1.2"
REPO = Path(__file__).resolve().parent
sys.path.insert(0, str(REPO / "tools"))
IMAGES = {"game": "default.xbe", "editor": "ige.xbe"}
EXES = {"game": "FarCryInstincts.exe", "editor": "FarCryInstinctsEditor.exe"}


def check_python():
    if sys.version_info < (3, 11):
        raise SystemExit("error: Python 3.11 or newer is needed")
    try:
        import capstone  # noqa: F401
    except ImportError:
        raise SystemExit("error: the capstone package is missing. Run: python -m pip install -r requirements.txt")


NETWORK_HINT = (
    "Check the internet connection; a proxy, firewall or antivirus can also block the download. Run it again to resume."
)


def preflight(work, install, parts, download_compiler):
    """what this PC lacks, said before anything starts; stops only on what the build cannot get past"""
    import preflight as checks

    if not checks.report(checks.checks(work, install, parts, download_compiler)):
        raise SystemExit(1)


def find_toolchain(work, accept_license, download=False):
    """the installed Visual Studio, or Microsoft's compiler downloaded into <work>/toolchain"""
    import msvc
    import toolchain

    folder = work / "toolchain"
    try:
        tc = toolchain.downloaded(folder) if download else toolchain.discover(folder)
        if not tc:
            raise toolchain.ToolchainError("not downloaded yet")
    except toolchain.ToolchainError:
        print("Downloading Microsoft's compiler and the Windows SDK")  # why: the check at the start said
        try:
            msvc.fetch(folder, accept_license)
        except msvc.DownloadError as e:
            raise SystemExit(f"error: {e}")
        except OSError as e:  # urllib's errors too
            raise SystemExit(f"error: downloading the compiler failed ({e}). {NETWORK_HINT}")
        tc = toolchain.downloaded(folder)
    print(f"Compiler: MSVC {tc['msvc']}, Windows SDK {tc['windows_sdk']}")
    return tc


def repository_commit():
    try:
        return subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def copy_game(disc_path, game):
    import disc

    if (game / "default.xbe").exists():
        print("  the game files are in place already")
        return
    try:
        source = disc.open_disc(disc_path)
    except disc.DiscError as e:
        raise SystemExit(f"error: {e}")
    problems = disc.verify(source)
    if problems:
        print("This is not the supported Far Cry Instincts release (title 55530008, version 1):")
        for p in problems[:20]:
            print("  " + p)
        raise SystemExit(1)
    partial = game.with_name(game.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    disc.extract(source, partial)
    partial.rename(game)


def translate(parts, install, work):
    """lift the game and/or the editor to C; two lifts run side by side (each is one process). A lift
    made from the same inputs earlier is kept; one from older inputs (an updated repository) is redone."""
    from concurrent.futures import ThreadPoolExecutor

    import lift

    todo = []
    for part in parts:
        image = IMAGES[part]
        out = work / "lift" / Path(image).stem
        if lift.current(image, out):
            print(f"  {image}: recompiled to C earlier")
            continue
        if out.exists():
            shutil.rmtree(out)
        todo.append((image, out))
    tag = len(todo) > 1
    with ThreadPoolExecutor(max(1, len(todo))) as pool:
        for f in [
            pool.submit(
                lift.lift, image, install / "game" / image, work / "deps" / "xboxrecomp", out, image if tag else None
            )
            for image, out in todo
        ]:
            f.result()


def compile_part(part, install, work, jobs, tc):
    """build the game or the editor from its lift, then install its executable"""
    import build

    image = IMAGES[part]
    stem = Path(image).stem
    lifted, built = work / "lift" / stem, work / "build" / stem
    if build.current(built, lifted, tc):
        print(f"  {image}: built earlier")
    else:
        if built.exists():
            shutil.rmtree(built)
        build.build(image, install / "game" / image, lifted, work / "deps", built, jobs, tc)
    shutil.copy2(built / "bin" / "fci-whole.exe", install / EXES[part])
    if part == "game":
        shutil.copy2(built / "bin" / "fci-kinit.dll", install / "fci-kinit.dll")
    record = install / "build.json"
    info = json.loads(record.read_text(encoding="utf-8")) if record.exists() else {}
    info.update(
        {"version": VERSION, "repository_commit": repository_commit(), "built": time.strftime("%Y-%m-%d %H:%M")}
    )
    info.setdefault("lift", {})[stem] = json.loads((lifted / "lift.json").read_text())["xbe_sha256"]
    record.write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")


def fetch_deps(work):
    import deps

    for name in deps.DEPS:
        try:
            deps.fetch(name, work / "deps")
        except (OSError, subprocess.CalledProcessError) as e:  # network, git and unpacking errors
            raise SystemExit(f"error: fetching {name} failed ({e}). {NETWORK_HINT}")


def setup(a):
    started = time.time()
    check_python()
    install, work = Path(a.install).resolve(), Path(a.work).resolve()
    preflight(work, install, 2 if a.editor else 1, a.download_compiler)
    install.mkdir(parents=True, exist_ok=True)
    print("1/5 Checking and copying the disc's files")
    copy_game(a.disc, install / "game")
    print("2/5 Finding the compiler")
    tc = find_toolchain(work, a.accept_msvc_license, a.download_compiler)
    print("3/5 Fetching the recompiler toolkit")
    fetch_deps(work)
    parts = ["game", "editor"] if a.editor else ["game"]
    print("4/5 Recompiling the game's code to C" + (" (the game and the map editor side by side)" if a.editor else ""))
    translate(parts, install, work)
    print("5/5 Building")
    for part in parts:
        compile_part(part, install, work, a.jobs, tc)
    if not (install / "fci.ini").exists():
        shutil.copy2(REPO / "src" / "fci.ini", install / "fci.ini")
    print(
        f"Done in {(time.time() - started) / 60:.0f} minutes. Start {install / EXES['game']}; "
        f"settings are in {install / 'fci.ini'}."
    )
    if not a.editor:
        print("The map editor is not built; run BuildEditor.cmd (or: fci-recomp.py editor) to add it.")


def editor(a):
    started = time.time()
    check_python()
    install, work = Path(a.install).resolve(), Path(a.work).resolve()
    if not (install / EXES["game"]).exists() or not (install / "game" / "ige.xbe").exists():
        raise SystemExit(f"error: set up the game in {install} first (BuildGame.cmd or: fci-recomp.py setup)")
    preflight(work, install, 1, a.download_compiler)
    tc = find_toolchain(work, a.accept_msvc_license, a.download_compiler)
    fetch_deps(work)
    translate(["editor"], install, work)
    compile_part("editor", install, work, a.jobs, tc)
    print(
        f"Done in {(time.time() - started) / 60:.0f} minutes. The game's Map Editor menu starts "
        f"{install / EXES['editor']}."
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="action", required=True)
    s = sub.add_parser("setup", help="check the disc, recompile and build the game, install it")
    s.add_argument("disc")
    s.add_argument("--editor", action="store_true")
    e = sub.add_parser("editor", help="build the map editor for a game set up earlier")
    for p in (s, e):
        p.add_argument("--install", default=str(REPO / "Far Cry Instincts"))
        p.add_argument("--work", default=str(REPO / "build"))
        p.add_argument("--jobs", type=int)
        p.add_argument("--accept-msvc-license", action="store_true")
        p.add_argument("--download-compiler", action="store_true")
    a = ap.parse_args()
    (setup if a.action == "setup" else editor)(a)


if __name__ == "__main__":
    main()
