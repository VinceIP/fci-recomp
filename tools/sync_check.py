#!/usr/bin/env python3
"""Compare two RECOMP_FCI_TELEMETRY files (a host and a joiner in the same match) for player sync.

Reads two JSON-lines telemetry files (one line per sample: {"ms", "wall", "tick", "players":[...]}).
For each player id present in both files, the owner's track is the (wall, pos, yaw) sequence from the
file where it is "local": true; each remote sample (the other file, local false) is compared with the
owner's track linearly interpolated at (remote wall - lag). One lag per player is fit: the lag in
[0, 500] ms (5 ms steps) that minimises the median position residual. It passes when every player's
p95 residual is within tolerance, the fitted lag is small, and enough players and samples are present
(re/notes/q20 §11).

Usage:
    python tools/sync_check.py host.jsonl join.jsonl [--pos-tol 0.05] [--yaw-tol 1.0]
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from typing import Any

import numpy as np

# ang is [pitch, roll, yaw]; the yaw is the third component (it changes when a player turns, the
# other two stay near 0 — re/notes/q20 §11).
YAW_INDEX = 2
LAG_MIN_MS = 0
LAG_MAX_MS = 500
LAG_STEP_MS = 5
MIN_SAMPLES = 10
OWNER_MOVE_MPS = 0.2


def load(path: str) -> list[tuple[int, dict[int, dict[str, Any]]]]:
    """Read a telemetry file into [(wall_ms, players_by_id)] sorted by wall clock."""
    samples: list[tuple[int, dict[int, dict[str, Any]]]] = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            players: dict[int, dict[str, Any]] = {}
            for p in rec["players"]:
                players[int(p["id"])] = p
            samples.append((int(rec["wall"]), players))
    samples.sort(key=lambda s: s[0])
    return samples


def yaw_delta(a: float, b: float) -> float:
    """The absolute angular difference in degrees, in [0, 180]."""
    return abs((a - b + 180.0) % 360.0 - 180.0)


def player_track(
    samples: list[tuple[int, dict[int, dict[str, Any]]]], pid: int
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The (wall, pos, yaw) arrays for one player id, in wall order."""
    walls: list[float] = []
    pos: list[list[float]] = []
    yaw: list[float] = []
    for w, players in samples:
        p = players.get(pid)
        if p is None:
            continue
        walls.append(float(w))
        pos.append([float(x) for x in p["pos"]])
        yaw.append(float(p["ang"][YAW_INDEX]))
    return np.asarray(walls), np.asarray(pos), np.asarray(yaw)


# faster than any vehicle in a match: a jump this fast between two samples is a spawn or teleport
TELEPORT_SPEED = 30.0  # m/s


def _jump(walls: np.ndarray, pos: np.ndarray | None, i: int) -> bool:
    """True when the owner's segment i (walls[i] -> walls[i+1]) is a teleport."""
    if pos is None or i < 0 or i >= len(walls) - 1:
        return False
    dt = max(float(walls[i + 1] - walls[i]), 1.0) / 1000.0
    return float(np.linalg.norm(pos[i + 1] - pos[i])) / dt > TELEPORT_SPEED


def _interval(walls: np.ndarray, t: float, pos: np.ndarray | None = None) -> int | None:
    """The index i with walls[i] <= t <= walls[i+1], or None if t is outside the span or next to a
    teleport (interpolating across one compares two places; the copy may also update a sample late)."""
    if t < walls[0] or t > walls[-1] or len(walls) < 2:
        return None
    i = int(np.searchsorted(walls, t, side="right")) - 1
    i = max(0, min(i, len(walls) - 2))
    if any(_jump(walls, pos, j) for j in (i - 1, i, i + 1)):
        return None
    return i


def interp_pos(walls: np.ndarray, pos: np.ndarray, t: float) -> np.ndarray | None:
    """The owner's position linearly interpolated at time t, or None outside the span or at a teleport."""
    i = _interval(walls, t, pos)
    if i is None:
        return None
    dt = walls[i + 1] - walls[i]
    if dt <= 0:
        return pos[i]
    return pos[i] + (pos[i + 1] - pos[i]) * ((t - walls[i]) / dt)


