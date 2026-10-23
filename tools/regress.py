"""Regression suite for the whole-program builds: scripted runs, visible stops, frame checks.

Each scenario runs copies of the builds under test (tools/game_driver.py: FCI_INSTALL and the other
variables) in a folder of its own with copied fixtures, drives the pad, captures frames and checks:
  - no `[FCI] STOP` (and no unexpected exit) in the logs,
  - required log lines (e.g. both relaunches of the editor round trip),
  - each captured frame is not black,
  - each frame against its reference: a 160x120 greyscale copy, by the mean absolute difference and
    by the worst 8x8 block (a missing piece of a model is local),
  - scenario extras (one window in front across relaunches, each instance's own; the Z: cache copies
    carry the disc files' times).
References and fixtures come from the game, so they live in the test folder (FCI_REGRESS, default
build/regress), never in Git. Fixtures, each copied in once:
  save-tunnel        a save folder whose checkpoint is Story Mode's Training tunnel
  editor-save        the save folder of a game that went to the Map Editor
  editor-launch.bin  the launch page that switch wrote (RECOMP_LAUNCH_DATA)

  python -B tools/regress.py fixture <name> <path>   copy a fixture into the test folder
  python -B tools/regress.py run [names...]      run all (or the named) scenarios, write a report
  python -B tools/regress.py run --bless [names] record the frames as the new references
  python -B tools/regress.py list

Timing is wall-clock, so frames vary a little run to run (two runs: mean <= 2.6, worst block <= 31 on
the animated title); the block tolerance is per scenario and the report prints every difference. A failed
check names the scenario folder to look at.
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import game_driver
from game_driver import EDITOR_EXE, FIXTURES, GAME_DIR, GAME_EXE, KINIT, REGRESS  # noqa: E402

GOLDEN = REGRESS / "golden"
FIXTURE_NAMES = ("save-tunnel", "editor-save", "editor-launch.bin")

# the editor's menus to terrain mode (Create a New Map), about 60 s after start
EDITOR_TO_TERRAIN = ["16.2:down:200", "16.6:a:200", "29.4:up:200", "29.8:a:200", "31.4:a:200", "50.9:a:200"]

SCENARIOS = {
    "boot": {
        "what": "game boots to the title screen (intro movies skipped)",
        "kind": "driven",
        "save": None,
        "seconds": 15,
        "steps": ["wait:title", "wait:frames:60", "capture"],
        "block": 25,
    },
    "story-tunnel": {
        "what": "Story Mode resumes the Training tunnel checkpoint; walk out, first-person machete",
        "kind": "driven",
        "save": "save-tunnel",
        "seconds": 35,
        "steps": [
            "wait:title",
            "sleep:2",
            "pad:start:200",
            "sleep:3",
            "pad:a:200",
            "sleep:3",
            "pad:a:200",
            "sleep:3",
            "pad:a:200",
            "wait:ingame",
            "sleep:2",
            "capture",
            "sleep:1.5",
            "pad:lup:2000",
            "sleep:1",
            "capture",
            "sleep:3",
            "capture",
        ],
        "block": 30,
    },
    "mouse-aim": {
        "what": "direct mouse aim on foot: 7200 counts turn exactly 360 degrees (back to the tunnel exit), "
        "then 300 counts look 15 degrees up",
        "kind": "driven",
        "save": "save-tunnel",
        "seconds": 30,
        "steps": [
            "wait:title",
            "sleep:2",
            "pad:start:200",
            "sleep:3",
            "pad:a:200",
            "sleep:3",
            "pad:a:200",
            "sleep:3",
            "pad:a:200",
            "wait:ingame",
            "sleep:2",
            "capture",
            "mouse:7200:0",
            "sleep:0.5",
            "capture",
            "mouse:0:-300",
            "sleep:0.5",
            "capture",
        ],
        "block": 30,
    },
    "editor-terrain": {
        "what": "map editor: Create a New Map, terrain bump with Y raises a hill",
        "kind": "editor",
        "seconds": 72,
        "plan": EDITOR_TO_TERRAIN + ["61:capture", "62:y:4000", "68:capture"],
        "block": 30,
    },
    "editor-jump": {
        "what": "map editor: Jump In Map walks the new map with hand and pistol",
        "kind": "editor",
        "seconds": 80,
        "plan": EDITOR_TO_TERRAIN + ["62:back:300", "75:capture", "76.5:capture"],
        "block": 30,
    },
    "relaunch-window": {
        "what": "game -> Map Editor -> Exit Editor -> main menu, one window in front, each instance's own",
        "kind": "flow",
        "seconds": 225,
        "pad_script": "70000:start,85000:down,87000:down,89000:a",
        "grabs": [40, 120, 220],
        "block": 70,
        "mean": 12,  # the title animates (Bink): mean 6.2, block 48.7 seen
        "log": ["relaunch: \\Device\\CdRom0;ige.xbe", "relaunch: \\Device\\CdRom0;default.xbe"],
    },
    "editor-graphics": {
        "what": "game -> Map Editor with the display and rendering options set by their fci.ini keys: both "
        "instances widescreen at 1920x1080 with ambient occlusion (split view), anti-aliasing and the "
        "engine variables",
        "kind": "flow",
        "seconds": 130,
        "pad_script": "70000:start,85000:down,87000:down,89000:a",
        # the player's keys (fci_config.c), and the developer-only AO split view
        "ini": [
            "display-aspect = 1",
            "gfx-res-scale = 2.25",
            "RECOMP_FCI_XBR_AO=3",
            "gfx-aa = 1",
            "gfx-anisotropic-filtering = 16",
            "gfx-shadow-scale = 4",
            "gfx-shadow-filter = 1",
            "gfx-object-draw-distance = 200",
        ],
        "grabs": [40, 125],
        "block": 70,
        "mean": 12,  # the title and the editor's menu animate
        "log": ["relaunch: \\Device\\CdRom0;ige.xbe"],
        # the game and then the editor: each made its device widescreen, rendered at 1920x1080 and
        # registered the variable with the option's value
        "log_count": {
            "flags 30": 2,
            "render scale 3.000 x 2.250": 2,
            "cvar e_obj_view_dist_ratio = 200 (default 48)": 2,
        },
    },
}


# Evolution (FCI_TITLE=evolution): each run makes a new profile, so no fixtures; the attract video that
# follows a return to the main menu is skipped with the intros
EVO_TO_MENU = ["wait:title", "sleep:2", "pad:start:200", "sleep:4", "pad:a:200", "sleep:4"]
# 3D frames start with the level's opening scene in the jeep (about 20 s); the gun is free after it
EVO_TO_GAME = EVO_TO_MENU + ["pad:a:200", "sleep:4", "pad:a:200", "wait:ingame", "sleep:25"]
# Map Editor (a relaunch into the editor), Skip Xbox Live Sign-In, OK, Create a New Map, Tropical: the terrain tool
EVO_TO_EDITOR = (
    EVO_TO_MENU
    + ["pad:down:150", "sleep:0.5"] * 3
    + ["pad:a:200", "sleep:28", "pad:down:150", "sleep:0.5", "pad:a:200", "sleep:2", "pad:up:150", "sleep:0.5"]
    + ["pad:a:200", "sleep:5", "pad:a:200", "sleep:5", "pad:a:200", "sleep:20"]
)
EVO_SCENARIOS = {
    "boot": {
        "what": "Evolution boots to the title screen (intro movies skipped)",
        "kind": "driven",
        "save": None,
        "seconds": 15,
        "steps": ["wait:title", "wait:frames:60", "capture"],
        "block": 140,  # the title's background is a video
        "mean": 20,
    },
    "menu": {
        "what": "Start: a new profile (Jack Carver) on the virtual keyboard, accepted, the main menu",
        "kind": "driven",
        "save": None,
        "seconds": 25,
        "steps": EVO_TO_MENU + ["capture"],
        "block": 140,  # the menu backgrounds are videos and the curtain sweeps: only a gross change counts
        "mean": 20,
        # the Z: copies carry the disc files' times, or every boot copies them again (runtime patch 0020)
        "cache_times": True,
    },
    "story": {
        "what": "Story Mode, Easy: the first level in the jeep's mounted gun, then the stick turns it",
        "kind": "driven",
        "save": None,
        "seconds": 85,
        "steps": EVO_TO_GAME + ["capture", "pad:rleft:1500", "sleep:1", "capture"],
        # the jeep drives on: a second earlier or later is another view, so only a gross change counts
        "block": 170,
        "mean": 40,
    },
    "mouse-aim": {
        "what": "direct mouse aim in the first level: 1000 counts turn the gun 50 degrees and back",
        "kind": "driven",
        "save": None,
        "seconds": 85,
        "steps": EVO_TO_GAME
        + ["capture", "mouse:1000:0", "sleep:0.5", "capture", "mouse:-1000:0", "sleep:0.5", "capture"],
        "block": 170,  # the jeep drives on, as in story
        "mean": 40,
    },
    "editor-terrain": {
        "what": "Map Editor from the main menu (a relaunch): a new Tropical map, Y raises a hill with the terrain tool",
        "kind": "driven",
        "save": None,
        "seconds": 120,
        "steps": EVO_TO_EDITOR + ["capture", "pad:y:3000", "sleep:1", "capture"],
        "block": 60,
        "mean": 15,
        "log": [r"relaunch: \Device\CdRom0;ige.xbe"],
    },
    "editor-jump": {
        "what": "Map Editor: Jump In Map (Back) walks the new map",
        "kind": "driven",
        "save": None,
        "seconds": 130,
        "steps": EVO_TO_EDITOR + ["pad:back:300", "sleep:12", "capture", "pad:lup:1500", "sleep:0.5", "capture"],
        "block": 60,
        "mean": 15,
    },
    "quit-menu": {
        "what": "pause, Quit to Main Menu, OK: the game relaunches and shows the main menu",
        "kind": "driven",
        "save": None,
        "seconds": 90,
        "steps": EVO_TO_GAME
        + ["pad:start:200", "sleep:2"]
        + ["pad:down:150", "sleep:0.4"] * 6
        + ["pad:a:200", "sleep:2", "pad:up:200", "sleep:1", "pad:a:200", "sleep:12", "capture"],
        "block": 170,  # as menu, and the relaunch lands anywhere in the background video's loop (144 seen)
        "mean": 20,
        "log": ["relaunch: \\Device\\CdRom0;default.xbe"],
    },
}
if game_driver.TITLE.key == "evolution":
    SCENARIOS, FIXTURE_NAMES = EVO_SCENARIOS, ()


def fixture(name, src):
    if name not in FIXTURE_NAMES:
        raise SystemExit("fixtures: " + ", ".join(FIXTURE_NAMES))
    src, dst = Path(src), FIXTURES / name
    if dst.exists():
        raise SystemExit(f"error: {dst} exists; remove it first to replace it")
    FIXTURES.mkdir(parents=True, exist_ok=True)
    (shutil.copytree if src.is_dir() else shutil.copy2)(src, dst)
    print("fixture", name, "from", src)


def lum_stats(path):
    from PIL import Image

    im = Image.open(path).convert("L").resize((160, 120))
    return im, sum(im.tobytes()) / (160 * 120)


MEAN_TOLERANCE = 6.0  # whole-frame mean; two runs of each scenario differed by at most 2.6


def diff(a, b):
    """mean absolute difference over the frame, and the worst 8x8 block's (a missing piece of a
    model is local: it barely moves the mean)"""
    pa, pb = a.tobytes(), b.tobytes()
    mean = sum(abs(x - y) for x, y in zip(pa, pb)) / (160 * 120)
    worst = 0.0
    for by in range(15):
        for bx in range(20):
            s = 0
            for y in range(by * 8, by * 8 + 8):
                o = y * 160 + bx * 8
                s += sum(abs(pa[o + i] - pb[o + i]) for i in range(8))
            worst = max(worst, s / 64)
    return mean, worst


def timeline(child, start, plan, pad, cap):
    for t, line in sorted((p.split(":", 1) for p in plan), key=lambda p: float(p[0])):
        while time.time() - start < float(t):
            if child.poll() is not None:
                return
            time.sleep(0.05)
        with open(cap if line == "capture" else pad, "a") as f:
            f.write(line + "\n")


def run_driven(sc, folder):
    """steps that wait on the game's state (tools/game_driver.py, RECOMP_FCI_STATUS)"""
    shutil.rmtree(folder)  # the driver makes it
    g = game_driver.Game(folder, save=sc.get("save"))
    with g:
        failed = None
        try:
            for step in sc["steps"]:
                g.step(step)
        except game_driver.StepFailed as e:
            failed = str(e)
    return {"early_exit": None, "logs": [folder / "stderr.log"], "frames": g.frames(), "failed": failed, "steps": g.log}


