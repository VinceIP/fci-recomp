#!/usr/bin/env python3
"""predator_map.py -- convert a Predator (360) STFS map package to an
Evolution (Xbox) four-file map folder.

A Predator map is a single STFS `CON ` package whose one embedded file,
`FC2_Save`, bundles:
  - a UTF-16BE map-name header (@ 0x14),
  - a `DAT2` index (@ 0x8108) of **16-byte** little-endian entries
    `(offset, size, size, hash)` -- the third field repeats the size,
  - a `CRY\\x01` level (@ 0x9108) that the index tiles from offset 0.

An Evolution map is a folder of four loose files plus two Xbox save-metadata
files:

  <stem>.dat      the level, exactly 0x1F0000 bytes (the fixed Evolution map
                  size; the last FAT entry is 0x01-padded to fill it)
  <stem>.fat      a `DAT2` table of **12-byte** entries `(offset, size, hash)`,
                  hashes ascending, tiling the `.dat` from 0 to 0x1F0000
  <stem>.dat.sig  HMAC-SHA1 signature of the `.dat`  (40 bytes)
  <stem>.fat.sig  HMAC-SHA1 signature of the `.fat`  (40 bytes)
  SaveMeta.xbx    UTF-16LE "Name=<map name>" (168 bytes)
  SaveImage.xbx   Xbox save-image header (`XPR0`, 4096 bytes)

Signature scheme (confirmed byte-identical against the four Evolution
samples, see research/qwen/notes/b89-predator-maps.md):

    sig = HMAC-SHA1(K, HMAC-SHA1(K, file)) || HMAC-SHA1(K, file)

with K = 16 zero bytes (the zero-key XboxHDKey substitution from B88).

The 12-byte `.fat` writer is proven byte-identical against all three
Evolution samples in `tests/test_predator_map.py`.

Size rule (0x1F0000 cap, from `sub_00085891`,
build/lift/evolution/ige/gen/recomp_0012.c:38571, which computes
`0x1F0000 - sum(entries)` as the padding):
  - data < 0x1F0000  -> pad the `.dat` with 0x01 bytes and extend the last
    entry's size to reach 0x1F0000 (as the Evolution samples do);
  - data == 0x1F0000 -> unchanged;
  - data > 0x1F0000  -> **fail** naming both sizes, unless `--allow-oversize`
    is given (experiments only).

Usage:
  predator_map.py convert <package> <outdir> [--stem NAME] [--allow-oversize]
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs  # noqa: E402

# 16-byte zero key (the B88 XboxHDKey substitution).
ZERO_KEY = b"\x00" * 16

FC2_SAVE_NAME = "FC2_Save"

# The fixed Evolution map size. The `.dat` is always exactly this many bytes;
# the last FAT entry is 0x01-padded to fill it. Source: sub_00085891
# (build/lift/evolution/ige/gen/recomp_0012.c:38571) computes
# 0x1F0000 - sum(entries) as the padding.
MAP_SIZE = 0x1F0000

# The padding byte used to fill the last entry up to MAP_SIZE. The three
# Evolution samples all pad with 0x01 (not zero).
PAD_BYTE = 0x01


class MapError(ValueError):
    """Raised when a package cannot be converted."""


# ---------------------------------------------------------------------------
# FC2_Save parsing
# ---------------------------------------------------------------------------
def read_fc2_name(fc2: bytes) -> str:
    """Return the map name (UTF-16BE) from the `FC2_Save` header @ 0x14."""
    if len(fc2) < 0x14:
        raise MapError("FC2_Save too small for a name header")
    raw = fc2[0x14 : 0x14 + 0x100]
    end = raw.find(b"\x00\x00")
    if end < 0:
        end = len(raw)
    return raw[:end].decode("utf-16-be", "replace")


def find_cry(fc2: bytes) -> int:
    """Return the offset of the `CRY\\x01` level in `FC2_Save`."""
    i = fc2.find(b"CRY\x01")
    if i < 0:
        raise MapError("no CRY level section in FC2_Save")
    return i


# ---------------------------------------------------------------------------
# embedded index (16-byte entries)
# ---------------------------------------------------------------------------
def parse_embedded_index(fc2: bytes) -> tuple[list[tuple[int, int, int]], int]:
    """Parse and validate the 16-byte `DAT2` index embedded in `FC2_Save`.

    Returns ``(entries, end)`` where ``entries`` is a list of
    ``(offset, size, hash)`` (the redundant third size field dropped) sorted
    by offset, and ``end`` is the sum of the sizes (the tiling end).

    Validates: magic, count fits, sizes equal (field2 == field3), hashes
    ascending, tiling from 0 without gaps, and ``end`` inside the CRY region.
    """
    i = fc2.find(b"DAT2")
    if i < 0:
        raise MapError("no DAT2 index in FC2_Save")
    if fc2[i : i + 4] != b"DAT2":
        raise MapError("bad DAT2 magic")
    count = struct.unpack_from("<I", fc2, i + 4)[0]
    if count == 0:
        raise MapError("DAT2 index has zero entries")
    need = i + 8 + count * 16
    if need > len(fc2):
        raise MapError(f"DAT2 index runs past end of FC2_Save (need {need}, have {len(fc2)})")
    raw = [struct.unpack_from("<IIII", fc2, i + 8 + k * 16) for k in range(count)]
    # sizes equal (the third field repeats the size)
    for k, (_off, sz, sz2, _h) in enumerate(raw):
        if sz != sz2:
            raise MapError(f"DAT2 entry {k}: size fields differ ({sz:#x} != {sz2:#x})")
    # Keep the entries in their stored (hash-ascending) order: that is the
    # order the Evolution .fat uses. The (offset, size) fields still tile the
    # .dat; only the row order is by hash.
    entries = [(off, sz, h) for (off, sz, _sz2, h) in raw]
    # hashes ascending (as in the Evolution .fat)
    hashes = [h for (_o, _s, h) in entries]
    for k in range(len(hashes) - 1):
        if hashes[k] > hashes[k + 1]:
            raise MapError(f"DAT2 hashes not ascending at entry {k} ({hashes[k]:#x} > {hashes[k + 1]:#x})")
    # tiling from 0 without gaps (check on a copy sorted by offset)
    byoff = sorted(entries, key=lambda e: e[0])
    prev = 0
    for off, sz, _h in byoff:
        if off != prev:
            raise MapError(f"DAT2 tiling gap: entry at {off:#x} but expected {prev:#x}")
        prev = off + sz
    end = prev
    # end inside the CRY region
    cry = find_cry(fc2)
    if end > len(fc2) - cry:
        raise MapError(f"DAT2 index ends at {end:#x} past the CRY region ({len(fc2) - cry:#x} bytes)")
    return entries, end


# ---------------------------------------------------------------------------
# 12-byte .fat writer (the Evolution form)
# ---------------------------------------------------------------------------
def make_fat(entries: list[tuple[int, int, int]]) -> bytes:
    """Build the Evolution 12-byte `DAT2` fat from ``(offset, size, hash)``.

    Proven byte-identical against all three Evolution samples (see
    tests/test_predator_map.py).
    """
    out = bytearray()
    out += b"DAT2"
    out += struct.pack("<I", len(entries))
    for off, size, h in entries:
        out += struct.pack("<III", off, size, h)
    return bytes(out)


def parse_fat(raw: bytes) -> list[tuple[int, int, int]]:
    """Parse an Evolution 12-byte `DAT2` fat into ``(offset, size, hash)``."""
    if raw[:4] != b"DAT2":
        raise MapError("bad DAT2 magic in .fat")
    count = struct.unpack_from("<I", raw, 4)[0]
    if len(raw) < 8 + count * 12:
        raise MapError(f".fat is {len(raw)} bytes but holds {count} entries (needs {8 + count * 12})")
    return [struct.unpack_from("<III", raw, 8 + k * 12) for k in range(count)]


# ---------------------------------------------------------------------------
# size handling (the 0x1F0000 cap)
# ---------------------------------------------------------------------------
def fit_to_map_size(
    dat: bytes, entries: list[tuple[int, int, int]], allow_oversize: bool = False
) -> tuple[bytes, list[tuple[int, int, int]]]:
    """Pad ``dat`` with 0x01 to MAP_SIZE and extend the last entry to fill it.

    - ``dat`` == MAP_SIZE: returned unchanged.
    - ``dat`` <  MAP_SIZE: padded with 0x01 and the last entry extended so
      off+size == MAP_SIZE (as the Evolution samples do).
    - ``dat`` >  MAP_SIZE: returned unchanged when ``allow_oversize`` is set
      (the entries already tile it); otherwise an error is raised.
    """
    if len(dat) == MAP_SIZE:
        return dat, entries
    if len(dat) > MAP_SIZE:
        if allow_oversize:
            return dat, entries
        raise MapError(f"dat is {len(dat):#x} bytes, larger than the map size {MAP_SIZE:#x}")
    # Pad with 0x01 (the byte the Evolution samples use) up to MAP_SIZE.
    pad = MAP_SIZE - len(dat)
    new_dat = dat + bytes([PAD_BYTE]) * pad
    # Extend the entry that reaches the current end (highest offset) so
    # off+size == MAP_SIZE. Keep the hash row order unchanged.
    new_entries = list(entries)
    last_off = max(off for off, _sz, _h in entries)
    for k, (off, _sz, h) in enumerate(new_entries):
        if off == last_off:
            new_entries[k] = (off, MAP_SIZE - off, h)
            break
    return new_dat, new_entries


def check_size(end: int, allow_oversize: bool) -> None:
    """Refuse data larger than MAP_SIZE unless ``allow_oversize`` is set."""
    if end > MAP_SIZE and not allow_oversize:
        raise MapError(
            f"map data is {end:#x} bytes, larger than the Evolution map size {MAP_SIZE:#x}; "
            f"refusing to write an oversized map. Re-run with --allow-oversize to write it anyway "
            f"(experiments only)."
        )


# ---------------------------------------------------------------------------
# signatures
# ---------------------------------------------------------------------------
def make_sig(data: bytes) -> bytes:
    """Return the 40-byte map signature for ``data``.

    sig = HMAC-SHA1(K, HMAC-SHA1(K, data)) || HMAC-SHA1(K, data), K = 16
    zero bytes.
    """
    inner = hmac.new(ZERO_KEY, data, hashlib.sha1).digest()
    outer = hmac.new(ZERO_KEY, inner, hashlib.sha1).digest()
    return outer + inner


# ---------------------------------------------------------------------------
# Xbox save metadata
# ---------------------------------------------------------------------------
def make_save_meta(name: str) -> bytes:
    """Return the 168-byte `SaveMeta.xbx`: UTF-16LE "Name=<name>"."""
    text = f"Name={name}"
    raw = b"\xff\xfe" + text.encode("utf-16-le")
    pad = 168 - len(raw)
    if pad < 0:
        raise MapError(f"map name too long for SaveMeta.xbx: {name!r}")
    raw += b"\x20\x00" * (pad // 2)
    return raw


def make_save_image() -> bytes:
    """Return a 4096-byte `SaveImage.xbx` (`XPR0` Xbox save-image header)."""
    out = bytearray(4096)
    out[0:4] = b"XPR0"
    return bytes(out)


# ---------------------------------------------------------------------------
# conversion
# ---------------------------------------------------------------------------
def convert(package: str, outdir: str, stem: str | None = None, allow_oversize: bool = False) -> dict:
    """Convert one Predator STFS package to an Evolution map folder.

    Returns a dict with the map name, stem, dat size, and written file paths.
    """
    info = stfs.summarize(package)
    shift = info["shift"]
    fc2 = None
    for e in info["entries"]:
        if e["name"] == FC2_SAVE_NAME:
            fc2 = stfs.read_file(info["data"], e, shift)
            break
    if fc2 is None:
        raise MapError(f"no {FC2_SAVE_NAME} file in package")

    name = read_fc2_name(fc2)
    cry_off = find_cry(fc2)
    entries, end = parse_embedded_index(fc2)
    check_size(end, allow_oversize)

    # Slice the .dat from the CRY region (the index tiles it from 0).
    dat = fc2[cry_off : cry_off + end]
    dat, entries = fit_to_map_size(dat, entries, allow_oversize)
    fat = make_fat(entries)

    if stem is None:
        stem = os.path.splitext(os.path.basename(package))[0]

    dest = os.path.join(outdir, stem)
    os.makedirs(dest, exist_ok=True)

    files = {}
    for fn, buf in (
        (f"{stem}.dat", dat),
        (f"{stem}.fat", fat),
        (f"{stem}.dat.sig", make_sig(dat)),
        (f"{stem}.fat.sig", make_sig(fat)),
        ("SaveMeta.xbx", make_save_meta(name)),
        ("SaveImage.xbx", make_save_image()),
    ):
        path = os.path.join(dest, fn)
        with open(path, "wb") as f:
            f.write(buf)
        files[fn] = path

    return {"stem": stem, "name": name, "dat_size": len(dat), "entries": len(entries), "files": files}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Convert a Predator STFS map package to an Evolution map folder.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_conv = sub.add_parser("convert", help="convert one package to a map folder")
    p_conv.add_argument("package", help="path to the Predator STFS package")
    p_conv.add_argument("outdir", help="output directory (a <stem>/ folder is created inside)")
    p_conv.add_argument("--stem", help="override the map folder/file stem (default: package stem)")
    p_conv.add_argument(
        "--allow-oversize",
        action="store_true",
        help="write the folder even if the map data exceeds the 0x1F0000 Evolution size (experiments only)",
    )
    args = ap.parse_args(argv)
    if args.cmd == "convert":
        try:
            result = convert(args.package, args.outdir, args.stem, args.allow_oversize)
        except MapError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        print(f"map name : {result['name']!r}")
        print(f"stem     : {result['stem']}")
        print(f".dat     : {result['dat_size']} bytes ({result['entries']} FAT entries)")
        for fn, path in result["files"].items():
            print(f"  {fn:<16} -> {path}")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