def interp_yaw(walls: np.ndarray, yaw: np.ndarray, t: float) -> float | None:
    """The owner's yaw linearly interpolated at time t, or None outside the span."""
    i = _interval(walls, t)
    if i is None:
        return None
    dt = walls[i + 1] - walls[i]
    if dt <= 0:
        return float(yaw[i])
    step = (float(yaw[i + 1]) - float(yaw[i]) + 180.0) % 360.0 - 180.0  # the short way across +-180
    return float(yaw[i] + step * ((t - walls[i]) / dt))


def owner_speed_at(walls: np.ndarray, pos: np.ndarray, t: float) -> float:
    """The owner's speed (m/s) during the interval containing t, 0.0 outside the span."""
    i = _interval(walls, t)
    if i is None:
        return 0.0
    dt = walls[i + 1] - walls[i]
    if dt <= 0:
        return 0.0
    return float(np.linalg.norm(pos[i + 1] - pos[i])) / dt * 1000.0


MIN_MOVING_FOR_FIT = 5


def fit_lag(
    owalls: np.ndarray,
    opos: np.ndarray,
    oyaw: np.ndarray,
    rwalls: np.ndarray,
    rpos: np.ndarray,
    ryaw: np.ndarray,
) -> tuple[float, list[float], list[float], int, int, list[float]]:
    """The lag (ms) in [LAG_MIN, LAG_MAX] (LAG_STEP steps) minimising the median position residual over
    the samples where the owner moves (a still owner fits every lag, so those decide nothing; with fewer
    than MIN_MOVING_FOR_FIT moving samples, over all of them).

    Returns (lag_ms, pos_residuals, yaw_residuals, n_moving, n_still) for the fitted lag; the
    residuals are over the remote samples whose (wall - lag) falls inside the owner's track span.
    """
    # a moving owner's copy may be one sample interval off the single fitted lag (the lag jitters by
    # about one network update), so each residual is allowed the owner's speed times that interval
    jitter_s = float(np.median(np.diff(rwalls))) / 1000.0 if len(rwalls) > 1 else 0.0
    best: tuple[float, list[float], list[float], int, int, list[float]] = (
        float(LAG_MIN_MS),
        [],
        [],
        0,
        0,
        [],
    )
    best_med = math.inf
    for lag in range(LAG_MIN_MS, LAG_MAX_MS + 1, LAG_STEP_MS):
        pos_res: list[float] = []
        yaw_res: list[float] = []
        moving_res: list[float] = []
        excess: list[float] = []
        n_moving = 0
        n_still = 0
        for k in range(len(rwalls)):
            t = rwalls[k] - lag
            op = interp_pos(owalls, opos, t)
            if op is None:
                continue
            oy = interp_yaw(owalls, oyaw, t)
            if oy is None:
                continue
            d = float(np.linalg.norm(rpos[k] - op))
            speed = owner_speed_at(owalls, opos, t)
            excess.append(d - speed * jitter_s)
            if speed > OWNER_MOVE_MPS:
                n_moving += 1
                moving_res.append(d)
            else:
                n_still += 1
            pos_res.append(d)
            yaw_res.append(yaw_delta(ryaw[k], float(oy)))
        if not pos_res:
            continue
        med = float(np.median(moving_res if len(moving_res) >= MIN_MOVING_FOR_FIT else pos_res))
        if med < best_med:
            best_med = med
            best = (float(lag), pos_res, yaw_res, n_moving, n_still, excess)
    return best


def _pct(vals: list[float], q: float) -> float:
    return float(np.percentile(vals, q)) if vals else 0.0


