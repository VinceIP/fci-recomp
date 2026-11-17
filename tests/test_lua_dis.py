"""tools/lua_dis.py on synthetic chunks built in-test (no game data).

Covers: header, proto fields, constants (string + number), nested functions,
a few opcodes' rendering, R/K resolution, and the failing bad-header case.
"""

from pathlib import Path
import struct
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import lua_dis  # noqa: E402

# --- synthetic chunk writer -------------------------------------------------


def _string(s):
    """LoadString encoding: u32 size (incl. trailing NUL) + bytes, or size 0 for None."""
    if s is None:
        return struct.pack("<I", 0)
    b = s.encode("utf-8") + b"\x00"
    return struct.pack("<I", len(b)) + b


def _const(v):
    if isinstance(v, str):
        return bytes([lua_dis.LUA_TSTRING]) + _string(v)
    if isinstance(v, (int, float)):
        return bytes([lua_dis.LUA_TNUMBER]) + struct.pack("<f", float(v))
    raise TypeError(v)


def _insns(words):
    return struct.pack("<I", len(words)) + b"".join(struct.pack("<I", w) for w in words)


def abc(op, a, b, c):
    return op | (a << lua_dis.POS_A) | (b << lua_dis.POS_B) | (c << lua_dis.POS_C)


def abc_op(op, a, bc):
    return op | (a << lua_dis.POS_A) | (bc << lua_dis.POS_Bc)


def sbc_op(op, a, sbc):
    return abc_op(op, a, (sbc + lua_dis.MAX_sBc) & 0x3FFFF)


def proto(source, line, params, vararg, maxstack, consts, code, nested=(), locals_=(), lines=(), nupvalues=0):
    out = b""
    out += _string(source)
    out += struct.pack("<IIIII", line, nupvalues, params, vararg, maxstack)
    # locals
    out += struct.pack("<I", len(locals_))
    for name, s, e in locals_:
        out += _string(name) + struct.pack("<II", s, e)
    # lines
    out += struct.pack("<I", len(lines)) + b"".join(struct.pack("<I", x) for x in lines)
    # constants
    out += struct.pack("<I", len(consts)) + b"".join(_const(c) for c in consts)
    # nested
    out += struct.pack("<I", len(nested)) + b"".join(nested)
    # code
    out += _insns(code)
    return out


def header():
    return (
        lua_dis.SIGNATURE
        + bytes([lua_dis.VERSION, 1])
        + bytes([4, 4, 4, 6, 8, 8, 10, 4])
        + struct.pack("<f", 3.14159265358979323846e8)
    )


def chunk(main_proto):
    return header() + main_proto


# --- tests ------------------------------------------------------------------


def test_header_roundtrip():
    data = chunk(proto("=(none)", 0, 0, 0, 1, [], []))
    h, _ = lua_dis.parse_chunk(data)
    assert h["version"] == 0x41
    assert h["sizes"]["lua_Number"] == 4
    assert h["sizes"]["SIZE_OP"] == 6
    assert abs(h["test_number"] - 3.14159265e8) < 2.0


def test_bad_signature_fails():
    data = b"\x1bXua" + header()[3:]
    with pytest.raises(lua_dis.ParseError):
        lua_dis.parse_chunk(data)


def test_bad_version_fails():
    data = header()
    data = data[:4] + bytes([0x99]) + data[5:]
    with pytest.raises(lua_dis.ParseError):
        lua_dis.parse_chunk(data)


def test_bad_size_field_fails():
    # claim lua_Number is 8 bytes -> rejected
    data = header().replace(bytes([4, 4, 4, 6, 8, 8, 10, 4]), bytes([4, 4, 4, 6, 8, 8, 10, 8]))
    with pytest.raises(lua_dis.ParseError):
        lua_dis.parse_chunk(data)


def test_trailing_garbage_fails():
    data = chunk(proto("=(none)", 0, 0, 0, 1, [], [])) + b"\x00\x00"
    with pytest.raises(lua_dis.ParseError):
        lua_dis.parse_chunk(data)


def test_proto_fields():
    inner = proto(None, 7, 2, 1, 5, ["x"], [abc(0, 0, 1, 0)])  # MOVE R0 R1
    outer = proto(
        "MyScript",
        1,
        0,
        0,
        3,
        ["a", 2.5],
        [abc_op(1, 0, 0), abc_op(1, 1, 1)],  # LOADK R0 K0, LOADK R1 K1
        nested=(inner,),
    )
    data = chunk(outer)
    _, p = lua_dis.parse_chunk(data)
    assert p.source == "MyScript"
    assert p.line_defined == 1
    assert p.numparams == 0
    assert p.maxstacksize == 3
    assert p.consts == ["a", pytest.approx(2.5, abs=1e-6)]
    assert len(p.nested) == 1
    child = p.nested[0]
    assert child.source == "MyScript"  # inherited
    assert child.line_defined == 7
    assert child.numparams == 2
    assert child.is_vararg == 1
    assert child.consts == ["x"]
    assert child.code == [abc(0, 0, 1, 0)]


