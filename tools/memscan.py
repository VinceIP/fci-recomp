#!/usr/bin/env python3
"""memscan.py — scan a 32-bit Xbox FCI memory dump for candidate field values.

The dump is a flat image of the guest's linear memory: guest VA range
[0x00010000, 0x04000000), file offset = VA - 0x10000 (see src/fci_threads.c,
the RECOMP_FCI_MEMDUMP / `dump:` step handler). 32-bit, little-endian.

Subcommands
  scan   <dump> <type> <value>            first pass: every VA where <type>==<value>
  filter <dump> <vaf> <type> <value>      narrow a candidate file to the new value
  vec    <dump> <vaf> <type> <n>          read n consecutive <type> at each VA
  ptr    <dump> <vaf> <lo> <hi>           keep VAs whose u32 there is a ptr in [lo,hi)
  peek   <dump> <va> <type> <n>           print n consecutive <type> from a VA
  delta  <dumpA> <dumpB> <type> <dval>    VAs where value(B)-value(A) == <dval>

<type> is one of: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64
<value> is decimal or 0x-hex; for float types a decimal float.
<vaf> is a file of VAs, one per line, 0x-prefixed hex (what scan/filter emit).
Output: candidate VAs one per line as 0x%08X on stdout; the count on stderr.
A `--max N` flag caps stdout lines (the count is still the true total).

Everything is numpy-backed; a full 64 MB scan is well under a second.
No game is involved — this only reads a dump file that already exists.
"""

import argparse
import sys

import numpy as np

VA_BASE = 0x00010000  # first mapped guest VA
VA_TOP = 0x04000000  # one past the last mapped guest VA

# dtype name -> (numpy dtype, itemsize)
TYPES = {
    "i8": (np.int8, 1),
    "u8": (np.uint8, 1),
    "i16": (np.int16, 2),
    "u16": (np.uint16, 2),
    "i32": (np.int32, 4),
    "u32": (np.uint32, 4),
    "i64": (np.int64, 8),
    "u64": (np.uint64, 8),
    "f32": (np.float32, 4),
    "f64": (np.float64, 8),
}


def load_dump(path):
    buf = np.fromfile(path, dtype=np.uint8)
    if len(buf) < 0x10000:
        sys.exit(f"dump too small: {len(buf)} bytes (expected >= 0x{VA_TOP - VA_BASE:x})")
    return buf


def parse_value(s, dt):
    if np.issubdtype(dt, np.floating):
        return float(s)
    return int(s, 0)


def va_to_idx(va, nbuf):
    idx = va - VA_BASE
    if idx < 0 or idx >= nbuf:
        raise ValueError(f"VA 0x{va:x} outside dump range")
    return idx


def read_at(buf, va, dt, isize):
    """Read one value of type dt at guest VA va (handles any alignment)."""
    idx = va_to_idx(va, len(buf))
    return (
        int(np.frombuffer(buf[idx : idx + isize], dtype=dt)[0])
        if not np.issubdtype(dt, np.floating)
        else float(np.frombuffer(buf[idx : idx + isize], dtype=dt)[0])
    )


def gather(buf, vas, dt, isize):
    """Vectorized read of one value per VA (byte-gather, alignment-agnostic)."""
    idxs = np.array([va_to_idx(v, len(buf)) for v in vas], dtype=np.int64)
    if np.issubdtype(dt, np.floating):
        # little-endian float: assemble the bytes then view
        raw = np.empty((len(vas), isize), dtype=np.uint8)
        for b in range(isize):
            raw[:, b] = buf[idxs + b]
        return raw.view(dt).ravel()
    # integer: assemble little-endian integer from bytes
    acc = np.zeros(len(vas), dtype=np.uint64)
    for b in range(isize):
        acc |= buf[idxs + b].astype(np.uint64) << (8 * b)
    if np.issubdtype(dt, np.unsignedinteger):
        return acc.astype(dt)
    # signed: two's-complement reinterpret over the low isize bytes
    mask = (1 << (8 * isize)) - 1
    acc &= mask
    signbit = 1 << (8 * isize - 1)
    acc = np.where(acc & signbit, acc - (1 << (8 * isize)), acc)
    return acc.astype(dt)


def emit(vas, max_n):
    vas = sorted(vas)
    for i, v in enumerate(vas):
        if max_n and i >= max_n:
            break
        print(f"0x{v:08X}")
    print(f"# {len(vas)} candidates", file=sys.stderr)


def read_vaf(path):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.append(int(line, 16))
    return out