def compare_player(
    a: list[tuple[int, dict[int, dict[str, Any]]]],
    b: list[tuple[int, dict[int, dict[str, Any]]]],
    local_a: set[int],
    local_b: set[int],
    pid: int,
) -> dict[str, Any] | None:
    """Compare one player: owner track from the file where it is local, remote from the other.

    Returns None if the player has no clear owner/remote split (local in both or neither file).
    """
    if pid in local_a and pid not in local_b:
        owner, remote, owner_file = a, b, "a"
    elif pid in local_b and pid not in local_a:
        owner, remote, owner_file = b, a, "b"
    else:
        return None
    owalls, opos, oyaw = player_track(owner, pid)
    rwalls, rpos, ryaw = player_track(remote, pid)
    base = {"id": pid, "owner_file": owner_file}
    if len(owalls) < 2 or len(rwalls) < 1:
        return {**base, "n": 0, "ok": False}
    lag, pos_res, yaw_res, n_moving, n_still, excess = fit_lag(owalls, opos, oyaw, rwalls, rpos, ryaw)
    if not pos_res:
        return {**base, "lag_ms": lag, "n": 0, "moving": 0, "still": 0, "ok": False}
    return {
        **base,
        "lag_ms": lag,
        "n": len(pos_res),
        "moving": n_moving,
        "still": n_still,
        "pos_max": round(max(pos_res), 6),
        "pos_excess_max": round(max(excess), 6),
        "pos_median": round(float(np.median(pos_res)), 6),
        "pos_p95": round(_pct(pos_res, 95), 6),
        "yaw_max": round(max(yaw_res), 6),
        "yaw_median": round(float(np.median(yaw_res)), 6),
        "yaw_p95": round(_pct(yaw_res, 95), 6),
    }


def player_ok(r: dict[str, Any], pos_tol: float, yaw_tol: float, max_lag: float) -> bool:
    """The per-player pass: enough samples, small lag, and the residuals within tolerance."""
    if r.get("n", 0) < MIN_SAMPLES:
        return False
    if r.get("lag_ms", 1e9) > max_lag:
        return False
    if r.get("pos_p95", 1e9) > pos_tol:
        return False
    if r.get("pos_excess_max", 1e9) > 4 * pos_tol:  # the worst sample, less the moving allowance
        return False
    if r.get("yaw_p95", 1e9) > yaw_tol:
        return False
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Compare two telemetry files for player sync")
    ap.add_argument("host", help="one instance's RECOMP_FCI_TELEMETRY file")
    ap.add_argument("join", help="the other instance's RECOMP_FCI_TELEMETRY file")
    ap.add_argument("--pos-tol", type=float, default=0.05, help="p95 position residual (m) for a pass")
    ap.add_argument("--yaw-tol", type=float, default=1.0, help="p95 yaw residual (deg) for a pass")
    ap.add_argument("--max-lag", type=float, default=300.0, help="max fitted lag (ms) for a pass")
    ap.add_argument("--min-players", type=int, default=2, help="players that must pass (default 2)")
    args = ap.parse_args(argv)

    try:
        a = load(args.host)
        b = load(args.join)
    except (OSError, json.JSONDecodeError) as e:
        print(f"sync_check: {e}", file=sys.stderr)
        return 2
    if not a or not b:
        print("sync_check: no samples", file=sys.stderr)
        return 2

    local_a = {pid for _, ps in a for pid, p in ps.items() if p.get("local")}
    local_b = {pid for _, ps in b for pid, p in ps.items() if p.get("local")}
    ids_a = {pid for _, ps in a for pid in ps}
    ids_b = {pid for _, ps in b for pid in ps}

    players: list[dict[str, Any]] = []
    for pid in sorted(ids_a & ids_b):
        r = compare_player(a, b, local_a, local_b, pid)
        if r is not None:
            r["ok"] = player_ok(r, args.pos_tol, args.yaw_tol, args.max_lag)
            players.append(r)

    n_ok = sum(1 for r in players if r["ok"])
    # nothing compared is a failure, not a pass: a run where the instances never met proves nothing
    ok = bool(players) and n_ok >= args.min_players

    report = {
        "players": players,
        "n_ok": n_ok,
        "min_players": args.min_players,
        "pos_tol": args.pos_tol,
        "yaw_tol": args.yaw_tol,
        "max_lag": args.max_lag,
        "ok": ok,
    }
    print(json.dumps(report, indent=2))
    detail = (
        "; ".join(
            f"id {r['id']}: lag {r.get('lag_ms', float('nan')):.0f} ms, n {r.get('n', 0)}, "
            f"p95 pos {r.get('pos_p95', float('nan')):.4f} m, p95 yaw {r.get('yaw_p95', float('nan')):.2f} deg"
            for r in players
        )
        or "no shared players"
    )
    print(
        f"sync_check: {'PASS' if ok else 'FAIL'} ({n_ok}/{len(players)} players ok; {detail})",
        file=sys.stderr,
    )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
