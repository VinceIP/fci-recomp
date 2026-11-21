"""Read-only extractor for Xbox 360 STFS packages (CON / LIVE / PIRS).

STFS (Standard Title File System) is the container the 360 uses for saved
games. Far Cry Instincts Predator "user maps" are STFS CON packages: content
type 1 (Saved Game), title id 0x555307DC. This reads the publicly documented
layout (free60 wiki "STFS") as used by those save packages.

Block layout (the part that trips people up)
--------------------------------------------
The volume is a sequence of 4 KiB blocks. Block 0 of the *file space* (the
offset base 0xC000) is the file table; block 1 is the first data block. The
file space is NOT contiguous in the file: a hash-table block is interleaved
after every 170 (0xAA) data blocks, and a level-1 table after every
170*170 (0x70E4) blocks. The mapping from a logical block number ``i`` (as
stored in the file table) to its physical file offset is the standard STFS
``fix_blocknum``:

    phys(i) = 0xC000 + (i + sep(i)) * 0x1000
    sep(i)  = ((i // 0xAA)   + 1) << shift   if i >= 0xAA
             + ((i // 0x70E4) + 1) << shift   if i >= 0x70E4

where ``shift`` (0 or 1) is the *table size shift*: each hash-table block
occupies ``1 << shift`` blocks. It is derived from the header "Size" field
(0x340): ``shift = 0`` if ``((size + 0xFFF) & 0xF000) >> 0xC == 0xB`` else
``1``. The Predator map packages use ``shift = 1`` (two-block tables), so
their data is interleaved with a hash block every 170 blocks and is not a
simple contiguous run.

The level-0 hash table for block ``i`` lives at

    tablenum = (i // 0xAA) * spacing
               + (i // 0x70E4 + 1) << shift   if i >= 0xAA
               + 1 << shift                    if i >= 0x70E4
               - (1 << shift)
    phys     = 0xC000 + tablenum * 0x1000

with ``spacing = 0xAB + shift`` (171 or 172), and record ``i % 0xAA`` in that
table holds the SHA1 of block ``i``. ``verify_hashes`` checks every block
against its record; any mismatch is an error.

Header / file table
-------------------
    0x0000  header: magic, content type, title id, display name (UTF-16BE)
    0x0379  volume descriptor: block separation, file-table location,
            total allocated / unallocated block counts
    0xC000  file table (block 0): 64-byte entries (40-byte name, flags,
            block count, starting block, file size, timestamps) until a
            null name

Endianness is big-endian for the header, volume descriptor and file-size
field; little-endian for the file-table cluster and block-count fields.

Usage:
    python tools/stfs.py list <package>
    python tools/stfs.py extract <package> <outdir>

Read-only: the package is never modified. Extracted files are written under
``<outdir>/<package-stem>/``.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

BLOCK = 0x1000  # one STFS block / cluster
MAGICS = {b"CON ": "CON", b"LIVE": "LIVE", b"PIRS": "PIRS"}
HEADER_FIELDS = 0x340  # "Size" field offset (header size, e.g. 0x971A)
VD_OFFSET = 0x379  # volume descriptor
FT_OFFSET = 0xC000  # file table = block 0 of the file space
FT_ENTRY = 64  # one file-table entry
FT_NAME = 40  # name length inside an entry
GROUP0 = 0xAA  # 170: blocks per level-0 group
GROUP1 = 0x70E4  # 28900: blocks per level-1 group


class StfsError(ValueError):
    """Raised when a package is not a readable STFS volume."""


def _u32be(b, off):
    return struct.unpack_from(">I", b, off)[0]


def _i24le(b, off):
    return int.from_bytes(b[off : off + 3], "little")


def read_header(data: bytes) -> dict:
    """Parse the STFS header (magic, content type, title id, display name)."""
    if len(data) < 0x411:
        raise StfsError("file too small for an STFS header")
    magic = data[:4]
    if magic not in MAGICS:
        raise StfsError(f"bad STFS magic {magic!r}")
    display = data[0x411 : 0x411 + 0x900].decode("utf-16-be", "replace")
    return {
        "magic": MAGICS[magic],
        "header_size": _u32be(data, HEADER_FIELDS),
        "content_type": struct.unpack_from(">i", data, 0x344)[0],
        "content_size": struct.unpack_from(">q", data, 0x34C)[0],
        "title_id": _u32be(data, 0x360),
        "platform": data[0x364],
        "console_id": data[0x36C:0x371],
        "profile_id": data[0x371:0x379],
        "display_name": display.rstrip("\x00"),
    }


def read_volume_descriptor(data: bytes) -> dict:
    """Parse the volume descriptor at 0x379 (24 bytes)."""
    vd = data[VD_OFFSET : VD_OFFSET + 0x24]
    return {
        "block_separation": vd[0x02],
        "ft_block_count": struct.unpack_from(">h", vd, 0x03)[0],
        "ft_block_number": int.from_bytes(vd[0x05:0x08], "big"),
        "total_allocated": struct.unpack_from(">i", vd, 0x1C)[0],
        "total_unallocated": struct.unpack_from(">i", vd, 0x20)[0],
    }


def table_size_shift(header_size: int) -> int:
    """The table size shift (0 or 1): each hash-table block is 1<<shift blocks.

    Derived from the header "Size" field the way the Xbox firmware does it.
    """
    return 0 if (((header_size + 0xFFF) & 0xF000) >> 0xC) == 0xB else 1


def fix_blocknum(i: int, shift: int) -> int:
    """Map a logical block number to its position in the file space.

    Adds the separation for the interleaved hash-table blocks (level 0 every
    170 blocks, level 1 every 170*170 blocks), each table being 1<<shift
    blocks wide.
    """
    sep = 0
    if i >= GROUP0:
        sep += ((i // GROUP0) + 1) << shift
    if i >= GROUP1:
        sep += ((i // GROUP1) + 1) << shift
    return i + sep


def block_offset(i: int, shift: int) -> int:
    """Physical file offset of logical block ``i``."""
    return FT_OFFSET + fix_blocknum(i, shift) * BLOCK


def l0_table_offset(i: int, shift: int) -> int:
    """Physical file offset of the level-0 hash table that covers block ``i``."""
    spacing = 0xAB + shift  # 171 (shift 0) or 172 (shift 1)
    tablenum = (i // GROUP0) * spacing
    if i >= GROUP0:
        tablenum += (i // GROUP1 + 1) << shift
        if i >= GROUP1:
            tablenum += 1 << shift
    tablenum -= 1 << shift
    return FT_OFFSET + tablenum * BLOCK


def read_file_table(data: bytes, ft_offset: int = FT_OFFSET) -> list[dict]:
    """Parse file-table entries at ``ft_offset`` until the first null name."""
    entries = []
    for i in range((len(data) - ft_offset) // FT_ENTRY):
        off = ft_offset + i * FT_ENTRY
        e = data[off : off + FT_ENTRY]
        if len(e) < FT_ENTRY:
            break
        flen = e[0x28] & 0x3F
        name = e[:flen].decode("utf-8", "replace") if flen else ""
        if not name:
            break
        entries.append(
            {
                "name": name,
                "flags": e[0x28],
                "block_count": _i24le(e, 0x29),
                "start_block": _i24le(e, 0x2F),
                "size": _u32be(e, 0x34),
                "mtime": struct.unpack_from("<I", e, 0x38)[0],
            }
        )
    return entries


def read_file(data: bytes, entry: dict, shift: int) -> bytes:
    """Return the payload bytes for one file-table entry (non-contiguous blocks)."""
    out = bytearray()
    sb = entry["start_block"]
    total = entry["block_count"]
    size = entry["size"]
    for b in range(total):
        off = block_offset(sb + b, shift)
        if off + BLOCK > len(data):
            raise StfsError(f"file {entry['name']!r} block {b} (cluster {sb + b}) runs past end of package")
        out += data[off : off + BLOCK]
    return bytes(out[:size])


def verify_hashes(data: bytes, entries: list[dict], shift: int) -> list[dict]:
    """Integrity check: verify EVERY file block against its level-0 hash record.

    For each block of each file, the level-0 table's record holds the SHA1 of
    that block. Any mismatch, missing record, or out-of-range table is an
    error. Returns per-file {name, ok, checked, total} rows.
    """
    rows = []
    for entry in entries:
        sb = entry["start_block"]
        total = entry["block_count"]
        ok = True
        checked = 0
        for b in range(total):
            cluster = sb + b
            t_off = l0_table_offset(cluster, shift)
            rec_off = t_off + (cluster % GROUP0) * 24
            if rec_off + 24 > len(data):
                ok = False
                break
            status = data[rec_off + 20]
            if status == 0xFF:
                ok = False
                break
            claimed = data[rec_off : rec_off + 20]
            off = block_offset(cluster, shift)
            actual = hashlib.sha1(data[off : off + BLOCK]).digest()
            if claimed != actual:
                ok = False
                break
            checked += 1
        rows.append({"name": entry["name"], "ok": ok, "checked": checked, "total": total})
    return rows


def summarize(path: str) -> dict:
    """Read a package and return header + volume descriptor + file table."""
    with open(path, "rb") as f:
        data = f.read()
    hdr = read_header(data)
    vd = read_volume_descriptor(data)
    entries = read_file_table(data)
    return {
        "path": path,
        "data": data,
        "header": hdr,
        "volume": vd,
        "entries": entries,
        "shift": table_size_shift(hdr["header_size"]),
    }


def cmd_list(path: str) -> int:
    info = summarize(path)
    h, v = info["header"], info["volume"]
    print(f"STFS {h['magic']} package: {os.path.basename(path)}")
    print(f"  title id    : {h['title_id']:08X}")
    print(f"  content type: {h['content_type']}")
    print(f"  display name: {h['display_name']!r}")
    print(f"  header size : 0x{h['header_size']:X}  (table size shift {info['shift']})")
    print(
        f"  volume      : allocated={v['total_allocated']} "
        f"unallocated={v['total_unallocated']} "
        f"ft_blocks={v['ft_block_count']} sep={v['block_separation']}"
    )
    print(f"  files ({len(info['entries'])}):")
    for e in info["entries"]:
        print(
            f"    {e['name']!r}  size=0x{e['size']:X} "
            f"blocks={e['block_count']} start={e['start_block']} "
            f"flags=0x{e['flags']:02X}"
        )
    return 0


def cmd_extract(path: str, outdir: str) -> int:
    info = summarize(path)
    shift = info["shift"]
    stem = os.path.splitext(os.path.basename(path))[0]
    dest = os.path.join(outdir, stem)
    os.makedirs(dest, exist_ok=True)
    for e in info["entries"]:
        payload = read_file(info["data"], e, shift)
        out = os.path.join(dest, e["name"])
        with open(out, "wb") as f:
            f.write(payload)
        print(f"  {e['name']}  0x{e['size']:X} -> {out}")
    rows = verify_hashes(info["data"], info["entries"], shift)
    for r in rows:
        state = "ok" if r["ok"] else "MISMATCH"
        print(f"  hash check {r['name']!r}: {state} ({r['checked']}/{r['total']} blocks)")
        if not r["ok"]:
            return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Read-only Xbox 360 STFS package extractor.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_list = sub.add_parser("list", help="print header + file table")
    p_list.add_argument("package")
    p_ext = sub.add_parser("extract", help="write file payloads to outdir")
    p_ext.add_argument("package")
    p_ext.add_argument("outdir")
    args = ap.parse_args(argv)
    if args.cmd == "list":
        return cmd_list(args.package)
    return cmd_extract(args.package, args.outdir)


if __name__ == "__main__":
    sys.exit(main())
