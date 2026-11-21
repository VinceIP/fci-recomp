#!/usr/bin/env python3
"""Disassembler for Far Cry Instincts compiled Lua bytecode.

FCI ships its game logic as precompiled chunks produced by a modified Lua 4
interpreter (the CryScriptSystem "LUA" tree). The on-disk format is the stock
Lua 4.0 "undump" layout with one notable change: ``lua_Number`` is a 32-bit
``float`` rather than a 64-bit double, and the header's test constant is
pi*1e8.

Layout (little-endian):

  header   : 18 bytes
    0..3    signature  b"\\x1bLua"
    4       version    0x41
    5       endianness 0x01 (little-endian; no byte swap)
    6       sizeof(int)         = 4
    7       sizeof(size_t)      = 4
    8       sizeof(Instruction) = 4
    9       SIZE_OP             = 6
    10      SIZE_A              = 8
    11      SIZE_B              = 8
    12      SIZE_C              = 10
    13      sizeof(lua_Number)  = 4
    14..17  test number (float) = pi*1e8

  proto (repeated recursively for nested functions):
    source        : string            (size==0 means "inherit from parent")
    lineDefined   : u32
    nupvalues     : u32
    numparams     : u32
    is_vararg     : u32
    maxstacksize  : u32
    nlocals       : u32, then nlocals * (string name, u32 startpc, u32 endpc)
    nlines        : u32, then nlines * u32 (line per instruction)
    nconsts       : u32, then nconsts * (u8 tag, value)
                    tag 2 = number (float32)
                    tag 3 = string (u32 size incl. trailing NUL, then bytes)
    nnested       : u32, then nnested protos (recursive)
    ncode         : u32, then ncode * u32 (instructions)

  instruction (32-bit):
    OP : bits 0..5   (6 bits)
    C  : bits 6..15  (10 bits)
    B  : bits 16..23 (8 bits)
    A  : bits 24..31 (8 bits)
    Bc : bits 6..23  (18 bits)
    sBc: Bc - 2**17  (signed, excess-K)

A "R/K" operand is a register when its value < MAXSTACK (250), otherwise a
constant index (value - MAXSTACK).

Usage:
  python tools/lua_dis.py FILE [--function NAME] [--calls]
"""

import argparse
import struct
import sys

# --- constants taken from build/fc1-src/CryScriptSystem/LUA -----------------

SIGNATURE = b"\x1bLua"
VERSION = 0x41

LUA_TNIL = 1
LUA_TNUMBER = 2
LUA_TSTRING = 3

MAXSTACK = 250  # R/K threshold (llimits.h)
SIZE_OP = 6
SIZE_C = 10
SIZE_B = 8
SIZE_A = 8
POS_C = SIZE_OP  # 6
POS_B = POS_C + SIZE_C  # 16
POS_A = POS_B + SIZE_B  # 24
POS_Bc = POS_C  # 6
SIZE_Bc = SIZE_C + SIZE_B  # 18
MAX_sBc = 1 << (SIZE_Bc - 1)  # 131072

# opcode table (lopcodes.h / lopcodes.c), in order
OPCODES = [
    "MOVE",
    "LOADK",
    "LOADINT",
    "LOADNIL",
    "LOADUPVAL",
    "GETGLOBAL",
    "GETTABLE",
    "SETGLOBAL",
    "SETTABLE",
    "NEWTABLE",
    "SELF",
    "ADD",
    "SUB",
    "MUL",
    "DIV",
    "POW",
    "UNM",
    "NOT",
    "CONCAT",
    "JMP",
    "CJMP",
    "TESTEQ",
    "TESTNE",
    "TESTLT",
    "TESTLE",
    "TESTGT",
    "TESTGE",
    "TESTT",
    "TESTF",
    "NILJMP",
    "CALL",
    "RETURN",
    "FORPREP",
    "FORLOOP",
    "TFORPREP",
    "TFORLOOP",
    "SETLIST",
    "SETLISTO",
    "CLOSURE",
]
NUM_OPCODES = len(OPCODES)


