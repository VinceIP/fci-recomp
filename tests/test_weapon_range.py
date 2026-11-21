"""tools/weapon_range.py and tools/luadat.py on synthetic weapon scripts (no game data)"""

from pathlib import Path
import struct
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import luadat  # noqa: E402
import weapon_range  # noqa: E402

HEADER = b"\x1bLua" + bytes([0x41, 1, 4, 4, 4, 6, 8, 8, 10, 4]) + struct.pack("<f", 314159265.0)


def string(s):
    return struct.pack("<i", len(s) + 1) + s.encode() + b"\0"


def script(weapon, fields):
    """a weapon script as the game compiles it: R0.name = weapon, then R0[field] = value for each field"""
    k = [weapon, "name", *fields]
    code = [(1 << 24) | (0 << 6) | 1, (1 << 24) | ((250 + 1) << 6) | 8]  # LOADK R1 K0; R0[K1] = R1
    for i, value in enumerate(fields.values()):
        code += [(1 << 24) | ((value + 0x1FFFF) << 6) | 2, (1 << 24) | ((250 + 2 + i) << 6) | 8]
    body = string("=(none)") + struct.pack("<5i", 0, 0, 0, 0, 8) + struct.pack("<ii", 0, 0)
    body += struct.pack("<i", len(k)) + b"".join(b"\x03" + string(s) for s in k)
    return HEADER + body + struct.pack("<ii", 0, len(code)) + struct.pack(f"<{len(code)}I", *code)


def game(tmp_path, scripts):
    """a game folder with lua.dat and lua.fat holding scripts (bytes), an other file first"""
    files = [b"not a script", *scripts]
    dat, entries = b"", []
    for i, f in enumerate(files):
        entries.append((len(dat), len(f), 0x1000 + i))
        dat += f
    folder = tmp_path / "FarCryInstincts" / "game"
    folder.mkdir(parents=True)
    (folder / "lua.dat").write_bytes(dat)
    (folder / "lua.fat").write_bytes(
        b"DAT2" + struct.pack("<I", len(entries)) + b"".join(struct.pack("<III", *e) for e in entries)
    )
    return folder


def values(folder):
    data, fat = (folder / "lua.dat").read_bytes(), (folder / "lua.fat").read_bytes()
    return {w: {f: v for f, v, _ in stores} for w, stores in weapon_range.ranges(data, fat).items()}


def test_parse_reads_constants_and_integer_fields():
    f = luadat.parse(script("Berretta", {"distance": 25, "distance_max": 30}))
    assert luadat.name(f) == "Berretta"
    assert [(field, v) for field, v, _ in luadat.int_fields(f, {"distance", "distance_max"})] == [
        ("distance", 25),
        ("distance_max", 30),
    ]


def test_parse_rejects_a_chunk_it_does_not_understand():
    with pytest.raises(ValueError):
        luadat.parse(script("Berretta", {"distance": 25})[:-3])


def test_apply_sets_the_ranges_and_keeps_the_original(tmp_path):
    folder = game(
        tmp_path,
        [
            script("Berretta", {"distance": 25, "distance_max": 30, "multiplayer_distance": 20}),
            script("M16", {"distance": 0, "distance_max": 0}),  # the grenade launcher's table
            script("M4", {"distance": 35, "distance_max": 70}),
            script("Machete", {"distance": 3, "distance_max": 4}),  # not in RANGES
        ],
    )
    before = (folder / "lua.dat").read_bytes()
    changes = weapon_range.apply(folder)
    after = (folder / "lua.dat").read_bytes()
    assert (folder / "lua.dat.orig").read_bytes() == before
    assert len(after) == len(before)
    assert sorted(changes) == [
        ("Berretta", "distance", 25, 50),
        ("Berretta", "distance_max", 30, 150),
        ("M4", "distance", 35, 250),
        ("M4", "distance_max", 70, 600),
    ]
    assert values(folder) == {
        "Berretta": {"distance": 50, "distance_max": 150},
        "M4": {"distance": 250, "distance_max": 600},
    }
    # the multiplayer range, the zeros and the other weapon are as they were: four words changed, nothing else
    changed = [i for i in range(len(before)) if before[i] != after[i]]
    data = luadat.index((folder / "lua.fat").read_bytes())
    loads = set()
    for offset, size, _ in data[1:]:
        for _field, _, at in luadat.int_fields(
            luadat.parse(after[offset : offset + size]), {"distance", "distance_max"}
        ):
            loads.update(range(offset + at, offset + at + 4))
    assert changed and set(changed) <= loads
    assert (
        luadat.int_fields(luadat.parse(after[data[1][0] : data[1][0] + data[1][1]]), {"multiplayer_distance"})[0][1]
        == 20
    )


def test_runs_do_not_stack_and_restore_puts_the_original_back(tmp_path):
    folder = game(tmp_path, [script("Glock", {"distance": 20, "distance_max": 30})])
    original = (folder / "lua.dat").read_bytes()
    weapon_range.apply(folder)
    first = (folder / "lua.dat").read_bytes()
    assert weapon_range.apply(folder) == [("Glock", "distance", 20, 50), ("Glock", "distance_max", 30, 150)]
    assert (folder / "lua.dat").read_bytes() == first
    assert weapon_range.restore(folder)
    assert (folder / "lua.dat").read_bytes() == original


def test_dry_run_writes_nothing(tmp_path):
    folder = game(tmp_path, [script("P90", {"distance": 30, "distance_max": 40})])
    original = (folder / "lua.dat").read_bytes()
    assert weapon_range.apply(folder, dry_run=True)
    assert (folder / "lua.dat").read_bytes() == original
    assert not (folder / "lua.dat.orig").exists()


def test_an_unknown_script_changes_nothing(tmp_path):
    folder = game(tmp_path, [script("MP5", {"distance": 30, "distance_max": 40})[:-3]])
    original = (folder / "lua.dat").read_bytes()
    with pytest.raises(ValueError):
        weapon_range.apply(folder)
    assert (folder / "lua.dat").read_bytes() == original
    assert not (folder / "lua.dat.orig").exists()
