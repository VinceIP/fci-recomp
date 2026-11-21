#!/usr/bin/env python3
"""xbe_peek.py -- read-only inspector for Xbox XBE executables.

The lift (build/lift/) is a C recompile of the XBE; it gives function *bodies*
but not reliable data (vtables, globals, string tables live in .rdata/.data and
the lift fragments them).  This tool reads the *binary* directly so you can
verify what the lift claims about data: vtable slots, pointer globals, strings.

It is strictly read-only: it opens the XBE, maps virtual addresses to file
offsets via the XBE section table, and prints bytes.  It never writes.

XBE header layout (little-endian, verified against the FCI/EVO discs):
  0x00  magic 'XBEH'
  0x104 base address (image base)
  0x11C section count
  0x120 section table virtual address (shva)
  section table file offset = shva - base
  each section header is 56 bytes = 6 x u32:
    (Flags, VirtualAddress, VirtualSize, RawAddress, RawSize, SectionNameVA)
  VA -> file offset:  off = (va - section.VirtualAddress) + section.RawAddress
  A VA is readable from the file only if it falls inside a section AND
  (va - section.VirtualAddress) < section.RawSize.  The .data section of these
  games has a large *virtual-only* tail (VirtualSize >> RawSize) that is zeroed
  at load and filled at runtime; VAs in that tail are NOT in the file.

Usage:
  xbe_peek.py sections <xbe>
      List sections (name, VA, vsize, raw, rsize).

  xbe_peek.py u32 <xbe> <va>
      Print the u32 at VA (hex + decimal).

  xbe_peek.py u32s <xbe> <va> <count>
      Print `count` consecutive u32s starting at VA (index: value).  This is
      how you read a vtable: `u32s xbe 0x61F234 24`.

  xbe_peek.py vtable <xbe> <va> <count>
      Like u32s but labels each slot (vt[0]..vt[n-1]) and flags which values
      look like code VAs (inside a .text section) vs data.

  xbe_peek.py str <xbe> <va>
      Read a string at VA.  Auto-detects UTF-16LE (BOM FF FE, or every other
      byte zero) vs 7-bit ASCII, and prints both the raw and decoded form.

  xbe_peek.py info <xbe>
      Print base address, section count, and the section table offset.

Examples:
  # The pak vtable on FCI: slot 19 (offset +0x4C) is the FindFile method.
  xbe_peek.py vtable build/qwen-in/xbe/fci-default.xbe 0x61F234 24

  # The 'B_Quit' action string.
  xbe_peek.py str build/qwen-in/xbe/fci-default.xbe 0x608CFC
"""

import struct
import sys

MAGIC = b"XBEH"


