"""The games' lua.dat: its DAT2 index (lua.fat) and the compiled Lua scripts in it (re/notes/WEAPONS.md).

A script is a Lua 5.0-like chunk with 4-byte float numbers. Each function: its source, five header ints,
line info, locals, typed constants (3 a string, 0 nil, anything else a float), nested functions, code.
An instruction is 32 bits: op = w & 0x3F, A = w >> 24, Bx = (w >> 6) & 0x3FFFF, sBx = Bx - 0x1FFFF;
SETTABLE's table register is (w >> 15) & 0x1FF and its key (w >> 6) & 0x1FF, a constant from 250 up.
"""

import struct

LOADK, LOADINT, SETTABLE = 1, 2, 8
RK_CONSTANT = 250  # an RK operand from here up names constant RK - 250
SBX_BIAS = 0x1FFFF


class Function:
    pass


def index(fat):
    """(offset, size, name hash) of each file in a DAT2 archive"""
    if fat[:4] != b"DAT2":
        raise ValueError("not a DAT2 index")
    return [struct.unpack_from("<III", fat, 8 + i * 12) for i in range(struct.unpack_from("<I", fat, 4)[0])]


def parse(chunk):
    """a script's main function; ValueError when it is not one or does not end where the chunk does"""
    p = 18  # signature, version, type sizes, the test number

    def i32():
        nonlocal p
        v = struct.unpack_from("<i", chunk, p)[0]
        p += 4
        return v

    def string():
        nonlocal p
        n = i32()
        s = chunk[p : p + n - 1].decode("latin-1") if n else None
        p += n
        return s

    def function():
        nonlocal p
        f = Function()
        f.source = string()
        f.header = [i32() for _ in range(5)]
        lines = i32()
        p += 4 * lines  # line info
        for _ in range(i32()):  # locals: name, start, end
            string()
            i32()
            i32()
        f.k = []
        for _ in range(i32()):
            kind = chunk[p]
            p += 1
            if kind == 3:
                f.k.append(string())
            elif kind == 0:
                f.k.append(None)
            else:
                f.k.append(struct.unpack_from("<f", chunk, p)[0])
                p += 4
        f.protos = [function() for _ in range(i32())]
        n = i32()
        f.code_at = p
        f.code = struct.unpack_from(f"<{n}I", chunk, p)
        p += 4 * n
        return f

    if chunk[:4] != b"\x1bLua":
        raise ValueError("not a Lua chunk")
    try:
        f = function()
    except struct.error:
        raise ValueError("a Lua chunk this reader does not understand")
    if p != len(chunk):
        raise ValueError("a Lua chunk this reader does not understand")
    return f


def functions(f):
    yield f
    for g in f.protos:
        yield from functions(g)


def name(f):
    """the function's first string constant: a weapon script's weapon"""
    return next((k for k in f.k if isinstance(k, str)), None)


def int_fields(f, names):
    """(field, value, offset of its LOADINT in the chunk) for each table field in names set from an integer"""
    found = []
    for i, w in enumerate(f.code):
        key = (w >> 6) & 0x1FF
        if w & 0x3F != SETTABLE or not RK_CONSTANT <= key < RK_CONSTANT + len(f.k):
            continue
        field = f.k[key - RK_CONSTANT]
        if field not in names:
            continue
        load = next((j for j in range(i - 1, max(-1, i - 4), -1) if f.code[j] >> 24 == w >> 24), None)
        if load is not None and f.code[load] & 0x3F == LOADINT:
            found.append((field, ((f.code[load] >> 6) & 0x3FFFF) - SBX_BIAS, f.code_at + 4 * load))
    return found


def load_int(word, value):
    """a LOADINT into the same register as word, of value"""
    return (word & 0xFF000000) | ((value + SBX_BIAS) << 6) | LOADINT
