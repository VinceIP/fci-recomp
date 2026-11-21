"""Longer weapon ranges: each gun's distance (full damage up to here) and distance_max (no hit beyond), in
metres, in the game's lua.dat (re/notes/WEAPONS.md).

  python -B tools/weapon_range.py [--install <folder>]... [--list] [--restore] [--dry-run]

  --install   a game folder (FarCryInstincts, FarCryEvolution); by default every game set up here
  --list      show the ranges in the game now
  --restore   put the original lua.dat back

The Xbox values are 20 to 70 m for most guns; RANGES has the new ones, the same for both games. Each value
is an integer load in a weapon script, changed in place: lua.dat keeps its size and lua.fat stays valid.
The first run keeps the original as lua.dat.orig and every run starts from it, so runs don't stack.
Multiplayer ranges, melee weapons, launchers and darts stay as they are. Close the game first.
"""

import argparse
from pathlib import Path
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import luadat  # noqa: E402
import titles  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
FIELDS = ("distance", "distance_max")
# weapon script: (distance, distance_max)
RANGES = {
    "Berretta": (50, 150),  # Beretta M9
    "BerrettaNoSup": (50, 150),
    "Glock": (50, 150),
    "DE": (60, 180),  # Desert Eagle
    "MP5": (100, 250),
    "P90": (100, 250),
    "M16": (250, 600),
    "M4": (250, 600),
    "FNM249": (250, 600),
    "Browning": (300, 800),
    "HKSL8": (400, 1000),
    "BarrettM82A1": (600, 1500),
    "M1S90": (25, 60),  # shotgun
}


def ranges(data, fat):
    """{weapon: [(field, value, offset in lua.dat)]} for the weapons in RANGES"""
    found = {}
    for offset, size, _ in luadat.index(fat):
        chunk = bytes(data[offset : offset + size])
        if not chunk.startswith(b"\x1bLua"):
            continue
        for f in luadat.functions(luadat.parse(chunk)):
            weapon = luadat.name(f)
            if weapon in RANGES:
                for field, value, at in luadat.int_fields(f, FIELDS):
                    if value > 0:  # the M16's table of zeros is its grenade launcher
                        found.setdefault(weapon, []).append((field, value, offset + at))
    return found


def apply(game, dry_run=False):
    """set RANGES in game/lua.dat, from lua.dat.orig when there is one; the changes as (weapon, field, old, new)"""
    dat, orig = game / "lua.dat", game / "lua.dat.orig"
    data = bytearray((orig if orig.exists() else dat).read_bytes())
    found = ranges(data, (game / "lua.fat").read_bytes())
    if not found:
        raise SystemExit(f"error: {dat} has no weapon ranges this tool knows; nothing was changed")
    changes = []
    for weapon, stores in sorted(found.items()):
        want = dict(zip(FIELDS, RANGES[weapon]))
        for field, value, at in stores:
            struct.pack_into("<I", data, at, luadat.load_int(struct.unpack_from("<I", data, at)[0], want[field]))
            changes.append((weapon, field, value, want[field]))
    if not dry_run:
        if not orig.exists():
            shutil.copy2(dat, orig)
        dat.write_bytes(data)
    return changes


def restore(game):
    orig = game / "lua.dat.orig"
    if not orig.exists():
        return False
    shutil.copy2(orig, game / "lua.dat")
    return True


def games(installs):
    """the game folders (with lua.dat) to change: those given, or every one set up in this folder"""
    folders = [Path(i) for i in installs] or [REPO / t.install for t in titles.TITLES.values()]
    found = [f / "game" for f in folders if (f / "game" / "lua.dat").is_file()]
    if installs and len(found) != len(folders):
        raise SystemExit("error: --install must name a game folder set up by BuildGame.cmd (one with game/lua.dat)")
    if not found:
        raise SystemExit("error: no game set up here yet (BuildGame.cmd)")
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    for game in games(a.install):
        print(game.parent.name)
        try:
            if a.list:
                for weapon, stores in sorted(
                    ranges((game / "lua.dat").read_bytes(), (game / "lua.fat").read_bytes()).items()
                ):
                    print(f"  {weapon:14s} " + ", ".join(f"{field} {value} m" for field, value, _ in stores))
            elif a.restore:
                print("  the original ranges are back" if restore(game) else "  unchanged: no lua.dat.orig")
            else:
                for weapon, field, old, new in apply(game, a.dry_run):
                    print(f"  {weapon:14s} {field:12s} {old:4d} m -> {new} m")
                print("  (dry run: nothing written)" if a.dry_run else "  done; --restore puts the original back")
        except PermissionError:
            raise SystemExit(f"error: {game / 'lua.dat'} cannot be written. Close the game and run this again.")
        except ValueError as e:
            raise SystemExit(f"error: {game / 'lua.dat'}: {e}; nothing was changed")


if __name__ == "__main__":
    main()
