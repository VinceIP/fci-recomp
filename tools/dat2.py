#!/usr/bin/env python3
"""dat2.py -- read-only lister/extractor for CryEngine DAT2 archives.

The Xbox builds of Far Cry Instincts / Instincts Evolution ship their game
data in paired files:

  <name>.fat   -- the index.  Layout (all little-endian):
                    magic  : 4 bytes  b"DAT2"
                    count  : u32      number of entries
                    entry[]: count x 12 bytes
                              offset : u32  byte offset into the .dat
                              size   : u32  payload length in bytes
                              hash   : u32  32-bit name hash (see below)

  <name>.dat   -- the payload store.  Raw, concatenated file contents.
                  Entry i lives at  dat[ offset_i : offset_i + size_i ].

There is NO compression: every payload is stored verbatim.  This is *not* a
zip archive; the .fat is a fixed-width FAT (file allocation table) and the
.dat is the data region.  The writer emits the b"DAT2" magic (see
build/lift/default/gen/recomp_0084.c, sub_002B9DC0) and the reader validates
it (build/lift/default/gen/recomp_0017.c, sub_0010D870).

Name -> hash
------------
Each entry's 32-bit `hash` is the standard CRC-32 (zlib.crc32) of the
normalised path: lower-case, '/' replaced by '\\', with a leading '\\'.
Example: crc32(b"\\magma\\crypregame.mgb") = 0xC5E270A2.  This was found
from a runtime trace (RECOMP_FCI_PAK_LOG) and holds for both Instincts and
Evolution.

This tool resolves entries three ways:
  --hash 0xNNNNNNNN   direct lookup by the stored hash (always works)
  --find SUBSTRING    locate the entry whose payload contains SUBSTRING
  --name PATH         normalise PATH (lower-case, / -> \\, leading \\) and
                      look up its CRC-32 in the FAT

`list` needs no hash function at all -- it walks the FAT and previews each
payload, auto-detecting the content type.

Usage:
  dat2.py list    <x.fat> [x.dat]
  dat2.py extract <x.fat> <x.dat> --hash 0xNNNNNNNN <out>
  dat2.py extract <x.fat> <x.dat> --find SUBSTRING <out>
  dat2.py extract <x.fat> <x.dat> --name PATH [--hash-fn NAME] <out>
  dat2.py entry   <x.fat> <x.dat> <index>     # dump one entry by FAT index
  dat2.py patch   <x.fat> <x.dat> --name PATH --from FILE
                  # same-size in-place replacement of one entry
"""

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"DAT2"


# --------------------------------------------------------------------------
# name normalisation and hash (for --name)
# --------------------------------------------------------------------------
def normalize_name(path):
    """Normalise a pak path the way the game hashes it.

    Lower-case, replace '/' with '\\', ensure a leading '\\'.
    Example: 'Magma/CryPregame.mgb' -> '\\\\magma\\crypregame.mgb'
    """
    p = path.lower().replace("/", "\\")
    if not p.startswith("\\"):
        p = "\\" + p
    return p


def name_hash(path):
    """CRC-32 of the normalised pak path (the DAT2 entry hash)."""
    return zlib.crc32(normalize_name(path).encode("utf-8")) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# FAT parsing
# --------------------------------------------------------------------------
class Fat:
    def __init__(self, fat_path, dat_path):
        self.fat_path = fat_path
        self.dat_path = dat_path
        raw = open(fat_path, "rb").read()
        if len(raw) < 8 or raw[:4] != MAGIC:
            raise SystemExit(f"error: {fat_path} does not start with {MAGIC!r} (got {raw[:4]!r}); not a DAT2 .fat")
        self.count = struct.unpack_from("<I", raw, 4)[0]
        expected = 8 + self.count * 12
        if len(raw) < expected:
            raise SystemExit(f"error: {fat_path} is {len(raw)} bytes but holds {self.count} entries (needs {expected})")
        self.entries = [struct.unpack_from("<III", raw, 8 + i * 12) for i in range(self.count)]
        self._dat = None

    def dat(self):
        if self._dat is None:
            self._dat = open(self.dat_path, "rb").read()
        return self._dat

    def payload(self, i):
        off, size, _ = self.entries[i]
        d = self.dat()
        if off + size > len(d):
            raise SystemExit(
                f"error: entry {i} (offset {off:#x} size {size:#x}) runs past end of {self.dat_path} ({len(d)} bytes)"
            )
        return d[off : off + size]


