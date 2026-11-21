"""Synthetic tests for tools/xbe_peek.py.

Build a tiny in-memory XBE (XBEH header + section table + .text/.rdata/.data),
write it to a temp file, and verify the read-only VA mapping, u32/vtable/str
reads, and the virtual-only-tail guard.  No real disc data is used.
"""

import struct
import sys

import pytest

sys.path.insert(0, "tools")
import xbe_peek  # noqa: E402


def build_xbe(tmp_path):
    """Construct a minimal valid XBE with known contents.

    Layout (file offsets chosen so nothing overlaps):
      base = 0x00400000
      section table: file 0x180 .. 0x248  (3 x 56 bytes)
      .text  va=0x00401000 vsize=0x100 raw=0x300 rsize=0x100
      .rdata va=0x00402000 vsize=0x100 raw=0x400 rsize=0x100
      .data  va=0x00403000 vsize=0x200 raw=0x500 rsize=0x40  (virtual tail!)
      name strings: file 0x900+

    .rdata contents (at file 0x400, va 0x402000):
      0x00: vtable: 4 code u32s + one data u32
      0x14: ascii string "B_Quit"
      0x20: utf-16le string "P_Start"
    """
    base = 0x00400000
    file_size = 0x1000
    d = bytearray(file_size)

    # header
    d[0:4] = b"XBEH"
    struct.pack_into("<I", d, 0x104, base)  # base address
    nsec = 3
    struct.pack_into("<I", d, 0x11C, nsec)  # section count
    # section table will live at file offset 0x180 (shva = base + 0x180)
    shva = base + 0x180
    struct.pack_into("<I", d, 0x120, shva)  # section table VA

    # section name strings (stored at file 0x900+)
    names = {".text": 0x900, ".rdata": 0x910, ".data": 0x920}
    for nm, off in names.items():
        d[off : off + len(nm) + 1] = nm.encode() + b"\x00"

    def sec(flags, va, vsize, raw, rsize, name):
        # Real XBE section header is 56 bytes: 6 u32s + 8 reserved + 20 hash
        # + 4 reserved.  We only fill the first 6; pad the rest with zeros.
        hdr = struct.pack("<IIIIII", flags, va, vsize, raw, rsize, base + names[name])
        return hdr + b"\x00" * (56 - len(hdr))

    sects = [
        sec(0x1, 0x00401000, 0x100, 0x300, 0x100, ".text"),
        sec(0x2, 0x00402000, 0x100, 0x400, 0x100, ".rdata"),
        sec(0x3, 0x00403000, 0x200, 0x500, 0x40, ".data"),  # vsize>rsize
    ]
    for i, s in enumerate(sects):
        assert len(s) == 56
        d[0x180 + i * 56 : 0x180 + (i + 1) * 56] = s

    # .text content (file 0x300): a fake code marker
    struct.pack_into("<I", d, 0x300, 0x90909090)

    # .rdata content (file 0x400, va 0x402000)
    # vtable at va 0x402000 (file 0x400): 5 slots
    vt = [0x00401000, 0x00401010, 0x00401020, 0x00401030, 0xDEADBEEF]
    for i, v in enumerate(vt):
        struct.pack_into("<I", d, 0x400 + i * 4, v)
    # ascii string "B_Quit" at va 0x402014 (file 0x414)
    d[0x414 : 0x414 + 7] = b"B_Quit\x00"
    # utf-16le string "P_Start" at va 0x402020 (file 0x420)
    u16 = "P_Start".encode("utf-16-le") + b"\x00\x00"
    d[0x420 : 0x420 + len(u16)] = u16

    # .data content (file 0x500, only 0x40 bytes present; va 0x403000 vsize 0x200)
    struct.pack_into("<I", d, 0x500, 0x12345678)  # readable
    # va 0x403100 (file 0x600) is in the virtual tail -> NOT readable

    p = tmp_path / "tiny.xbe"
    p.write_bytes(bytes(d))
    return str(p), {
        "base": base,
        "vt_va": 0x00402000,
        "vt": vt,
        "str_va": 0x00402014,
        "u16_va": 0x00402020,
        "data_ok": 0x00403000,
        "data_tail": 0x00403100,
    }


@pytest.fixture
def xbe(tmp_path):
    path, meta = build_xbe(tmp_path)
    return xbe_peek.Xbe(path), meta


def test_sections(xbe):
    x, meta = xbe
    assert x.base == meta["base"]
    assert x.nsec == 3
    names = [s["name"] for s in x.sections]
    assert names == [".text", ".rdata", ".data"]
    data = [s for s in x.sections if s["name"] == ".data"][0]
    assert data["vsize"] > data["rsize"]  # virtual tail


def test_u32(xbe):
    x, meta = xbe
    assert x.u32(meta["data_ok"]) == 0x12345678


def test_u32s_vtable(xbe):
    x, meta = xbe
    vals = x.u32s(meta["vt_va"], 5)
    assert vals == meta["vt"]


def test_vtable_code_flag(xbe, capsys):
    x, meta = xbe
    # slots 0-3 are in .text (code), slot 4 is data
    x_vtable = xbe_peek.cmd_vtable
    x_vtable(x, meta["vt_va"], 5)
    out = capsys.readouterr().out
    assert out.count("<- code") == 4  # only the four .text slots flagged


def test_str_ascii(xbe):
    x, meta = xbe
    b, dec, is16 = x.str(meta["str_va"])
    assert not is16
    assert dec == "B_Quit"


def test_str_utf16(xbe):
    x, meta = xbe
    b, dec, is16 = x.str(meta["u16_va"])
    assert is16
    assert dec == "P_Start"


def test_virtual_tail_guard(xbe):
    x, meta = xbe
    assert x.readable(meta["data_ok"])
    assert not x.readable(meta["data_tail"])
    with pytest.raises(SystemExit):
        x.u32(meta["data_tail"])


def test_read_out_of_any_section(xbe):
    x, _ = xbe
    with pytest.raises(SystemExit):
        x.u32(0x00999999)


def test_main_cli_vtable(xbe, capsys):
    x, meta = xbe
    # exercise the CLI path end-to-end
    rc = xbe_peek.main(["xbe_peek.py", "vtable", x.path if hasattr(x, "path") else "", hex(meta["vt_va"]), "5"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "vt[0" in out and "vt[4" in out


def test_rejects_non_xbe(tmp_path):
    p = tmp_path / "not.xbe"
    p.write_bytes(b"NOTXBE" + b"\x00" * 0x200)
    with pytest.raises(SystemExit):
        xbe_peek.Xbe(str(p))
