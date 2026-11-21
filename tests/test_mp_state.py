"""Tests for tools/mp_state.py on synthetic dumps.

A synthetic dump is a zero-filled byte string of the dump's VA range
[0x10000, 0x4000000); file offset = VA - 0x10000. We plant a known clock and a
known CEntity (vtable, id, centre, angles) plus its linked CPlayer, then assert
mp_state walks them and that a two-dump sync_check matches by id.
"""

from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import mp_state  # noqa: E402

BASE = 0x10000
# Must cover the vtable addresses 0x004B04E8 / 0x004AA7E0 and the objects at
# 0x00200000 / 0x00300000.
SIZE = 0x500000  # covers VA 0x10000..0x510000


def _plant(buf: bytearray, va: int, values, fmt) -> None:
    off = va - BASE
    assert 0 <= off + 4 * len(values) <= len(buf), f"va {va:#x} out of range"
    buf[off : off + 4 * len(values)] = struct.pack("<" + fmt, *values)


# The real vtable values (used as the first dword of each object) and a code
# address for the vtable slot targets.
CODE_TARGET = 0x00020100
ENTITY_VT = mp_state.ENTITY_VTABLE  # 0x004B04E8
PLAYER_VT = mp_state.PLAYER_VTABLE  # 0x004AA7E0


def _plant_vtable(buf: bytearray, vt: int) -> None:
    # Six slots, each pointing into the code section so _is_valid_vtable passes.
    _plant(buf, vt, (CODE_TARGET,) * 6, "6I")


def _plant_player(buf: bytearray, ent_va: int, foot) -> int:
    pva = 0x00300000
    _plant_vtable(buf, PLAYER_VT)
    _plant(buf, pva, (PLAYER_VT,), "I")  # vtable
    _plant(buf, pva + mp_state.OFF_PLAYER_ENTITY, (ent_va,), "I")  # -> CEntity
    _plant(buf, pva + mp_state.OFF_PLAYER_FOOT, foot, "fff")  # foot position
    return pva


def _plant_entity(buf: bytearray, eid: int, center, angles) -> int:
    eva = 0x00200000
    _plant_vtable(buf, ENTITY_VT)
    pva = _plant_player(buf, eva, center)  # CPlayer foot == centre here
    _plant(buf, eva, (ENTITY_VT,), "I")  # vtable
    _plant(buf, eva + mp_state.OFF_ENTITY_PLAYER, (pva,), "I")  # -> CPlayer
    _plant(buf, eva + mp_state.OFF_ENTITY_ID, (eid,), "I")  # m_nID
    _plant(buf, eva + mp_state.OFF_ENTITY_CENTER, center, "fff")  # m_center
    _plant(buf, eva + mp_state.OFF_ENTITY_ANGLES, angles, "fff")  # m_angles
    return eva


def _synthetic_dump() -> bytes:
    buf = bytearray(SIZE)
    _plant(buf, mp_state.CLOCK_VA, (1234,), "i")
    _plant_entity(buf, 5, (100.0, 200.0, 50.0), (0.0, 0.0, 90.0))
    return bytes(buf)


def test_find_clock_on_synthetic():
    state = mp_state.build_state(_synthetic_dump())
    clocks = [s for s in state if s["type"] == "clock"]
    assert len(clocks) == 1
    assert clocks[0]["address"] == mp_state.CLOCK_VA
    assert clocks[0]["value"] == 1234


def test_find_entity_on_synthetic():
    state = mp_state.build_state(_synthetic_dump())
    ents = [s for s in state if s["type"] == "entity"]
    assert len(ents) == 1, f"expected 1 entity, got {ents}"
    e = ents[0]
    assert e["id"] == 5
    assert e["address"] == 0x00200000
    assert e["center"] == [100.0, 200.0, 50.0]
    assert e["angles"] == [0.0, 0.0, 90.0]
    assert e["player_linked"] is True
    assert e["foot"] == [100.0, 200.0, 50.0]


def test_zero_dump_has_clock_only():
    state = mp_state.build_state(bytes(SIZE))
    clocks = [s for s in state if s["type"] == "clock"]
    ents = [s for s in state if s["type"] == "entity"]
    assert len(clocks) == 1
    assert clocks[0]["value"] == 0
    assert ents == []


def test_entity_out_of_bounds_rejected():
    # A centre with z below Z_MIN must not be reported.
    buf = bytearray(SIZE)
    _plant_entity(buf, 5, (100.0, 200.0, 0.5), (0.0, 0.0, 90.0))  # z < Z_MIN
    ents = mp_state.find_entities(np.frombuffer(bytes(buf), dtype=np.uint8))
    assert ents == []


def test_sync_check_matches_by_id():
    a = bytearray(SIZE)
    _plant(a, mp_state.CLOCK_VA, (100,), "i")
    _plant_entity(a, 5, (100.0, 200.0, 50.0), (0.0, 0.0, 90.0))
    b = bytearray(SIZE)
    _plant(b, mp_state.CLOCK_VA, (101,), "i")
    # Same id, position within the 1/512 m quantum, same yaw.
    _plant_entity(b, 5, (100.001, 200.001, 50.001), (0.0, 0.0, 90.0))
    rep = mp_state.sync_check(
        np.frombuffer(bytes(a), dtype=np.uint8),
        np.frombuffer(bytes(b), dtype=np.uint8),
        "a",
        "b",
    )
    assert rep["common_ids"] == [5]
    assert len(rep["players"]) == 1
    assert rep["players"][0]["in_sync"] is True
    assert all(abs(d) < 0.5 for d in rep["players"][0]["delta"])


def test_sync_check_flags_desync():
    a = bytearray(SIZE)
    _plant_entity(a, 5, (100.0, 200.0, 50.0), (0.0, 0.0, 90.0))
    b = bytearray(SIZE)
    _plant_entity(b, 5, (150.0, 200.0, 50.0), (0.0, 0.0, 90.0))  # 50 m off
    rep = mp_state.sync_check(
        np.frombuffer(bytes(a), dtype=np.uint8),
        np.frombuffer(bytes(b), dtype=np.uint8),
        "a",
        "b",
    )
    assert rep["players"][0]["in_sync"] is False
