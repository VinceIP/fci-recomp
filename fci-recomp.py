"""Build Far Cry Instincts or Evolution for Windows from your own copy of the original Xbox disc.

  python fci-recomp.py setup <disc> [--install <folder>] [--editor] [--work <folder>] [--jobs N]
  python fci-recomp.py editor [<disc>] [--title instincts|evolution] [--install <folder>] [--work <folder>]
                              [--jobs N]
  python fci-recomp.py update [--title instincts|evolution] [--install <folder>] [--work <folder>]
  python fci-recomp.py status [--install <folder>]... [--work <folder>]

  setup        check the disc, copy its files, recompile and build the game, install it
  editor       build the map editor for a game set up earlier (the game's Map Editor menu starts it); with
               the disc, it is that disc's game
  update       rebuild a game set up earlier (and its map editor, if built) with this version, from its
               installed files: no disc needed. Only what this version changed is redone.
  status       as JSON: each game set up here, the version it was built with, and what an update redoes

  <disc>       an Xbox disc image (XISO or full dump) or a folder with the disc's files; which game it is
               decides the rest (tools/titles.py)
  --install    where the game goes: its executable, fci.ini and the game files (default:
               FarCryInstincts or FarCryEvolution, in this folder)
  --title      for editor without a disc: which game, when both are set up here
  --editor     build the map editor too
  --work       where the toolkit, the compiler, the generated C and the build go (default: build/ in
               this folder); about 10 GB per game, and it can be deleted afterwards. Both games share the
               compiler and the toolkit.
  --jobs       compilers at once (default: by the processor's cores and the memory, about 2 GB each)
  --accept-msvc-license   accept Microsoft's Build Tools license terms without asking (only used when
               Visual Studio is not installed and its compiler is downloaded)
  --download-compiler     download Microsoft's compiler even when Visual Studio is installed (the
               pinned version this project is checked with)
  --events     also print progress as JSON lines for the launcher (tools/events.py)

Needs Windows 10/11 x64 and Python 3.11+ with the packages in requirements.txt (BuildGame.cmd provides
both). Visual Studio is used when it is installed with "Desktop development with C++"; otherwise
Microsoft's compiler and the Windows SDK are downloaded into the work folder (tools/msvc.py). Run it
again to resume after an interruption: finished steps are kept.
"""

import argparse
import errno
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback

VERSION = "0.2.2"
REPO = Path(__file__).resolve().parent
sys.path.insert(0, str(REPO / "tools"))
import events  # noqa: E402
import titles  # noqa: E402

IMAGES = {"game": "default.xbe", "editor": "ige.xbe"}
# the steps of a build, and roughly how much of its time each takes (for the launcher's progress bar)
STEPS = {
    "disc": ("Checking and copying the disc's files", 8),
    "compiler": ("Finding the compiler", 2),
    "toolkit": ("Fetching the recompiler toolkit", 2),
    "lift": ("Recompiling the game's code to C", 45),
    "build": ("Building", 43),
}


def plan(*keys):
    events.emit("plan", steps=[{"key": k, "text": STEPS[k][0], "weight": STEPS[k][1]} for k in keys])
    return list(keys)


def step(keys, key, suffix=""):
    """print and announce one step of the plan"""
    text = STEPS[key][0] + suffix
    print(f"{keys.index(key) + 1}/{len(keys)} {text}")
    events.emit("step", key=key, text=text)


def parts_dir(work, title, kind, image):
    """work/<kind>/<image stem>; a game other than Instincts under work/<kind>/<title>/ (Instincts keeps the
    folders of earlier versions, so its builds resume)"""
    base = work / kind if title.key == "instincts" else work / kind / title.key
    return base / Path(image).stem


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
COMPILER_HINT = (
    'Installing Visual Studio Build Tools with "Desktop development with C++" also works '
    "(https://visualstudio.microsoft.com/downloads/); the build then uses it instead of the download."
)
LOCKED_HINT = (
    "Another program had the file open: OneDrive, an antivirus or an open editor. Close it, or move fci-recomp "
    "outside OneDrive to a folder such as C:\\Games\\fci-recomp, then run the build again."
)
FULL_HINT = "The drive is full. Free some space and run the build again; it resumes where it stopped."
REPORT_HINT = "Run the build again; if it stops the same way, report it on Discord or GitHub with the build log."