def classify(buf):
    """Best-effort content type for a payload (for `list` previews)."""
    if buf[:5] == b"\x1bLua" or buf[:4] == b"\x1bLua":
        return "lua"
    if buf[:4] == b"DAT2":
        return "dat2"
    if buf[:2] == b"\xff\xd8":
        return "jpeg"
    if buf[:4] == b"\x89PNG":
        return "png"
    if buf[:4] == b"DDS ":
        return "dds"
    if buf[:4] == b"PK\x03\x04":
        return "zip"
    if buf[:2] == b"\xfe\xff":
        return "utf16le"
    if buf[:1] == b"<" and b"xml" in buf[:120].lower():
        return "xml"
    if buf[:1] == b"<" and b"<?xml" in buf[:60]:
        return "xml"
    if all(b == 0 for b in buf[:16]):
        return "zeros"
    # printable-ratio heuristic
    if buf:
        printable = sum(1 for b in buf[:512] if 32 <= b < 127 or b in (9, 10, 13))
        if printable / min(len(buf), 512) > 0.85:
            return "text"
    return "binary"


def preview(buf, limit=48):
    t = buf[:limit]
    return "".join(chr(b) if 32 <= b < 127 else "." for b in t)


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------
def cmd_list(fat, args):
    print(f"# {fat.fat_path}  ({fat.count} entries)")
    print(f"# {fat.dat_path}")
    print(f"{'idx':>5}  {'offset':>10}  {'size':>9}  {'hash':>10}  type    preview")
    for i, (off, size, h) in enumerate(fat.entries):
        if args.limit and i >= args.limit:
            break
        buf = fat.payload(i)
        print(f"{i:>5}  {off:#010x}  {size:#09x}  {h:#010x}  {classify(buf):<7}  {preview(buf)}")


def cmd_entry(fat, args):
    if args.index >= fat.count:
        raise SystemExit(f"error: index {args.index} out of range (0..{fat.count - 1})")
    buf = fat.payload(args.index)
    sys.stdout.buffer.write(buf)


def _resolve(fat, args):
    """Return the FAT index selected by --hash / --find / --name."""
    if args.hash is not None:
        for i, (_, _, h) in enumerate(fat.entries):
            if h == args.hash:
                return i
        raise SystemExit(f"error: no entry with hash {args.hash:#x}")
    if args.find is not None:
        needle = args.find.encode()
        matches = []
        for i in range(fat.count):
            if needle in fat.payload(i):
                matches.append(i)
        if not matches:
            raise SystemExit(f"error: no entry contains {args.find!r}")
        if len(matches) > 1:
            raise SystemExit(
                f"error: {args.find!r} matches {len(matches)} entries "
                f"{matches[:8]}{'...' if len(matches) > 8 else ''}; narrow the search"
            )
        return matches[0]
    if args.name is not None:
        return _resolve_by_name(fat, args)
    raise SystemExit("error: one of --hash / --find / --name is required")


def _resolve_by_name(fat, args):
    """Resolve a path to a FAT entry via its CRC-32 hash.

    The hash is zlib.crc32 of the normalised path (lower-case, '/' -> '\\',
    leading '\\'). See normalize_name().
    """
    h = name_hash(args.name)
    norm = normalize_name(args.name)
    for i, (_, _, eh) in enumerate(fat.entries):
        if eh == h:
            print(f"# name {args.name!r} -> {norm!r} -> crc32 {h:#x} -> entry {i}", file=sys.stderr)
            return i
    raise SystemExit(
        f"error: no entry with hash {h:#x} (crc32 of {norm!r}). Use --find SUBSTRING or --hash 0xNNNNNNNN."
    )


def cmd_extract(fat, args):
    i = _resolve(fat, args)
    off, size, h = fat.entries[i]
    buf = fat.payload(i)
    with open(args.out, "wb") as f:
        f.write(buf)
    print(f"# extracted entry {i} (offset {off:#x} size {size:#x} hash {h:#x}) -> {args.out}  [{classify(buf)}]")


