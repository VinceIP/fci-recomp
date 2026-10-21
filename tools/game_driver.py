"""Drive a game or editor run by its state, not the clock (RECOMP_FCI_STATUS, fci_threads.c).

Steps run in order; a wait polls the status the runtime writes ten times a second, so a run takes
as long as the game does and no longer.

  wait:title            the startup movies are over (or skipped) and the controllers are polled
  wait:ingame           3D frames are being built steadily (gameplay, a cutscene, Jump In Map)
  wait:frames:<n>       n more frames presented
  wait:polls            the controllers are polled (the editor's menus, the title)
  sleep:<seconds>
  pad:<button>[+<button>][:<ms>]   live pad (RECOMP_PAD_LIVE syntax: a b x y start back lup rright ...)
  mouse:<dx>:<dy>       mouse motion in counts, as if captured (on foot: dx * 0.05 degrees of turn)
  set:<KEY>=<value>     change a graphics option now (RECOMP_FCI_XBR_*, src/xbr/fci_gfx.h)
  capture               the next presented frame, under capture/
  dump:<name>           guest RAM now, memdump-<name>.bin

  python -B tools/game_driver.py <name> [--editor] [--save <fixture>] [--opt RECOMP_X=v ...] <step> ...

Runs in <regress>/drive/<name>-<time> and prints each step's time. The builds under test are copied to
<regress>/bin first, so the installed game stays free to play or rebuild; the game is ended at the end,
whatever happened.

  FCI_TITLE     the game: instincts (default) or evolution (tools/titles.py: its executables' names)
  FCI_INSTALL   the installed game (default: the game's folder in the repository): its executable, the
                map editor's, fci-kinit.dll and the game files in game/
  FCI_GAME_EXE, FCI_EDITOR_EXE, FCI_GAME_DIR   other builds or game files instead
  FCI_REGRESS   the test folder: fixtures, references and runs (default: build/regress); it holds data
                from the game, so it stays out of Git
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import titles  # noqa: E402

TITLE = titles.TITLES[os.environ.get("FCI_TITLE", "instincts")]
INSTALL = Path(os.environ.get("FCI_INSTALL", REPO / TITLE.install))
GAME_DIR = Path(os.environ.get("FCI_GAME_DIR", INSTALL / "game"))
SOURCE_GAME_EXE = Path(os.environ.get("FCI_GAME_EXE", INSTALL / TITLE.game_exe))
SOURCE_EDITOR_EXE = Path(os.environ.get("FCI_EDITOR_EXE", INSTALL / TITLE.editor_exe))
REGRESS = Path(os.environ.get("FCI_REGRESS", REPO / "build" / "regress"))
FIXTURES = REGRESS / "fixtures"
# the copies that run (stage_builds)
GAME_EXE = REGRESS / "bin" / TITLE.game_exe
EDITOR_EXE = REGRESS / "bin" / TITLE.editor_exe
KINIT = REGRESS / "bin" / "fci-kinit.dll"


def stage_builds():
    """copy the builds under test to REGRESS/bin, where the runs start them"""
    GAME_EXE.parent.mkdir(parents=True, exist_ok=True)
    for src, dst in (
        (SOURCE_GAME_EXE, GAME_EXE),
        (SOURCE_EDITOR_EXE, EDITOR_EXE),
        (SOURCE_GAME_EXE.with_name("fci-kinit.dll"), KINIT),
    ):
        if not src.exists():
            if dst == EDITOR_EXE:  # an install without the editor; only --editor runs need it
                continue
            raise SystemExit(f"error: {src} is missing (set FCI_INSTALL or FCI_GAME_EXE / FCI_EDITOR_EXE)")
        if not dst.exists() or dst.read_bytes() != src.read_bytes():
            shutil.copy2(src, dst)


class StepFailed(Exception):
    pass


# FCI_TEST_QUIET=1 (the default): the game runs below normal priority and its window is kept beyond the
# right edge of the desktop, so a test run leaves the user's own work alone. Frames still come from the
# back buffer (capture) or PrintWindow, which work off screen. FCI_TEST_QUIET=0 shows the window.
QUIET = os.environ.get("FCI_TEST_QUIET", "1") != "0"
BELOW_NORMAL_PRIORITY_CLASS = 0x4000


def launch(argv, **kw):
    """subprocess.Popen for a game run, quiet unless FCI_TEST_QUIET=0"""
    if QUIET:
        kw["creationflags"] = kw.get("creationflags", 0) | BELOW_NORMAL_PRIORITY_CLASS
    child = subprocess.Popen(argv, **kw)
    if QUIET:
        threading.Thread(target=keep_off_screen, args=(child,), daemon=True).start()
    return child


def keep_off_screen(child):
    """move the game's frame window off the desktop as soon as it exists, and again if it comes back
    (the relaunched editor and game share the first instance's frame, so its process owns it)"""
    import ctypes
    import ctypes.wintypes as wt

    user32 = ctypes.windll.user32
    enum = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    x = user32.GetSystemMetrics(76) + user32.GetSystemMetrics(78) + 100  # SM_XVIRTUALSCREEN + SM_CXVIRTUALSCREEN
    name = ctypes.create_unicode_buffer(64)
    pid = wt.DWORD()
    rect = wt.RECT()

    def visit(h, _):
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == child.pid:
            user32.GetClassNameW(h, name, 64)
            user32.GetWindowRect(h, ctypes.byref(rect))
            if name.value == "FCIWholeFrame" and rect.left < x:
                user32.SetWindowPos(h, None, x, 0, 0, 0, 0x0001 | 0x0004 | 0x0010)  # NOSIZE NOZORDER NOACTIVATE
        return True

    callback = enum(visit)
    while child.poll() is None:
        user32.EnumWindows(callback, 0)
        time.sleep(0.05)


class Game:
    def __init__(self, folder, editor=False, save=None, options=(), skip_intros=True):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True)
        self.pad = self.folder / "pad.txt"
        self.cap = self.folder / "capture-live.txt"
        self.control = self.folder / "control.txt"
        self.status_file = self.folder / "status.json"
        for f in (self.pad, self.cap, self.control):
            f.write_text("")
        (self.folder / "capture").mkdir()
        if editor:
            exe, xbe = EDITOR_EXE, GAME_DIR / "ige.xbe"
            shutil.copytree(FIXTURES / "editor-save", self.folder / "save")
            shutil.copy2(FIXTURES / "editor-launch.bin", self.folder / "launch.bin")
            extra = ["RECOMP_LAUNCH_DATA=" + str(self.folder / "launch.bin")]
        else:
            exe, xbe = GAME_EXE, GAME_DIR / "default.xbe"
            if save:
                shutil.copytree(FIXTURES / save, self.folder / "save")
            else:
                (self.folder / "save").mkdir()
            extra = ["RECOMP_FCI_SKIP_INTROS=1"] if skip_intros else []
        self.argv = (
            [
                str(exe),
                str(xbe),
                str(GAME_DIR),
                str(self.folder / "save"),
                "RECOMP_USB=1",
                "RECOMP_FCI_XBR=1",
                "RECOMP_FCI_XBR_BACKGROUND=1",
                "RECOMP_FCI_XBR_NO_PRESENT_WAIT=1",
                "RECOMP_FCI_XBR_CAPTURE_LIVE=" + str(self.cap),
                "RECOMP_PAD_LIVE=" + str(self.pad),
                "RECOMP_FCI_STATUS=" + str(self.status_file),
                "RECOMP_FCI_CONTROL=" + str(self.control),
            ]
            + extra
            + list(options)
        )
        self.child = None
        self.log = []

    def __enter__(self):
        env = dict(os.environ, XBE_CAPTURE_DIR=str(self.folder / "capture"))
        env.pop("RECOMP_FCI_FRAME", None)
        self.err = open(self.folder / "stderr.log", "w")
        self.start = time.time()
        self.child = launch(self.argv, cwd=self.folder, env=env, stdout=subprocess.DEVNULL, stderr=self.err)
        return self

    def __exit__(self, *exc):
        subprocess.run(["taskkill", "/PID", str(self.child.pid), "/T", "/F"], capture_output=True)
        self.child.wait()
        self.err.close()
        return False

    def elapsed(self):
        return time.time() - self.start

    def status(self):
        try:
            return json.loads(self.status_file.read_text())
        except (OSError, ValueError):
            return {}

    def wait(self, test, what, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if self.child.poll() is not None:
                raise StepFailed(f"{what}: the game exited ({self.child.returncode})")
            st = self.status()
            if st and test(st):
                return st
            time.sleep(0.1)
        raise StepFailed(f"{what}: not reached in {timeout:.0f} s (last status {self.status()})")

    def step(self, spec, timeout=90):
        t0 = self.elapsed()
        kind, _, arg = spec.partition(":")
        if kind == "wait":
            if arg == "title":
                self.wait(lambda s: s.get("polls", 0) > 0 and s.get("movies", 0) >= 3, spec, timeout)
            elif arg == "polls":
                self.wait(lambda s: s.get("polls", 0) > 0, spec, timeout)
            elif arg == "ingame":
                prev = {}

                def steady(s):  # 3D frames grew by 10+ over the last second
                    now = time.time()
                    prev.setdefault("hist", []).append((now, s.get("frames3d", 0)))
                    hist = [h for h in prev["hist"] if now - h[0] <= 1.0]
                    prev["hist"] = hist
                    return len(hist) > 5 and hist[-1][1] - hist[0][1] >= 10

                self.wait(steady, spec, timeout)
            elif arg.startswith("frames:"):
                base = self.status().get("presents", 0)
                n = int(arg.split(":")[1])
                self.wait(lambda s: s.get("presents", 0) >= base + n, spec, timeout)
            else:
                raise StepFailed(f"unknown wait {arg}")
        elif kind == "sleep":
            time.sleep(float(arg))
        elif kind == "pad":
            with open(self.pad, "a") as f:
                f.write(arg + "\n")
            hold = arg.split(":")[1] if ":" in arg else "150"
            time.sleep(int(hold) / 1000 + 0.05)
        elif kind == "mouse":
            dx, dy = (int(v) for v in arg.split(":"))
            with open(self.control, "a") as f:
                f.write(f"mouse {dx} {dy}\n")
            time.sleep(0.3)  # read within 100 ms, applied on the next frame
        elif kind == "set":
            with open(self.control, "a") as f:
                f.write(f"set {arg}\n")
            time.sleep(0.3)  # read within 100 ms, applied at the next present
        elif kind == "capture":
            n = len(list((self.folder / "capture").iterdir()))
            with open(self.cap, "a") as f:
                f.write("capture\n")
            end = time.time() + 10
            while time.time() < end and not any(
                p.joinpath("final.bmp").exists() for p in list((self.folder / "capture").iterdir())[n:]
            ):
                time.sleep(0.1)
            time.sleep(1.1)  # capture folders are named by the second
        elif kind == "dump":
            with open(self.control, "a") as f:
                f.write(f"dump {arg}\n")
            target = self.folder / f"memdump-{arg}.bin"
            end = time.time() + 20
            while time.time() < end and (not target.exists() or target.stat().st_size < 0x03FF0000):
                time.sleep(0.1)
        else:
            raise StepFailed(f"unknown step {spec}")
        self.log.append((spec, round(t0, 1), round(self.elapsed() - t0, 1)))

    def frames(self):
        return sorted((self.folder / "capture").glob("*/final.bmp"))


def run(name, steps, **kw):
    stage_builds()
    folder = REGRESS / "drive" / f"{name}-{time.strftime('%H%M%S')}"
    with Game(folder, **kw) as g:
        try:
            for s in steps:
                g.step(s)
            ok, why = True, ""
        except StepFailed as e:
            ok, why = False, str(e)
        log = (folder / "stderr.log").read_text(errors="replace").splitlines()
        stops = [line for line in log if "[FCI] STOP" in line]
    return {
        "ok": ok and not stops,
        "why": why or "; ".join(stops),
        "steps": g.log,
        "folder": folder,
        "frames": g.frames(),
        "seconds": round(g.elapsed(), 1),
    }


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    name, rest = argv[0], argv[1:]
    kw, opts, steps = {}, [], []
    while rest:
        a = rest.pop(0)
        if a == "--editor":
            kw["editor"] = True
        elif a == "--save":
            kw["save"] = rest.pop(0)
        elif a == "--opt":
            opts.append(rest.pop(0))
        elif a == "--intros":
            kw["skip_intros"] = False
        else:
            steps.append(a)
    r = run(name, steps, options=opts, **kw)
    for spec, at, took in r["steps"]:
        print(f"  {at:7.1f} s  +{took:5.1f}  {spec}")
    print(f"{'ok' if r['ok'] else 'FAILED: ' + r['why']} in {r['seconds']} s; {r['folder']}")
    return 0 if r["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
