"""Turn an input recording (RECOMP_FCI_RECORD, src/fci_record.c) into a test run for tools/game_driver.py.

  python -B tools/record2script.py <recording> <out stem> [--save <fixture>] [--max-gap <s>] [--capture]

Writes <out stem>.replay, the play after the player first appears (times in game seconds from then, which
RECOMP_FCI_PLAYBACK replays on the game's clock, so at any frame rate), and <out stem>.steps, the driver's
arguments: the menu presses before it as live pad steps, then waits for the game and for the replay's end.

  python -B tools/game_driver.py <name> @<out stem>.steps [--opt RECOMP_FCI_XBR_FPS=120 ...]

Menu presses keep their recorded hold; the time between them is the recorded one up to --max-gap (default
1.5 s: time spent reading a menu is not needed), and the first waits for the title (wait:title).
"""

import argparse
from pathlib import Path
import struct

# report byte 2 and the analog bytes 4..11 (usb_gamepad.c's names, the driver's pad: syntax)
DIGITAL = ["up", "down", "left", "right", "start", "back", "lthumb", "rthumb"]
ANALOG = ["a", "b", "x", "y", "black", "white", "lt", "rt"]
PRESSED = 0x20  # an analog button counts from here
THROW = 16384  # a stick counts as pushed from half its throw


def buttons(report):
    """the driver's names for what a 20-byte report holds; empty when idle"""
    names = [n for i, n in enumerate(DIGITAL) if report[2] >> i & 1]
    names += [n for i, n in enumerate(ANALOG) if report[4 + i] >= PRESSED]
    lx, ly, rx, ry = struct.unpack_from("<4h", report, 12)
    for side, x, y in (("l", lx, ly), ("r", rx, ry)):
        names += [side + "left"] if x <= -THROW else [side + "right"] if x >= THROW else []
        names += [side + "up"] if y >= THROW else [side + "down"] if y <= -THROW else []
    return names


def parse(path):
    pads, aims, states, sync = [], [], [], None
    for line in Path(path).read_text().splitlines():
        w = line.split()
        if not w or w[0].startswith("#"):
            continue
        if w[0] == "pad":
            pads.append((float(w[1]), float(w[2]), int(w[3]), bytes.fromhex(w[4])))
        elif w[0] == "aim":
            aims.append((float(w[1]), float(w[2]), w[3], w[4]))
        elif w[0] == "state":
            # the player's entity id and the id of what the view controls (another: a vehicle, presumably)
            states.append((float(w[1]), float(w[2]), w[8], w[9]))
        elif w[0] == "player" and int(w[3], 16) and sync is None:
            sync = (float(w[1]), float(w[2]))
    if sync is None:
        raise SystemExit(f"error: {path}: the player never appears (no gameplay to replay)")
    return pads, aims, states, sync


def still(states, ms, t):
    """whether the game's clock had stood at t for the 300 ms of state before host time ms"""
    near = [s for s in states if ms - 300 <= s[0] <= ms]
    return len(near) >= 2 and all(abs(s[1] - t) < 0.0005 for s in near)


def controlled(states, sync_ms):
    """spans of game time when the view controls something other than the player: (first, last, id)"""
    spans = []
    for ms, t, pid, cid in states:
        if ms < sync_ms or int(pid, 16) == 0 or cid == pid:
            continue
        if spans and spans[-1][2] == cid and t - spans[-1][1] < 0.5:
            spans[-1][1] = t
        else:
            spans.append([t, t, cid])
    return spans


def menu_steps(pads, sync_ms, max_gap):
    """pad 0's presses before the sync as driver steps"""
    presses, held, since = [], [], 0.0
    for ms, _, pad, report in pads:
        if pad or ms >= sync_ms:
            continue
        now = buttons(report)
        if now != held:
            if held:
                presses.append((since, ms, held))
            held, since = now, ms
    steps, last = ["wait:title"], None
    for start, end, names in presses:
        if last is not None:
            gap = min((start - last) / 1000, max_gap)
            if gap >= 0.05:
                steps.append(f"sleep:{gap:.2f}")
        steps.append(f"pad:{'+'.join(names)}:{max(100, round(end - start))}")
        last = end
    return steps, held


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("recording")
    ap.add_argument("out")
    ap.add_argument("--save", help="the fixture the recording started from (game_driver --save)")
    ap.add_argument("--max-gap", type=float, default=1.5)
    ap.add_argument("--capture", action="store_true", help="capture a frame at the end")
    a = ap.parse_args()
    pads, aims, states, (sync_ms, sync_s) = parse(a.recording)
    steps, held = menu_steps(pads, sync_ms, a.max_gap)
    if held:
        print(f"warning: {'+'.join(held)} still held when the player appears; the replay starts with it")

    out = Path(a.out).resolve()
    replay = out.with_suffix(".replay")
    lines = [f"# from {Path(a.recording).name}: game seconds from the player's first appearance"]
    before, prev = {}, {}  # each pad's report at the sync, so a held button stays held; its last report
    for ms, t, pad, report in pads:
        if ms < sync_ms:
            before[pad] = report
            continue
        last_ms = prev.get(pad, sync_ms)
        prev[pad] = ms
        # the clock stood still (a loading screen): its value depends on the frame rate, so the replay waits
        # for it to stand still and then for the recorded host time since the report before, up to max_gap
        wait = f" {min(ms - last_ms, a.max_gap * 1000):.0f}" if still(states, ms, t) else ""
        lines.append(f"pad {max(0.0, t - sync_s):.4f} {pad} {report.hex().upper()}{wait}")
    lines[1:1] = [f"pad 0.0000 {pad} {r.hex().upper()}" for pad, r in sorted(before.items()) if any(r[2:])]
    lines += [f"aim {t - sync_s:.4f} {yaw} {pitch}" for ms, t, yaw, pitch in aims if ms >= sync_ms]
    end = max([s[1] for s in states] + [t for _, t, _, _ in pads] + [t for _, t, _, _ in aims]) - sync_s
    lines.append(f"end {end:.4f}")
    replay.write_text("\n".join(lines) + "\n")

    head = [f"# from {Path(a.recording).name}: {len(steps) - 1} menu steps, then {end:.1f} s of play"]
    head += [f"--save {a.save}"] if a.save else []
    head += [f"--opt RECOMP_FCI_PLAYBACK={replay}"]
    tail = ["wait:ingame", "wait:replay"] + (["capture"] if a.capture else [])
    out.with_suffix(".steps").write_text("\n".join(head + steps + tail) + "\n")
    n_pad = sum(1 for ln in lines if ln.startswith("pad "))
    print(f"{out.with_suffix('.steps')}: {len(steps) - 1} menu steps")
    print(f"{replay}: {n_pad} reports, {len(lines) - n_pad - 2} aim frames, {end:.1f} s")
    for first, last, cid in controlled(states, sync_ms):
        print(f"  controlling {cid}, not the player: {first - sync_s:.1f} to {last - sync_s:.1f} s")


if __name__ == "__main__":
    main()
