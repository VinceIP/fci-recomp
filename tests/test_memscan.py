#!/usr/bin/env python3
"""tests for tools/memscan.py against a synthetic 32-bit dump.

Builds a small in-memory image with known values at known guest VAs, writes it
as a dump file (file offset = VA - 0x10000), and checks each subcommand.
Run:  python3 -m pytest tests/test_memscan.py -q
"""

import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "memscan.py")
VA_BASE = 0x00010000
SIZE = 0x20000  # 128 KB, covers VA [0x10000, 0x30000)


def _build_dump(path):
    buf = bytearray(SIZE)

    def put32(va, v):
        off = va - VA_BASE
        buf[off : off + 4] = struct.pack("<i", v)

    def putu32(va, v):
        off = va - VA_BASE
        buf[off : off + 4] = struct.pack("<I", v)

    def putf32(va, v):
        off = va - VA_BASE
        buf[off : off + 4] = struct.pack("<f", v)

    put32(0x10050, 100)  # health candidate #1
    put32(0x10100, 100)  # health candidate #2
    put32(0x10200, 250)  # a different value
    putf32(0x10300, 3.14)  # a float
    for i, v in enumerate([7, 8, 9, 10]):  # a std::vector<int>-like block
        put32(0x10400 + i * 4, v)
    putu32(0x10500, 0x20000)  # a pointer into the mapped range
    putu32(0x10504, 0x99)  # a non-pointer small value
    with open(path, "wb") as f:
        f.write(buf)


@pytest.fixture
def dump(tmp_path):
    p = str(tmp_path / "dump.bin")
    _build_dump(p)
    return p


def _run(*args):
    r = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    assert r.returncode == 0, f"memscan {' '.join(args)} failed: {r.stderr}"
    out = [ln for ln in r.stdout.splitlines() if ln and not ln.startswith("#")]
    return [int(x, 16) for x in out], r


def _vaf(tmp_path, name, vas):
    p = str(tmp_path / name)
    with open(p, "w") as f:
        for v in vas:
            f.write(f"0x{v:08X}\n")
    return p


def test_scan(dump, tmp_path):
    vas, _ = _run("scan", dump, "i32", "100")
    assert 0x10050 in vas and 0x10100 in vas
    assert 0x10200 not in vas
    vas, _ = _run("scan", dump, "i32", "250")
    assert vas.count(0x10200) == 1
    vas, _ = _run("scan", dump, "f32", "3.14")
    assert 0x10300 in vas


def test_scan_unaligned(dump):
    # a u32 at an odd byte offset must still be found (sub-alignment coverage)
    with open(dump, "r+b") as f:
        f.seek(0x10601 - VA_BASE)
        f.write(struct.pack("<I", 0x11223344))
    vas, _ = _run("scan", dump, "u32", "0x11223344")
    assert 0x10601 in vas


def test_filter(dump, tmp_path):
    vas, _ = _run("scan", dump, "i32", "100")
    vaf = _vaf(tmp_path, "cands.txt", vas)
    assert _run("filter", dump, vaf, "i32", "250")[0] == []  # none of them is 250
    assert _run("filter", dump, vaf, "i32", "100")[0] == vas  # all kept


def test_vec(dump, tmp_path):
    vaf = _vaf(tmp_path, "vec.txt", [0x10400])
    r = subprocess.run([sys.executable, TOOL, "vec", dump, vaf, "i32", "4"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    line = [ln for ln in r.stdout.splitlines() if ln.startswith("0x00010400")]
    assert line and line[0].endswith("7 8 9 10")


def test_ptr(dump, tmp_path):
    vaf = _vaf(tmp_path, "ptrs.txt", [0x10500, 0x10504])
    vas, _ = _run("ptr", dump, vaf, "0x10000", "0x30000")
    assert 0x10500 in vas and 0x10504 not in vas


def test_delta(tmp_path):
    a = str(tmp_path / "a.bin")
    b = str(tmp_path / "b.bin")
    _build_dump(a)
    _build_dump(b)
    with open(b, "r+b") as f:
        f.seek(0x10050 - VA_BASE)
        f.write(struct.pack("<i", 150))  # was 100 -> delta +50 (the "clock")
        f.seek(0x10200 - VA_BASE)
        f.write(struct.pack("<i", 225))  # was 250 -> delta -25 ("took damage")
    r = subprocess.run([sys.executable, TOOL, "delta", a, b, "i32", "50"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert 0x10050 in [int(x, 16) for x in r.stdout.split()]
    r = subprocess.run([sys.executable, TOOL, "delta", a, b, "i32", "-25"], capture_output=True, text=True)
    assert 0x10200 in [int(x, 16) for x in r.stdout.split()]


def test_peek(dump):
    r = subprocess.run([sys.executable, TOOL, "peek", dump, "0x10400", "i32", "4"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    vals = [int(ln.split(":")[1]) for ln in r.stdout.splitlines()]
    assert vals == [7, 8, 9, 10]