def cmd_scan(a):
    dt, isize = TYPES[a.type]
    buf = load_dump(a.dump)
    val = parse_value(a.value, dt)
    found = set()
    # scan at every sub-alignment so a field at any alignment is caught
    for shift in range(isize):
        seg = buf[shift:]
        n = (len(seg) // isize) * isize
        if n == 0:
            continue
        view = np.frombuffer(seg[:n], dtype=dt)
        mask = view == val
        if not mask.any():
            continue
        hit = np.nonzero(mask)[0]
        # VA = VA_BASE + shift + i*isize
        found.update((VA_BASE + shift + hit * isize).tolist())
    emit(sorted(found), a.max)


def cmd_filter(a):
    dt, isize = TYPES[a.type]
    buf = load_dump(a.dump)
    val = parse_value(a.value, dt)
    vas = read_vaf(a.vaf)
    vals = gather(buf, vas, dt, isize)
    keep = [v for v, x in zip(vas, vals) if x == val]
    emit(keep, a.max)


def cmd_vec(a):
    dt, isize = TYPES[a.type]
    buf = load_dump(a.dump)
    vas = read_vaf(a.vaf)
    for v in vas:
        idx = va_to_idx(v, len(buf))
        if idx + a.n * isize > len(buf):
            continue
        row = np.frombuffer(buf[idx : idx + a.n * isize], dtype=dt)
        fmt = " ".join(f"{x:g}" if np.issubdtype(dt, np.floating) else str(int(x)) for x in row)
        print(f"0x{v:08X}: {fmt}")


def cmd_ptr(a):
    dt, _ = TYPES["u32"]
    buf = load_dump(a.dump)
    vas = read_vaf(a.vaf)
    lo, hi = int(a.lo, 0), int(a.hi, 0)
    vals = gather(buf, vas, dt, 4)
    keep = [v for v, x in zip(vas, vals) if lo <= int(x) < hi]
    emit(keep, a.max)


def cmd_delta(a):
    """Print VAs where value(B) - value(A) == the expected delta. Finds free-running
    counters (the match clock, ~+50/s) and event-driven fields (health, -N on damage).
    The two dumps must be the same size."""
    da = load_dump(a.a)
    db = load_dump(a.b)
    if da.shape[0] != db.shape[0]:
        sys.exit("the two dumps have different sizes")
    dt, isize = TYPES[a.type]
    d = parse_value(a.dval, dt)
    found = set()
    for shift in range(isize):
        n = ((len(da) - shift) // isize) * isize
        if n == 0:
            continue
        va = np.frombuffer(da[shift : shift + n], dtype=dt)
        vb = np.frombuffer(db[shift : shift + n], dtype=dt)
        diff = (vb - va).astype(np.int64)
        hit = np.nonzero(diff == d)[0]
        found.update((VA_BASE + shift + hit * isize).tolist())
    emit(sorted(found), a.max)


def cmd_peek(a):
    dt, isize = TYPES[a.type]
    buf = load_dump(a.dump)
    idx = va_to_idx(int(a.va, 0), len(buf))
    if idx + a.n * isize > len(buf):
        sys.exit("peek range exceeds dump")
    row = np.frombuffer(buf[idx : idx + a.n * isize], dtype=dt)
    for i, x in enumerate(row):
        va = int(a.va, 0) + i * isize
        if np.issubdtype(dt, np.floating):
            print(f"0x{va:08X}: {float(x):g}")
        else:
            print(f"0x{va:08X}: {int(x)}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--max", type=int, default=0, help="cap stdout lines (0 = all)")

    sp = sub.add_parser("scan", help="first pass")
    sp.add_argument("dump")
    sp.add_argument("type", choices=sorted(TYPES))
    sp.add_argument("value")
    add_common(sp)
    sp.set_defaults(fn=cmd_scan)

    sp = sub.add_parser("filter", help="narrow a candidate file")
    sp.add_argument("dump")
    sp.add_argument("vaf")
    sp.add_argument("type", choices=sorted(TYPES))
    sp.add_argument("value")
    add_common(sp)
    sp.set_defaults(fn=cmd_filter)

    sp = sub.add_parser("vec", help="read a vector at each VA")
    sp.add_argument("dump")
    sp.add_argument("vaf")
    sp.add_argument("type", choices=sorted(TYPES))
    sp.add_argument("n", type=int)
    sp.set_defaults(fn=cmd_vec)

    sp = sub.add_parser("ptr", help="keep VAs pointing into [lo,hi)")
    sp.add_argument("dump")
    sp.add_argument("vaf")
    sp.add_argument("lo")
    sp.add_argument("hi")
    add_common(sp)
    sp.set_defaults(fn=cmd_ptr)

    sp = sub.add_parser("peek", help="print n values from a VA")
    sp.add_argument("dump")
    sp.add_argument("va")
    sp.add_argument("type", choices=sorted(TYPES))
    sp.add_argument("n", type=int)
    sp.set_defaults(fn=cmd_peek)

    sp = sub.add_parser("delta", help="VAs where value(B)-value(A) == dval")
    sp.add_argument("a", help="earlier dump")
    sp.add_argument("b", help="later dump")
    sp.add_argument("type", choices=sorted(TYPES))
    sp.add_argument("dval", help="expected difference, e.g. 50 or -25")
    add_common(sp)
    sp.set_defaults(fn=cmd_delta)

    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
