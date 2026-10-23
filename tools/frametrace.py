"""Summarise and compare frame traces (RECOMP_FCI_TRACE, src/fci_trace.c; re/notes/FRAME-RATE.md).

  python -B tools/frametrace.py <trace.csv> [more.csv ...]

For each trace: frames a second (host), the game's frame time (median, 99th percentile, worst), distance
moved and the speed while moving (camera position against the game's clock). The same scripted input at
different frame rates should give the same speeds and distances.
"""

import csv
import math
from pathlib import Path
import statistics
import sys

MOVING = 0.05  # metres a second below which the camera counts as still


def load(path):
    with open(path, newline="") as f:
        return [{k: float(v) if v else None for k, v in row.items()} for row in csv.DictReader(f)]


def percentile(values, p):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(p * len(ordered)))]


def summary(rows):
    host = (rows[-1]["host_ms"] - rows[0]["host_ms"]) / 1000.0
    frame = [r["frame_s"] for r in rows if r["frame_s"]]
    distance, speeds = 0.0, []
    placed = [r for r in rows if r["x"] is not None]  # the player's actor is known
    for a, b in zip(placed, placed[1:]):
        step = math.dist((a["x"], a["y"], a["z"]), (b["x"], b["y"], b["z"]))
        dt = (b["game_s"] - a["game_s"]) if b["game_s"] is not None and a["game_s"] is not None else None
        if step > 5.0:  # a teleport or a camera cut, not movement
            continue
        distance += step
        if dt and step / dt > MOVING:
            speeds.append(step / dt)
    return {
        "frames": len(rows),
        "fps": len(rows) / host if host > 0 else 0.0,
        "frame_ms_median": 1000 * statistics.median(frame) if frame else None,
        "frame_ms_p99": 1000 * percentile(frame, 0.99) if frame else None,
        "frame_ms_worst": 1000 * max(frame) if frame else None,
        "distance_m": distance,
        "speed_median": statistics.median(speeds) if speeds else 0.0,
    }


def main(paths):
    if not paths:
        raise SystemExit(__doc__)
    rows = [(Path(p).name, summary(load(p))) for p in paths]
    keys = list(rows[0][1])
    print("trace".ljust(28) + "".join(k.rjust(16) for k in keys))
    for name, s in rows:
        print(name[:27].ljust(28) + "".join(f"{s[k]:16.3f}" if s[k] is not None else " " * 15 + "-" for k in keys))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
