#!/usr/bin/env python3
"""Extract multiplayer player-entity state from an fci-recomp memory dump.

Dump format: a contiguous image of VA range [0x10000, 0x4000000);
file offset = VA - 0x10000. 32-bit little-endian.

Two modes (see research/qwen/notes/q20-sync-telemetry.md):

1. Heuristic mode (default): find the global clock at 0x003CC868 and float
   triples that look like world positions with a small integer nearby.

2. Entity mode (--entities): walk the CEntity objects directly. A CEntity is
   any object whose vtable (first dword) is 0x004B04E8. The confirmed layout
   (Evolution, from the Q20 sync dumps) is:
     +0x000  vtable            (0x004B04E8)
     +0x004  CPlayer*          (back-reference; the CPlayer's +0x23C points back)
     +0x1E0  m_nID             (entity id, e.g. host player = 2, joiner = 3)
     +0x214  m_center          (Vec3; bounding-box centre for the owner,
                                quantised network position for a remote player)
     +0x220  m_angles          (Vec3, degrees)
   The CPlayer (vtable 0x004AA7E0) keeps the foot/ground position at +0x34C.

3. --sync-check A.bin B.bin: walk the entities in two dumps (e.g. host-s3 and
   join-s3), match players by entity id, and report each player's position and
   angles as seen on each instance plus the deltas. This is the core
   multiplayer-sync telemetry: the same id must appear on both instances with
   matching (up to 1/512 m quantisation) positions.

Output: one JSON object per line on stdout.
"""

from __future__ import annotations

import argparse
import json
import sys

import numpy as np

BASE = 0x10000
CLOCK_VA = 0x003CC868

# CEntity / CPlayer vtables (Evolution, from the Q20 sync dumps).
ENTITY_VTABLE = 0x004B04E8
PLAYER_VTABLE = 0x004AA7E0

# CEntity member offsets (Evolution).
OFF_ENTITY_VTABLE = 0x000
OFF_ENTITY_PLAYER = 0x004
OFF_ENTITY_ID = 0x1E0
OFF_ENTITY_CENTER = 0x214
OFF_ENTITY_ANGLES = 0x220
# CPlayer member offsets (Evolution).
OFF_PLAYER_ENTITY = 0x23C  # CPlayer -> CEntity (mutual with OFF_ENTITY_PLAYER)
OFF_PLAYER_FOOT = 0x34C  # CPlayer foot / ground position (Vec3)

# World-position plausibility bounds for FCI outdoor maps (meters).
X_MIN, X_MAX = 50.0, 500.0
Y_MIN, Y_MAX = 50.0, 500.0
Z_MIN, Z_MAX = 1.0, 100.0
# A plausible entity id.
ID_MIN, ID_MAX = 1, 256
# Code-section range: a vtable is valid only if its slots point here.
CODE_LO, CODE_HI = 0x10000, 0x400000


def load_dump(path: str) -> np.ndarray:
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 0x1000:
        raise ValueError(f"dump too small: {len(data)} bytes")
    return np.frombuffer(data, dtype=np.uint8)


def read_u32(buf: np.ndarray, va: int) -> int | None:
    off = va - BASE
    if off < 0 or off + 4 > len(buf):
        return None
    return int(np.frombuffer(buf[off : off + 4], dtype=np.uint32)[0])


def read_f32(buf: np.ndarray, va: int) -> float | None:
    off = va - BASE
    if off < 0 or off + 4 > len(buf):
        return None
    return float(np.frombuffer(buf[off : off + 4], dtype=np.float32)[0])


def read_f32x3(buf: np.ndarray, va: int) -> list[float] | None:
    off = va - BASE
    if off < 0 or off + 12 > len(buf):
        return None
    return [float(x) for x in np.frombuffer(buf[off : off + 12], dtype=np.float32)]


def _is_valid_vtable(buf: np.ndarray, vt: int) -> bool:
    """A vtable is plausible if its first six slots point into the code section."""
    if not (CODE_LO <= vt < 0x1100000):
        return False
    off = vt - BASE
    if off < 0 or off + 24 > len(buf):
        return False
    slots = np.frombuffer(buf[off : off + 24], dtype=np.uint32)
    return bool(np.all((slots >= CODE_LO) & (slots < CODE_HI)))


def find_clock(buf: np.ndarray) -> dict | None:
    val = read_u32(buf, CLOCK_VA)
    if val is None:
        return None
    return {"type": "clock", "address": CLOCK_VA, "value": int(val)}


def _in_world(p: list[float] | None) -> bool:
    if p is None:
        return False
    return (
        np.isfinite(p[0])
        and np.isfinite(p[1])
        and np.isfinite(p[2])
        and X_MIN <= p[0] <= X_MAX
        and Y_MIN <= p[1] <= Y_MAX
        and Z_MIN <= p[2] <= Z_MAX
    )