def run_direct(sc, folder):
    """game or editor executable started with arguments, live pad and live captures"""
    pad, cap, capdir = folder / "pad.txt", folder / "capture-live.txt", folder / "capture"
    pad.write_text("")
    cap.write_text("")
    capdir.mkdir()
    if sc["kind"] == "game":
        exe, xbe = GAME_EXE, GAME_DIR / "default.xbe"
        save = folder / "save"
        if sc["save"]:
            shutil.copytree(FIXTURES / sc["save"], save)
        else:
            save.mkdir()
        extra = []
    else:
        exe, xbe = EDITOR_EXE, GAME_DIR / "ige.xbe"
        shutil.copytree(FIXTURES / "editor-save", folder / "save")
        shutil.copy2(FIXTURES / "editor-launch.bin", folder / "launch.bin")
        save = folder / "save"
        extra = ["RECOMP_LAUNCH_DATA=" + str(folder / "launch.bin")]
    # the startup movies are skipped (RECOMP_FCI_SKIP_INTROS): 25 s less per game scenario
    argv = [
        str(exe),
        str(xbe),
        str(GAME_DIR),
        str(save),
        "RECOMP_USB=1",
        "RECOMP_FCI_XBR=1",
        "RECOMP_FCI_SKIP_INTROS=1",
        "RECOMP_FCI_XBR_BACKGROUND=1",
        "RECOMP_FCI_XBR_NO_PRESENT_WAIT=1",
        "RECOMP_FCI_XBR_CAPTURE_LIVE=" + str(cap),
        "RECOMP_PAD_LIVE=" + str(pad),
    ] + extra
    env = dict(os.environ, XBE_CAPTURE_DIR=str(capdir))
    env.pop("RECOMP_FCI_FRAME", None)
    start = time.time()
    with open(folder / "stderr.log", "w") as err:
        child = game_driver.launch(argv, cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=err)
        try:
            timeline(child, start, sc["plan"], pad, cap)
            while time.time() - start < sc["seconds"] and child.poll() is None:
                time.sleep(0.2)
            early = child.poll()
        finally:  # never leave a game running, whatever failed
            subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], capture_output=True)
            child.wait()
    frames = sorted(capdir.glob("*/final.bmp"))
    return {"early_exit": early, "logs": [folder / "stderr.log"], "frames": frames}