class ParseError(Exception):
    """Raised when the byte stream does not match the expected layout."""


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def _need(self, n: int) -> None:
        if self.pos + n > len(self.data):
            raise ParseError(f"truncated read at offset {self.pos} (need {n} bytes)")

    def u8(self) -> int:
        self._need(1)
        b = self.data[self.pos]
        self.pos += 1
        return b

    def u32(self) -> int:
        self._need(4)
        v = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def f32(self) -> float:
        self._need(4)
        v = struct.unpack_from("<f", self.data, self.pos)[0]
        self.pos += 4
        return v

    def string(self):
        """Return the string, or None when the size field is 0."""
        size = self.u32()
        if size == 0:
            return None
        self._need(size)
        raw = self.data[self.pos : self.pos + size]
        self.pos += size
        if raw.endswith(b"\x00"):
            raw = raw[:-1]
        return raw.decode("utf-8", "replace")


class Proto:
    __slots__ = (
        "source",
        "line_defined",
        "nupvalues",
        "numparams",
        "is_vararg",
        "maxstacksize",
        "locals",
        "lines",
        "consts",
        "nested",
        "code",
    )

    def __init__(self):
        self.source = None
        self.line_defined = 0
        self.nupvalues = 0
        self.numparams = 0
        self.is_vararg = 0
        self.maxstacksize = 0
        self.locals = []  # list of (name, startpc, endpc)
        self.lines = []  # list of int
        self.consts = []  # list of str | float | None
        self.nested = []  # list of Proto
        self.code = []  # list of int (raw 32-bit words)

    @property
    def label(self) -> str:
        src = self.source if self.source is not None else "<(none)>"
        return f"{src}:{self.line_defined}"


def parse_proto(r: Reader, parent_source) -> Proto:
    p = Proto()
    p.source = r.string()
    if p.source is None:
        p.source = parent_source
    p.line_defined = r.u32()
    p.nupvalues = r.u32()
    p.numparams = r.u32()
    p.is_vararg = r.u32()
    p.maxstacksize = r.u32()

    nlocals = r.u32()
    for _ in range(nlocals):
        name = r.string()
        startpc = r.u32()
        endpc = r.u32()
        p.locals.append((name, startpc, endpc))

    nlines = r.u32()
    for _ in range(nlines):
        p.lines.append(r.u32())

    nconsts = r.u32()
    for _ in range(nconsts):
        tag = r.u8()
        if tag == LUA_TNUMBER:
            p.consts.append(r.f32())
        elif tag == LUA_TSTRING:
            p.consts.append(r.string())
        else:
            raise ParseError(f"bad constant type tag {tag} at offset {r.pos - 1}")

    nnested = r.u32()
    for _ in range(nnested):
        p.nested.append(parse_proto(r, p.source))

    ncode = r.u32()
    for _ in range(ncode):
        p.code.append(r.u32())

    return p


def parse_header(r: Reader) -> dict:
    r._need(4)
    sig = r.data[r.pos : r.pos + 4]
    if sig != SIGNATURE:
        raise ParseError(f"bad signature {sig!r} (expected {SIGNATURE!r})")
    r.pos += 4
    version = r.u8()
    if version != VERSION:
        raise ParseError(f"unrecognised version byte 0x{version:02x} (expected 0x{VERSION:02x})")
    endian = r.u8()
    if endian != 1:
        raise ParseError(f"unsupported endianness byte 0x{endian:02x} (only little-endian 0x01 is handled)")
    sizes = {
        "int": r.u8(),
        "size_t": r.u8(),
        "Instruction": r.u8(),
        "SIZE_OP": r.u8(),
        "SIZE_A": r.u8(),
        "SIZE_B": r.u8(),
        "SIZE_C": r.u8(),
        "lua_Number": r.u8(),
    }
    expected = {
        "int": 4,
        "size_t": 4,
        "Instruction": 4,
        "SIZE_OP": 6,
        "SIZE_A": 8,
        "SIZE_B": 8,
        "SIZE_C": 10,
        "lua_Number": 4,
    }
    for k, want in expected.items():
        if sizes[k] != want:
            raise ParseError(f"header size field '{k}' is {sizes[k]}, expected {want}")
    test_number = r.f32()
    return {"version": version, "sizes": sizes, "test_number": test_number}


def parse_chunk(data: bytes):
    r = Reader(data)
    header = parse_header(r)
    main = parse_proto(r, None)
    if r.pos != len(data):
        raise ParseError(f"trailing garbage: {len(data) - r.pos} bytes after final proto")
    return header, main


# --- disassembly -----------------------------------------------------------