def find_entities(buf: np.ndarray) -> list[dict]:
    """Walk every CEntity (vtable ENTITY_VTABLE) and read its player fields."""
    d = np.frombuffer(buf, dtype=np.uint32)
    out: list[dict] = []
    for i in np.where(d == ENTITY_VTABLE)[0]:
        va = BASE + int(i) * 4
        if va < 0x100000:  # skip the low static/data region
            continue
        eid = read_u32(buf, va + OFF_ENTITY_ID)
        if eid is None or not (ID_MIN <= eid <= ID_MAX):
            continue
        center = read_f32x3(buf, va + OFF_ENTITY_CENTER)
        angles = read_f32x3(buf, va + OFF_ENTITY_ANGLES)
        if not _in_world(center):
            continue
        player = read_u32(buf, va + OFF_ENTITY_PLAYER)
        # Validate the CPlayer<->CEntity mutual reference when present.
        player_ok = False
        foot = None
        if player is not None and _is_valid_vtable(buf, read_u32(buf, player) or 0):
            player_ok = read_u32(buf, player + OFF_PLAYER_ENTITY) == va
            foot = read_f32x3(buf, player + OFF_PLAYER_FOOT)
        out.append(
            {
                "type": "entity",
                "address": va,
                "id": int(eid),
                "center": [round(x, 4) for x in center],
                "angles": [round(x, 3) for x in angles] if angles else None,
                "player": player,
                "player_linked": bool(player_ok),
                "foot": [round(x, 4) for x in foot] if foot else None,
            }
        )
    out.sort(key=lambda e: e["id"])
    return out


def _effective_pos(e: dict) -> list[float] | None:
    """The ground (foot) position, the quantity that is comparable across
    instances. The owner's CEntity m_center is the bounding-box centre (foot +
    ~1.44 m Z) while a remote player's m_center equals its foot, so the foot
    (CPlayer+0x34C) is the like-for-like value on both sides."""
    return e.get("foot") or e.get("center")


def sync_check(buf_a: np.ndarray, buf_b: np.ndarray, name_a: str, name_b: str) -> dict:
    """Match players by entity id across two dumps and report the deltas.

    The comparable position is the foot (see _effective_pos): the owner keeps
    its exact foot position and the remote keeps the 1/512 m-quantised foot,
    so a synced pair differs by at most the quantum plus interpolation.
    """
    ea = {e["id"]: e for e in find_entities(buf_a)}
    eb = {e["id"]: e for e in find_entities(buf_b)}
    common = sorted(set(ea) & set(eb))
    players = []
    for eid in common:
        a, b = ea[eid], eb[eid]
        pa, pb = _effective_pos(a), _effective_pos(b)
        if pa is None or pb is None:
            continue
        dpos = [pa[k] - pb[k] for k in range(3)]
        da = a["angles"] or [0, 0, 0]
        db = b["angles"] or [0, 0, 0]
        players.append(
            {
                "id": eid,
                name_a: {"addr": a["address"], "pos": pa, "angles": a["angles"]},
                name_b: {"addr": b["address"], "pos": pb, "angles": b["angles"]},
                "delta": [round(x, 4) for x in dpos],
                "delta_yaw": round(da[2] - db[2], 3),
                # A synced pair matches to within the 1/512 m quantum plus a
                # little interpolation slack; flag larger gaps.
                "in_sync": all(abs(x) < 0.5 for x in dpos) and abs(da[2] - db[2]) < 2.0,
            }
        )
    return {
        "type": "sync_check",
        name_a: {"ids": sorted(ea)},
        name_b: {"ids": sorted(eb)},
        "common_ids": common,
        "players": players,
    }


def build_state(buf: np.ndarray) -> list[dict]:
    out: list[dict] = []
    clock = find_clock(buf)
    if clock:
        out.append(clock)
    out.extend(find_entities(buf))
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dump", help="path to a memory dump (.bin)")
    p.add_argument(
        "--sync-check",
        metavar="OTHER.bin",
        help="compare this dump against OTHER.bin by entity id and print a sync report",
    )
    p.add_argument(
        "--name-a",
        default="a",
        help="label for the first dump in a --sync-check report",
    )
    p.add_argument(
        "--name-b",
        default="b",
        help="label for the second dump in a --sync-check report",
    )
    args = p.parse_args(argv)
    try:
        buf = load_dump(args.dump)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.sync_check:
        try:
            other = load_dump(args.sync_check)
        except (OSError, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        print(json.dumps(sync_check(buf, other, args.name_a, args.name_b), indent=2))
        return 0

    for s in build_state(buf):
        print(json.dumps(s))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