class Xbe:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        if self.d[:4] != MAGIC:
            raise SystemExit(f"{path}: not an XBE (magic {self.d[:4]!r})")
        self.base = struct.unpack_from("<I", self.d, 0x104)[0]
        self.nsec = struct.unpack_from("<I", self.d, 0x11C)[0]
        self.shva = struct.unpack_from("<I", self.d, 0x120)[0]
        self.to = self.shva - self.base
        self.sections = []
        for i in range(self.nsec):
            off = self.to + i * 56
            flags, sva, vsize, raw, rsize, nameva = struct.unpack_from("<IIIIII", self.d, off)
            noff = nameva - self.base
            end = self.d.find(b"\x00", noff, noff + 16)
            name = self.d[noff:end].decode("ascii", "replace")
            self.sections.append({"name": name, "va": sva, "vsize": vsize, "raw": raw, "rsize": rsize, "flags": flags})

    def _sec(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["vsize"]:
                return s
        return None

    def readable(self, va, size=1):
        s = self._sec(va)
        if not s:
            return False
        o = va - s["va"]
        return o + size <= s["rsize"]

    def read(self, va, size):
        s = self._sec(va)
        if not s:
            raise SystemExit(f"{va:#x}: not in any section")
        o = va - s["va"]
        if o + size > s["rsize"]:
            raise SystemExit(
                f"{va:#x}: in virtual-only tail of {s['name']} "
                f"(offset {o:#x} > raw size {s['rsize']:#x}); not in file, "
                f"set at runtime"
            )
        return self.d[s["raw"] + o : s["raw"] + o + size]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def u32s(self, va, count):
        return [struct.unpack("<I", self.read(va + 4 * i, 4))[0] for i in range(count)]

    def str(self, va, maxlen=256):
        s = self._sec(va)
        if s:
            avail = s["rsize"] - (va - s["va"])
            maxlen = min(maxlen, avail)
        b = self.read(va, maxlen)
        # UTF-16LE detect FIRST (BOM, or majority zero high bytes), because the
        # high bytes of UTF-16LE are zero and would truncate an ASCII scan.
        is16 = (len(b) >= 2 and b[:2] == b"\xff\xfe") or (
            len(b) >= 4
            and sum(1 for i in range(0, len(b) - 1, 2) if b[i + 1] == 0) * 2 >= len(b)
            # require at least one non-zero low byte so all-zero isn't 'utf16'
            and any(b[i] for i in range(0, len(b) - 1, 2))
        )
        if is16:
            # UTF-16LE null terminator is \x00\x00 at an EVEN offset; a char's
            # own high byte is \x00 at an odd offset, so scan even offsets only.
            z = None
            for i in range(0, len(b) - 1, 2):
                if b[i] == 0 and b[i + 1] == 0:
                    z = i
                    break
            if z is not None:
                b = b[:z]
            dec = b.decode("utf-16-le", "replace")
        else:
            z = b.find(b"\x00")
            if z >= 0:
                b = b[:z]
            dec = b.decode("latin-1")
        return b, dec, is16


def cmd_sections(x):
    print(f"base={x.base:#x}  nsec={x.nsec}  sectable@file={x.to:#x}")
    for s in x.sections:
        tail = ""
        if s["vsize"] > s["rsize"]:
            tail = f"  (virtual-only tail {s['vsize'] - s['rsize']:#x})"
        print(
            f"  {s['name']:<8} va={s['va']:#010x} vsize={s['vsize']:#010x} "
            f"raw={s['raw']:#010x} rsize={s['rsize']:#010x}{tail}"
        )


def cmd_u32(x, va):
    v = x.u32(va)
    print(f"{va:#010x} = {v:#010x} ({v})")


def cmd_u32s(x, va, count):
    for i, v in enumerate(x.u32s(va, count)):
        print(f"  [{i:<2}] {va + 4 * i:#010x} = {v:#010x}")


def cmd_vtable(x, va, count):
    text_vas = set()
    for s in x.sections:
        if s["name"] == ".text":
            text_vas = (s["va"], s["va"] + s["vsize"])
    vals = x.u32s(va, count)
    print(f"vtable at {va:#x} ({count} slots):")
    for i, v in enumerate(vals):
        tag = ""
        if text_vas and text_vas[0] <= v < text_vas[1]:
            tag = "  <- code"
        print(f"  vt[{i:<2}] (off {i * 4:#04x}) {v:#010x}{tag}")


def cmd_str(x, va):
    b, dec, is16 = x.str(va)
    enc = "utf-16le" if is16 else "ascii/latin1"
    print(f"{va:#010x} [{enc}] len={len(b)}")
    print(f"  raw: {b.hex()}")
    print(f"  str: {dec!r}")


def cmd_info(x):
    print(f"file={x.path}")
    print(f"base={x.base:#x}")
    print(f"nsec={x.nsec}")
    print(f"section table va={x.shva:#x} file_off={x.to:#x}")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    va_args = []
    for a in argv[3:]:
        va_args.append(int(a, 0) if a.lower().startswith("0x") or a.isdigit() else a)
    x = Xbe(path)
    if cmd == "sections":
        cmd_sections(x)
    elif cmd == "info":
        cmd_info(x)
    elif cmd == "u32" and len(va_args) == 1:
        cmd_u32(x, va_args[0])
    elif cmd in ("u32s", "vtable") and len(va_args) == 2:
        if cmd == "u32s":
            cmd_u32s(x, va_args[0], va_args[1])
        else:
            cmd_vtable(x, va_args[0], va_args[1])
    elif cmd == "str" and len(va_args) == 1:
        cmd_str(x, va_args[0])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