# ---- the relaunch scenario: no arguments, fci.ini beside the executables, window checks ----
user32 = ctypes.windll.user32
WNDENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def window_class(h):
    b = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, b, 64)
    return b.value


def window_pid(h):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def frames_and_children():
    frames = []
    user32.EnumWindows(
        WNDENUM(lambda h, _: frames.append(h) or True if window_class(h) == "FCIWholeFrame" else True), 0
    )
    out = []
    for f in frames:
        kids = []
        user32.EnumChildWindows(f, WNDENUM(lambda h, _: kids.append(h) or True), 0)  # noqa: B023 (called here)
        out.append((f, kids))
    return out


def grab(h, path):
    from PIL import Image

    r = wt.RECT()
    user32.GetClientRect(h, ctypes.byref(r))
    w, hh = r.right, r.bottom
    if w <= 0 or hh <= 0:
        return False  # minimised or not laid out: nothing to grab
    g = ctypes.windll.gdi32
    dc = user32.GetDC(h)
    mdc = g.CreateCompatibleDC(dc)
    bmp = g.CreateCompatibleBitmap(dc, w, hh)
    g.SelectObject(mdc, bmp)
    user32.PrintWindow(h, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT

    class Bih(ctypes.Structure):
        _fields_ = [
            ("size", wt.DWORD),
            ("w", wt.LONG),
            ("h", wt.LONG),
            ("planes", wt.WORD),
            ("bits", wt.WORD),
            ("comp", wt.DWORD),
            ("img", wt.DWORD),
            ("xp", wt.LONG),
            ("yp", wt.LONG),
            ("used", wt.DWORD),
            ("imp", wt.DWORD),
        ]

    buf = ctypes.create_string_buffer(w * hh * 4)
    g.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(Bih(40, w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)), 0)
    Image.frombuffer("RGBA", (w, hh), buf, "raw", "BGRA", 0, 1).convert("RGB").save(path)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    user32.ReleaseDC(h, dc)
    return True