def test_locals_and_lines():
    p = proto("S", 0, 1, 0, 2, [], [abc(0, 0, 1, 0)], locals_=[("i", 0, 3)], lines=[10, 11, 12])
    _, got = lua_dis.parse_chunk(chunk(p))
    assert got.locals == [("i", 0, 3)]
    assert got.lines == [10, 11, 12]


def test_render_move():
    w = abc(0, 3, 5, 0)
    assert lua_dis.render_instruction(w, []) == "MOVE\tR(3) R(5)"


def test_render_loadk_string():
    w = abc_op(1, 2, 0)
    out = lua_dis.render_instruction(w, ["hello"])
    assert out == 'LOADK\tR(2) Kst(0) "hello"'


def test_render_loadk_number():
    w = abc_op(1, 0, 1)
    out = lua_dis.render_instruction(w, [0, 4.0])
    assert out == "LOADK\tR(0) Kst(1) 4"


def test_render_getglobal():
    w = abc_op(5, 0, 0)
    out = lua_dis.render_instruction(w, ["Checkpoint"])
    assert out == 'GETGLOBAL\tR(0) Gbl[Kst(0) "Checkpoint"]'


def test_render_self_method():
    # SELF A B C, C is R/K; use a constant index via C >= MAXSTACK
    c = lua_dis.MAXSTACK + 0
    w = abc(10, 1, 2, c)
    out = lua_dis.render_instruction(w, ["OnEnterArea"])
    assert out == 'SELF\tR(1) R(2) Kst(0) "OnEnterArea"'


def test_render_rk_register_vs_const():
    # GETTABLE A B C with C < MAXSTACK -> register
    w = abc(6, 0, 1, 2)
    assert lua_dis.render_instruction(w, []) == "GETTABLE\tR(0) R(1) R(2)"
    # C >= MAXSTACK -> constant (index 0)
    w = abc(6, 0, 1, lua_dis.MAXSTACK + 0)
    assert lua_dis.render_instruction(w, ["k"]) == 'GETTABLE\tR(0) R(1) Kst(0) "k"'


def test_render_sbc_jump():
    w = sbc_op(19, 0, -5)  # JMP -5
    assert lua_dis.render_instruction(w, []) == "JMP\t-5"
    w = sbc_op(19, 0, 7)  # JMP 7
    assert lua_dis.render_instruction(w, []) == "JMP\t7"


def test_render_loadint():
    w = sbc_op(2, 0, 42)  # LOADINT R0 42
    assert lua_dis.render_instruction(w, []) == "LOADINT\tR(0) 42"


def test_unknown_opcode_named():
    # opcode 63 (all ones in 6 bits) is beyond the table
    w = 0x3F
    assert lua_dis.render_instruction(w, []) == "<unknown opcode 63>"


def test_collect_calls():
    # a function that does: SELF self,method ; GETGLOBAL Game
    method_idx = lua_dis.MAXSTACK + 1
    consts = ["x", "OnReset", "Game"]
    code = [
        abc(10, 1, 0, method_idx),  # SELF R1 R0 Kst(1) "OnReset"
        abc_op(5, 2, 2),  # GETGLOBAL R2 Gbl[Kst(2) "Game"]
        abc(30, 1, 1, 0),  # CALL
    ]
    p = lua_dis.Proto()
    p.consts = consts
    p.code = code
    assert lua_dis.collect_calls(p) == ["OnReset", "Game"]


def test_find_by_line():
    inner = proto(None, 92, 3, 0, 8, ["t"], [abc(0, 0, 0, 0)])
    outer = proto("=(none)", 0, 0, 0, 3, [], [], nested=(inner,))
    _, root = lua_dis.parse_chunk(chunk(outer))
    matches = lua_dis.find_functions(root, "92")
    assert len(matches) == 1
    assert matches[0].line_defined == 92


def test_end_to_end_disassemble(tmp_path):
    inner = proto(
        None, 5, 1, 0, 3, ["OnReset"], [abc(10, 1, 0, lua_dis.MAXSTACK + 0), abc(30, 1, 1, 0), abc(31, 0, 0, 0)]
    )
    outer = proto(
        "Checkpoint", 1, 0, 0, 3, ["Checkpoint", "OnReset"], [abc_op(5, 0, 0), abc(31, 0, 0, 0)], nested=(inner,)
    )
    f = tmp_path / "c.lua"
    f.write_bytes(chunk(outer))
    rc = lua_dis.main([str(f)])
    assert rc == 0
    rc = lua_dis.main([str(f), "--calls"])
    assert rc == 0
    rc = lua_dis.main([str(f), "--function", "5"])
    assert rc == 0


def test_main_bad_file_rc(tmp_path):
    f = tmp_path / "x.lua"
    f.write_bytes(b"\x1bLua\x00" + b"\x00" * 20)  # bad version
    assert lua_dis.main([str(f)]) == 1