def _refuse_original(dat_path):
    """Refuse to modify a file that is likely the original game archive.

    Checks (any one is sufficient to refuse):
    1. The file is hard-linked (st_nlink > 1): the original is often
       hard-linked into the save directory by the launcher.
    2. The file is under a path that looks like a media/reference folder
       (build/, qwen-in/, or any path containing 'original').
    """
    st = os.stat(dat_path)
    if st.st_nlink > 1:
        raise SystemExit(
            f"error: {dat_path} is hard-linked (nlink={st.st_nlink}); "
            f"refusing to modify what may be the original. "
            f"Make a copy first: cp {dat_path} /tmp/test.dat"
        )
    lower = dat_path.lower().replace("\\", "/")
    for marker in ("/build/", "qwen-in", "/original"):
        if marker in lower:
            raise SystemExit(
                f"error: {dat_path} is under a media/reference path ({marker!r}); "
                f"refusing to modify what may be the original. "
                f"Make a copy in a scratch directory first."
            )


def cmd_patch(fat, args):
    """Same-size in-place replacement of one entry in the .dat."""
    i = _resolve(fat, args)
    off, size, h = fat.entries[i]
    # safety: refuse to modify the original (before any other check)
    _refuse_original(fat.dat_path)
    # read the replacement
    with open(args.from_file, "rb") as f:
        repl = f.read()
    if len(repl) != size:
        raise SystemExit(
            f"error: replacement is {len(repl)} bytes but entry {i} is {size} bytes; "
            f"patch requires a same-size replacement (repacks are not yet supported)"
        )
    # write in place
    with open(fat.dat_path, "r+b") as f:
        f.seek(off)
        cur = f.read(size)
        f.seek(off)
        f.write(repl)
    print(
        f"# patched entry {i} (offset {off:#x} size {size:#x} hash {h:#x}) "
        f"in {fat.dat_path}  [{classify(cur)} -> {classify(repl)}]"
    )


# --------------------------------------------------------------------------
# arg parsing
# --------------------------------------------------------------------------
def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_fat(sp):
        sp.add_argument("fat", help="path to the .fat index")
        sp.add_argument("--dat", help="path to the .dat (default: sibling .dat)")

    def finish(ns):
        if ns.dat is None:
            ns.dat = ns.fat[:-4] + ".dat" if ns.fat.endswith(".fat") else ns.fat + ".dat"
        return Fat(ns.fat, ns.dat)

    sp = sub.add_parser("list", help="list all FAT entries")
    add_fat(sp)
    sp.add_argument("--limit", type=int, default=0, help="only show first N entries")
    sp.set_defaults(func=lambda a: cmd_list(finish(a), a))

    sp = sub.add_parser("entry", help="dump one entry by FAT index to stdout")
    add_fat(sp)
    sp.add_argument("index", type=int)
    sp.set_defaults(func=lambda a: cmd_entry(finish(a), a))

    sp = sub.add_parser("extract", help="extract one entry to a file")
    add_fat(sp)
    sp.add_argument("--hash", type=lambda s: int(s, 0), help="entry hash, e.g. 0xCD5FCFB7")
    sp.add_argument("--find", help="extract the entry whose payload contains this text")
    sp.add_argument(
        "--name",
        help="resolve a path by its CRC-32 hash (normalised: lower-case, backslash, leading backslash)",
    )
    sp.add_argument("out", help="output file")
    sp.set_defaults(func=lambda a: cmd_extract(finish(a), a))

    sp = sub.add_parser("patch", help="same-size in-place replacement of one entry")
    add_fat(sp)
    sp.add_argument("--hash", type=lambda s: int(s, 0), help="entry hash, e.g. 0xCD5FCFB7")
    sp.add_argument("--find", help="the entry whose payload contains this text")
    sp.add_argument("--name", help="entry path (resolved by CRC-32 hash)")
    sp.add_argument("--from", dest="from_file", required=True, help="replacement file (must be same size as the entry)")
    sp.set_defaults(func=lambda a: cmd_patch(finish(a), a))

    a = p.parse_args(argv)
    a.func(a)


if __name__ == "__main__":
    main()