def run_flow(sc, folder):
    shutil.copy2(GAME_EXE, folder / GAME_EXE.name)
    shutil.copy2(EDITOR_EXE, folder / EDITOR_EXE.name)
    shutil.copy2(KINIT, folder / "fci-kinit.dll")
    shutil.copytree(FIXTURES / "save-tunnel", folder / "save")
    (folder / "fci.ini").write_text(
        "\n".join(
            [
                "; regression scenario copy",
                "game=" + str(GAME_DIR),
                "save=" + str(folder / "save"),
                "log=" + str(folder / "fci.log"),
                "RECOMP_FCI_XBR_BACKGROUND=1",
                "RECOMP_FCI_XBR_NO_PRESENT_WAIT=1",
                "RECOMP_FCI_SKIP_INTROS=1",
                "RECOMP_PAD_SCRIPT=" + sc["pad_script"],
                *sc.get("ini", []),
                "",
            ]
        )
    )
    env = dict(os.environ)
    env.pop("RECOMP_FCI_FRAME", None)
    start = time.time()
    child = game_driver.launch([str(folder / GAME_EXE.name)], cwd=folder, env=env)
    frames, windows = [], []
    try:
        early = flow_timeline(sc, folder, start, child, frames, windows)
    finally:  # the first instance's job takes the later ones with it
        subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], capture_output=True)
        child.wait()
    return {"early_exit": early, "logs": [folder / "fci.log"], "frames": frames, "windows": windows}