def what_to_do(e):
    """the next step for an error nothing caught: one the player can fix, else a report"""
    if isinstance(e, OSError):
        if e.errno == errno.ENOSPC or getattr(e, "winerror", None) in (39, 112):
            return FULL_HINT
        if isinstance(e, PermissionError) or getattr(e, "winerror", None) in (5, 32, 33):
            return LOCKED_HINT
    return REPORT_HINT


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

    def ask(license_url):
        """with the launcher, it asks: say so and stop; it starts the build again with --accept-msvc-license"""
        if not events.enabled:
            return msvc.ask_in_console(license_url)
        events.emit("license", url=license_url)
        raise SystemExit(3)

    try:
        tc = toolchain.downloaded(folder) if download else toolchain.discover(folder)
        if not tc:
            raise toolchain.ToolchainError("not downloaded yet")
    except toolchain.ToolchainError:
        print("Downloading Microsoft's compiler and the Windows SDK")  # why: the check at the start said
        try:
            msvc.fetch(
                folder,
                accept_license,
                ask=ask,
                progress=lambda done, total: events.progress("compiler", done, total, "Downloading the compiler"),
            )
        except msvc.DownloadError as e:
            raise SystemExit(f"error: {e}\n{COMPILER_HINT}")
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


def open_disc(disc_path):
    """the disc and the game on it"""
    import disc

    try:
        source = disc.open_disc(disc_path)
    except disc.DiscError as e:
        raise SystemExit(f"error: {e}")
    title = disc.identify(source)
    if title is None:
        raise SystemExit(f"error: this disc is not a supported release: {disc.why_unsupported(source)}")
    return source, title