def _get_op(word: int) -> int:
    return word & 0x3F


def _get_a(word: int) -> int:
    return (word >> POS_A) & 0xFF


def _get_b(word: int) -> int:
    return (word >> POS_B) & 0xFF


def _get_c(word: int) -> int:
    return (word >> POS_C) & 0x3FF


def _get_bc(word: int) -> int:
    return (word >> POS_Bc) & 0x3FFFF


def _get_sbc(word: int) -> int:
    return _get_bc(word) - MAX_sBc


def _fmt_const(idx: int, consts) -> str:
    if 0 <= idx < len(consts):
        c = consts[idx]
        if c is None:
            return f"Kst({idx}) nil"
        if isinstance(c, str):
            return f'Kst({idx}) "{c}"'
        return f"Kst({idx}) {c:g}"
    return f"Kst({idx}) <out of range>"


def _fmt_rk(val: int, consts) -> str:
    if val < MAXSTACK:
        return f"R({val})"
    return _fmt_const(val - MAXSTACK, consts)


def render_instruction(word: int, consts) -> str:
    op = _get_op(word)
    if op >= NUM_OPCODES:
        return f"<unknown opcode {op}>"
    name = OPCODES[op]
    a = _get_a(word)
    b = _get_b(word)
    c = _get_c(word)
    bc = _get_bc(word)
    sbc = _get_sbc(word)

    def rk(v):
        return _fmt_rk(v, consts)

    simple = {
        "MOVE": f"R({a}) R({b})",
        "LOADK": f"R({a}) {_fmt_const(bc, consts)}",
        "LOADINT": f"R({a}) {sbc}",
        "LOADNIL": f"R({a}) R({b})",
        "LOADUPVAL": f"R({a}) UpValue[{bc}]",
        "GETGLOBAL": f"R({a}) Gbl[{_fmt_const(bc, consts)}]",
        "GETTABLE": f"R({a}) R({b}) {rk(c)}",
        "SETGLOBAL": f"Gbl[{_fmt_const(bc, consts)}] R({a})",
        "SETTABLE": f"R({b}) {rk(c)} R({a})",
        "NEWTABLE": f"R({a}) size={bc}",
        "SELF": f"R({a}) R({b}) {rk(c)}",
        "ADD": f"R({a}) R({b}) {rk(c)}",
        "SUB": f"R({a}) R({b}) {rk(c)}",
        "MUL": f"R({a}) R({b}) {rk(c)}",
        "DIV": f"R({a}) R({b}) {rk(c)}",
        "POW": f"R({a}) R({b}) {rk(c)}",
        "UNM": f"R({a}) R({b})",
        "NOT": f"R({a}) R({b})",
        "CONCAT": f"R({a}) R({b}) R({c})",
        "JMP": f"{sbc}",
        "CJMP": f"{sbc}",
        "TESTT": f"R({a}) R({b})",
        "TESTF": f"R({a}) R({b})",
        "NILJMP": f"R({a})",
        "CALL": f"R({a}) narg={b} nret={c}",
        "RETURN": f"R({a}) R({b})",
        "FORPREP": f"R({a}) {sbc}",
        "FORLOOP": f"R({a}) {sbc}",
        "TFORPREP": f"R({a}) {sbc}",
        "TFORLOOP": f"R({a}) {sbc}",
        "SETLIST": f"R({a}) {bc}",
        "SETLISTO": f"R({a}) {bc}",
        "CLOSURE": f"R({a}) KPROTO[{bc}]",
    }
    if name in simple:
        return f"{name}\t{simple[name]}"
    sym = {"TESTEQ": "==", "TESTNE": "~=", "TESTLT": "<", "TESTLE": "<=", "TESTGT": ">", "TESTGE": ">="}
    if name in sym:
        return f"{name}\tR({a}) {sym[name]} {rk(c)}"
    return f"{name}\tA={a} B={b} C={c} Bc={bc}"


def iter_functions(root: Proto):
    """Yield (depth, proto) in pre-order."""
    stack = [(0, root)]
    while stack:
        depth, p = stack.pop()
        yield depth, p
        for child in reversed(p.nested):
            stack.append((depth + 1, child))