def flow_timeline(sc, folder, start, child, frames, windows):
    for t in sc["grabs"]:
        while time.time() - start < t and child.poll() is None:
            time.sleep(0.2)
        # the session's visible frames: each instance runs an executable copied into the folder
        mine = [
            (f, k)
            for f, k in frames_and_children()
            if user32.IsWindowVisible(f) and Path(game_driver.process_path(window_pid(f))).parent == folder
        ]
        r = wt.RECT()
        if mine:
            user32.GetWindowRect(mine[0][0], ctypes.byref(r))
        windows.append(
            {
                "t": t,
                "frames": len(mine),
                "frame_pids": [window_pid(f) for f, _ in mine],
                "render_pids": [window_pid(k) for _, kids in mine for k in kids],
                "rect": [r.left, r.top, r.right, r.bottom],
            }
        )
        if mine:
            path = folder / f"grab-{t}.png"
            if grab(mine[0][0], path):
                frames.append(path)
            else:
                r = wt.RECT()
                user32.GetWindowRect(mine[0][0], ctypes.byref(r))
                windows[-1]["empty_client"] = [r.left, r.top, r.right, r.bottom, bool(user32.IsIconic(mine[0][0]))]
    return child.poll()


def cache_time_problems(cache, disc):
    """the game's copies in its Z: cache against the disc files: the game copies again when the times differ"""
    originals = {p.name.lower(): p for p in disc.glob("*.ss2")} if disc.is_dir() else {}
    copies = sorted(cache.glob("*.ss2")) if cache.is_dir() else []
    if not copies:
        return [f"no cache copies in {cache}"]
    return [
        f"cache copy {c.name} has another time than the disc's"
        for c in copies
        if c.name.lower() in originals and c.stat().st_mtime_ns != originals[c.name.lower()].stat().st_mtime_ns
    ]


def check(name, sc, result, folder, bless):
    problems, notes = [], []
    text = "".join(p.read_text(errors="replace") for p in result["logs"] if p.exists())
    for line in text.splitlines():
        if "[FCI] STOP" in line:
            problems.append("stop: " + line.strip())
    if result["early_exit"] is not None:
        problems.append(f"exited early with {result['early_exit']}")
    if result.get("failed"):
        problems.append("step failed: " + result["failed"])
    for needle in sc.get("log", []):
        if needle not in text:
            problems.append("log line missing: " + needle)
    for needle, count in sc.get("log_count", {}).items():
        if text.count(needle) < count:
            problems.append(f"log line {needle!r} seen {text.count(needle)} times, {count} expected")
    if sc.get("cache_times"):
        problems += cache_time_problems(folder / "save" / "Cache" / "sounds_xb", GAME_DIR / "sounds_xb")
    if "windows" in result:
        w = result["windows"]
        if any(x["frames"] != 1 for x in w):
            problems.append("expected one visible frame window at a time: " + json.dumps(w))
        pids = [x["render_pids"][0] if x["render_pids"] else None for x in w]
        if len(set(pids)) != len(pids) or None in pids:
            problems.append("expected a new render window per instance: " + json.dumps(w))
        # the process that draws owns the window in front (overlays follow it), in the first one's place
        if any(x["frame_pids"] != x["render_pids"][:1] for x in w):
            problems.append("expected each instance's frame to be its own: " + json.dumps(w))
        if len({tuple(x["rect"]) for x in w}) > 1:
            problems.append("expected every frame in the same place: " + json.dumps(w))
        notes.append("windows " + json.dumps(w))
    expected = (
        sum(1 for p in sc.get("plan", []) if p.endswith(":capture"))
        or len(sc.get("grabs", []))
        or sum(1 for p in sc.get("steps", []) if p == "capture")
    )
    if len(result["frames"]) != expected:
        problems.append(f"{len(result['frames'])} frames captured, {expected} expected")
    gdir = GOLDEN / name
    for i, frame in enumerate(result["frames"]):
        im, mean = lum_stats(frame)
        if mean < 4:
            problems.append(f"frame {i} is black (mean {mean:.1f})")
        ref = gdir / f"{i}.png"
        if bless:
            gdir.mkdir(parents=True, exist_ok=True)
            im.save(ref)
            notes.append(f"frame {i} blessed")
        elif ref.exists():
            from PIL import Image

            mean, block = diff(im, Image.open(ref).convert("L"))
            limit = sc.get("mean", MEAN_TOLERANCE)
            notes.append(f"frame {i} mean {mean:.1f} (<= {limit}), worst block {block:.1f} (<= {sc['block']})")
            if mean > limit or block > sc["block"]:
                problems.append(f"frame {i} differs from its reference: mean {mean:.1f}, worst block {block:.1f}")
        else:
            notes.append(f"frame {i}: no reference yet (run with --bless)")
    return problems, notes