def copy_game(source, title, game):
    import disc

    if (game / "default.xbe").exists():
        if titles.of_image("default.xbe", game / "default.xbe") != title:
            raise SystemExit(f"error: {game} holds another game's files; choose another --install folder")
        print("  the game files are in place already")
        return
    problems = disc.verify(source, title=title)
    if problems:
        print(f"This is not the supported {title.name} release (title {title.title_id}):")
        for p in problems[:20]:
            print("  " + p)
        raise SystemExit(1)
    partial = game.with_name(game.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    disc.extract(source, partial, title)
    partial.rename(game)


def translate(parts, title, install, work):
    """lift the game and/or the editor to C; two lifts run side by side (each is one process). A lift
    made from the same inputs earlier is kept; one from older inputs (an updated repository) is redone."""
    from concurrent.futures import ThreadPoolExecutor

    import lift

    todo = []
    for part in parts:
        image = IMAGES[part]
        out = parts_dir(work, title, "lift", image)
        if lift.current(image, title, out):
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


def compile_part(part, title, install, work, jobs, tc):
    """build the game or the editor from its lift, then install its executable"""
    import build

    image = IMAGES[part]
    stem = Path(image).stem
    lifted, built = parts_dir(work, title, "lift", image), parts_dir(work, title, "build", image)
    if build.current(built, lifted, tc):
        print(f"  {image}: built earlier")
    else:
        if built.exists():
            shutil.rmtree(built)
        # objects of the generated files an earlier build compiled the same way are reused (an update that
        # changes only the host code then recompiles only that); the cache keeps what this build used
        cache = parts_dir(work, title, "objects", image)
        started = time.time() - 1
        build.build(image, install / "game" / image, lifted, work / "deps", built, jobs, tc, obj_cache=cache)
        for obj in cache.glob("*.obj"):
            if obj.stat().st_mtime < started:
                obj.unlink()
    shutil.copy2(built / "bin" / "fci-whole.exe", install / title.exe(image))
    if part == "game":
        shutil.copy2(built / "bin" / "fci-kinit.dll", install / "fci-kinit.dll")
    record = install / "build.json"
    info = json.loads(record.read_text(encoding="utf-8")) if record.exists() else {}
    info.update(
        {
            "version": VERSION,
            "title": title.key,
            "repository_commit": repository_commit(),
            "built": time.strftime("%Y-%m-%d %H:%M"),
        }
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


def install_ini(title, install):
    """the options file, its default folders under the title's %APPDATA% folder"""
    if not (install / "fci.ini").exists():
        text = (REPO / "src" / "fci.ini").read_text(encoding="utf-8")
        text = text.replace("%APPDATA%\\fci-recomp\\", f"%APPDATA%\\{title.appdata}\\")
        (install / "fci.ini").write_text(text, encoding="utf-8")


def build_parts(keys, parts, title, install, work, a):
    """the steps after the disc: compiler, toolkit, lift, build"""
    step(keys, "compiler")
    tc = find_toolchain(work, a.accept_msvc_license, a.download_compiler)
    step(keys, "toolkit")
    fetch_deps(work)
    step(keys, "lift", " (the game and the map editor side by side)" if len(parts) > 1 else "")
    translate(parts, title, install, work)
    step(keys, "build")
    for part in parts:
        compile_part(part, title, install, work, a.jobs, tc)
    install_ini(title, install)


def setup(a):
    started = time.time()
    check_python()
    source, title = open_disc(a.disc)
    print(title.name)
    install, work = Path(a.install or REPO / title.install).resolve(), Path(a.work).resolve()
    preflight(work, install, 2 if a.editor else 1, a.download_compiler)
    install.mkdir(parents=True, exist_ok=True)
    keys = plan("disc", "compiler", "toolkit", "lift", "build")
    step(keys, "disc")
    copy_game(source, title, install / "game")
    build_parts(keys, ["game", "editor"] if a.editor else ["game"], title, install, work, a)
    minutes = (time.time() - started) / 60
    print(f"Done in {minutes:.0f} minutes. Start {install / title.game_exe}; settings are in {install / 'fci.ini'}.")
    events.emit("done", minutes=round(minutes))
    if not a.editor:
        print("The map editor is not built; run BuildEditor.cmd (or: fci-recomp.py editor) to add it.")


def installed_title(a):
    """the game an editor build is for: --install's, --title's, or the one game set up here"""
    if a.install:
        game = Path(a.install) / "game" / "default.xbe"
        title = titles.of_image("default.xbe", game) if game.exists() else None
        if title is None:
            raise SystemExit(f"error: set up the game in {a.install} first (BuildGame.cmd or: fci-recomp.py setup)")
        return title, Path(a.install).resolve()
    if a.title:
        title = titles.TITLES[a.title]
        return title, (REPO / title.install).resolve()
    found = [t for t in titles.TITLES.values() if (REPO / t.install / "game" / "default.xbe").exists()]
    if len(found) != 1:
        raise SystemExit(
            "error: set up a game first (BuildGame.cmd)"
            if not found
            else "error: both games are set up here; choose one with --title instincts or --title evolution"
        )
    return found[0], (REPO / found[0].install).resolve()


def editor(a):
    started = time.time()
    check_python()
    if a.disc:
        _, title = open_disc(a.disc)
        install = Path(a.install or REPO / title.install).resolve()
    else:
        title, install = installed_title(a)
    work = Path(a.work).resolve()
    if not (install / title.game_exe).exists() or not (install / "game" / "ige.xbe").exists():
        raise SystemExit(f"error: set up the game in {install} first (BuildGame.cmd or: fci-recomp.py setup)")
    preflight(work, install, 1, a.download_compiler)
    build_parts(plan("compiler", "toolkit", "lift", "build"), ["editor"], title, install, work, a)
    minutes = (time.time() - started) / 60
    print(f"Done in {minutes:.0f} minutes. The game's Map Editor menu starts {install / title.editor_exe}.")
    events.emit("done", minutes=round(minutes))


def update(a):
    """rebuild with this version from the installed game files; the input digests decide what is redone"""
    started = time.time()
    check_python()
    title, install = installed_title(a)
    work = Path(a.work).resolve()
    if not (install / "game" / "default.xbe").exists():
        raise SystemExit(f"error: set up the game in {install} first (BuildGame.cmd or: fci-recomp.py setup)")
    parts = ["game"] + (["editor"] if (install / title.editor_exe).exists() else [])
    print(f"Updating {title.name} to {VERSION}")
    preflight(work, install, len(parts), a.download_compiler)
    build_parts(plan("compiler", "toolkit", "lift", "build"), parts, title, install, work, a)
    minutes = (time.time() - started) / 60
    print(f"Done in {minutes:.0f} minutes.")
    events.emit("done", minutes=round(minutes))


def status(a):
    """each game set up in --install (or this folder): what is built, with which version, and whether this
    version's update recompiles its code to C (the long step) or only builds"""
    import lift

    work = Path(a.work).resolve()
    folders = [Path(p).resolve() for p in a.install or [REPO / t.install for t in titles.TITLES.values()]]
    found = {}
    for install in folders:
        game = install / "game" / "default.xbe"
        title = titles.of_image("default.xbe", game) if game.exists() else None
        if title is None:
            continue
        record = install / "build.json"
        info = json.loads(record.read_text(encoding="utf-8")) if record.exists() else {}
        built = {part: (install / title.exe(image)).exists() for part, image in IMAGES.items()}
        found[title.key] = {
            "install": str(install),
            "built": built,
            "version": info.get("version"),
            "recompile": {
                part: not lift.current(image, title, parts_dir(work, title, "lift", image))
                for part, image in IMAGES.items()
                if built[part]
            },
        }
    print(json.dumps({"protocol": events.PROTOCOL, "version": VERSION, "titles": found}, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="action", required=True)
    s = sub.add_parser("setup", help="check the disc, recompile and build the game, install it")
    s.add_argument("disc")
    s.add_argument("--editor", action="store_true")
    e = sub.add_parser("editor", help="build the map editor for a game set up earlier")
    e.add_argument("disc", nargs="?")
    u = sub.add_parser("update", help="rebuild a game set up earlier with this version, without the disc")
    for p in (e, u):
        p.add_argument("--title", choices=sorted(titles.TITLES))
    for p in (s, e, u):
        p.add_argument("--install")
        p.add_argument("--work", default=str(REPO / "build"))
        p.add_argument("--jobs", type=int)
        p.add_argument("--accept-msvc-license", action="store_true")
        p.add_argument("--download-compiler", action="store_true")
        p.add_argument("--events", action="store_true")
    t = sub.add_parser("status", help="the games set up here, as JSON")
    t.add_argument("--install", action="append")
    t.add_argument("--work", default=str(REPO / "build"))
    a = ap.parse_args()
    events.enabled = getattr(a, "events", False)
    try:
        {"setup": setup, "editor": editor, "update": update, "status": status}[a.action](a)
    except SystemExit as e:
        if isinstance(e.code, str):
            events.emit("error", text=e.code.removeprefix("error: "))
        raise
    except Exception as e:  # the traceback still goes to stderr, which the launcher keeps in its log
        events.emit("error", text=f"{type(e).__name__}: {e}\n{what_to_do(e)}")
        traceback.print_exc()
        raise SystemExit(f"\nerror: {type(e).__name__}: {e}\n{what_to_do(e)}")


if __name__ == "__main__":
    main()