def find_functions(root: Proto, name: str):
    """Find protos matching `name`.

    `name` may be a line number (``92``), a source substring (``Checkpoint``),
    or ``source:line``. When only a line is given, the whole tree is searched
    (nested functions share their parent's source, so the line is the
    distinguishing key).
    """
    want_line = None
    key = name
    if ":" in name:
        base, _, ln = name.rpartition(":")
        if ln.isdigit():
            want_line = int(ln)
            key = base
    if key.isdigit():
        want_line = int(key)
        key = None
    key_lc = key.lower() if key else None

    out = []
    for _depth, p in iter_functions(root):
        line_ok = want_line is None or p.line_defined == want_line
        src_ok = key_lc is None or key_lc in (p.source or "").lower()
        if line_ok and src_ok:
            out.append(p)
    return out


def collect_calls(p: Proto):
    """Return the ordered list of global/method names this function references
    as call targets (OP_SELF method operand, OP_GETGLOBAL names)."""
    calls = []
    for word in p.code:
        op = _get_op(word)
        if op >= NUM_OPCODES:
            continue
        if op == OPCODES.index("SELF"):
            c = _get_c(word)
            if c >= MAXSTACK:
                idx = c - MAXSTACK
                if 0 <= idx < len(p.consts) and isinstance(p.consts[idx], str):
                    calls.append(p.consts[idx])
        elif op == OPCODES.index("GETGLOBAL"):
            bc = _get_bc(word)
            if 0 <= bc < len(p.consts) and isinstance(p.consts[bc], str):
                calls.append(p.consts[bc])
    return calls


def print_proto(p: Proto, depth: int = 0, out=sys.stdout) -> None:
    pad = "  " * depth
    c = print
    c(f"{pad}function {p.label}", file=out)
    c(
        f"{pad}  params={p.numparams} vararg={p.is_vararg} "
        f"maxstack={p.maxstacksize} upvalues={p.nupvalues} "
        f"consts={len(p.consts)} nested={len(p.nested)} insns={len(p.code)}",
        file=out,
    )
    if p.consts:
        c(f"{pad}  constants:", file=out)
        for i, k in enumerate(p.consts):
            if k is None:
                val = "nil"
            elif isinstance(k, str):
                val = f'"{k}"'
            else:
                val = f"{k:g}"
            c(f"{pad}    [{i}] {val}", file=out)
    if p.locals:
        c(f"{pad}  locals:", file=out)
        for name, s, e in p.locals:
            c(f"{pad}    {name} (pc {s}..{e})", file=out)
    if p.code:
        c(f"{pad}  instructions:", file=out)
        for i, word in enumerate(p.code):
            line = ""
            if i < len(p.lines):
                line = f"  ; line {p.lines[i]}"
            c(f"{pad}    [{i:4d}] {render_instruction(word, p.consts)}{line}", file=out)
    for child in p.nested:
        c(f"{pad}  nested:", file=out)
        print_proto(child, depth + 1, out)


def print_calls(root: Proto, out=sys.stdout) -> None:
    c = print
    c(f"# call targets (globals/methods) per function: {root.source}", file=out)
    for depth, p in iter_functions(root):
        pad = "  " * depth
        calls = collect_calls(p)
        if calls:
            c(f"{pad}{p.label}: {', '.join(calls)}", file=out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Disassemble FCI compiled Lua bytecode (modified Lua 4).")
    ap.add_argument("file", help="path to a compiled .lua chunk")
    ap.add_argument(
        "--function",
        metavar="NAME",
        help="only disassemble function(s) whose source matches NAME (use NAME:LINE to also match the definition line)",
    )
    ap.add_argument("--calls", action="store_true", help="print only the global/method call names per function")
    args = ap.parse_args(argv)

    try:
        data = open(args.file, "rb").read()
    except OSError as e:
        print(f"error: cannot read {args.file}: {e}", file=sys.stderr)
        return 2

    try:
        header, root = parse_chunk(data)
    except ParseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.calls:
        print_calls(root)
        return 0

    if args.function:
        matches = find_functions(root, args.function)
        if not matches:
            print(f"error: no function with source matching {args.function!r}", file=sys.stderr)
            return 1
        for p in matches:
            print_proto(p)
    else:
        print(
            f"# {args.file}: version=0x{header['version']:02x} "
            f"lua_Number={header['sizes']['lua_Number']}B "
            f"test={header['test_number']:g}",
            file=sys.stderr,
        )
        print_proto(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