def contact_sheet(rows, path):
    from PIL import Image

    tiles = [(n, f) for n, fs in rows for f in fs]
    if not tiles:
        return
    cols = 4
    sheet = Image.new("RGB", (cols * 320, ((len(tiles) + cols - 1) // cols) * 240))
    for k, (_, f) in enumerate(tiles):
        sheet.paste(Image.open(f).convert("RGB").resize((320, 240)), ((k % cols) * 320, (k // cols) * 240))
    sheet.save(path)


def leftovers():
    """processes of earlier runs (an interrupted run can leave a game playing its pad script): only
    copies under REGRESS, never the installed game"""
    ps = (
        "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and "
        f"$_.ExecutablePath -like '{REGRESS}{chr(92)}*' }} | ForEach-Object {{ $_.ProcessId }}"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.isdigit()]


def run(names, bless):
    stale = leftovers()
    for pid in stale:
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    if stale:
        print(f"ended {len(stale)} leftover process(es) of earlier runs: {stale}")
    missing = [n for n in FIXTURE_NAMES if not (FIXTURES / n).exists()]
    if missing:
        raise SystemExit(f"fixtures missing ({', '.join(missing)}): add them with `regress.py fixture`")
    game_driver.stage_builds()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = REGRESS / "runs" / stamp
    out.mkdir(parents=True)
    report = {"stamp": stamp, "bless": bless, "builds": {}, "scenarios": {}}
    import hashlib

    for label, exe in (("game", GAME_EXE), ("editor", EDITOR_EXE)):
        if exe.exists():  # a game without its editor build
            report["builds"][label] = hashlib.sha256(exe.read_bytes()).hexdigest()
    rows = []
    for name in names:
        sc = SCENARIOS[name]
        folder = out / name
        folder.mkdir()
        print(f"{name}: {sc['what']} ({sc['seconds']} s)", flush=True)
        t0 = time.time()
        result = (
            run_flow(sc, folder)
            if sc["kind"] == "flow"
            else run_driven(sc, folder)
            if sc["kind"] == "driven"
            else run_direct(sc, folder)
        )
        problems, notes = check(name, sc, result, folder, bless)
        report["scenarios"][name] = {
            "passed": not problems,
            "problems": problems,
            "notes": notes,
            "seconds": round(time.time() - t0, 1),
            "folder": str(folder),
        }
        rows.append((name, result["frames"]))
        print(f"  {'PASS' if not problems else 'FAIL'}" + "".join("\n    " + p for p in problems + notes), flush=True)
    contact_sheet(rows, out / "contact.png")
    (out / "report.json").write_text(json.dumps(report, indent=1))
    failed = [n for n, r in report["scenarios"].items() if not r["passed"]]
    print(f"{len(names) - len(failed)}/{len(names)} passed; report {out / 'report.json'}")
    return 1 if failed else 0


def main(argv):
    if not argv or argv[0] == "list":
        for n, sc in SCENARIOS.items():
            print(f"{n:16} {sc['seconds']:4} s  {sc['what']}")
        return 0
    if argv[0] == "fixture" and len(argv) == 3:
        fixture(argv[1], argv[2])
        return 0
    if argv[0] == "run":
        rest = argv[1:]
        bless = "--bless" in rest
        names = [a for a in rest if a != "--bless"] or list(SCENARIOS)
        unknown = [n for n in names if n not in SCENARIOS]
        if unknown:
            raise SystemExit("unknown scenario: " + ", ".join(unknown))
        return run(names, bless)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
