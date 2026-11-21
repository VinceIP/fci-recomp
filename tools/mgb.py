#!/usr/bin/env python3
"""mgb.py — parse and patch MAGMA .mgb menu files.

The .mgb format is a binary UI layout file used by the CryEngine Magma system.
This tool works at the byte level: it finds element records by their CRC-32 ID,
determines record boundaries by locating the next known element record start,
and inserts new records by copying a template.

Element record start pattern:
  +0x00  u32le  Element ID (CRC-32 of ASCII name)
  +0x04  u32le  0
  +0x08  u32le  Type (0x00000001, 0x00000002, or 0x00000004)
  +0x0C  ...    (ID high byte packed, varies)

Button record structure:
  +0x00  u32le  Element ID
  +0x04  ...    (fixed header, 178 bytes total to +0xB1)
  +0xAE  u8     Label character count
  +0xB2  utf16le Label (padded to 4-byte boundary)
  ...    ...    (variable trailing: "CryCommon", hashes, nav/child refs)
  -5     u8     Nav flag (0x01 = valid up-link)
  -4     u32le  Up-button ID (the button above this one in the nav chain)

Commands:
  dump FILE AREA_NAME     Print an area's elements
  insert FILE --area A --after B --template T --name N --label L --out OUT
  rename FILE --old NAME --new NAME --label LABEL --out OUT
"""

import argparse
import struct
import sys
import zlib


def crc32_name(name: str) -> int:
    """CRC-32 (IEEE) of the ASCII element name."""
    return zlib.crc32(name.encode("ascii")) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# known element names
# --------------------------------------------------------------------------

KNOWN_NAMES = [
    "B_TitleButton",
    "B_StoryMode",
    "B_SplitScreen",
    "B_Multiplayer",
    "B_SpecialFeatures",
    "B_Options",
    "B_Credits",
    "B_MapEditor",
    "B_Cheats",
    "B_CheatsDebug",
    "B_Quit",
    "B_Quit_to_Menu",
    "B_Resume",
    "B_LiveMenu",
    "B_CheatMenu",
    "B_CheatDebug",
    "B_Revert_Save",
    "B_Restart_Level",
    "B_LoadLast",
    "P_Main",
    "P_StartMenu",
    "P_MPStartMenu",
]

KNOWN_IDS: dict[str, int] = {name: crc32_name(name) for name in KNOWN_NAMES}


def name_for_id(element_id: int) -> str | None:
    """Look up a known element name from its ID."""
    for name, eid in KNOWN_IDS.items():
        if eid == element_id:
            return name
    return None


def find_id(data: bytes, element_id: int) -> int:
    """Find the first occurrence of a u32le element ID. Returns -1 if not found."""
    return data.find(struct.pack("<I", element_id))


def is_record_start(data: bytes, off: int, element_id: int) -> bool:
    """Check if data[off:] looks like a record start for the given element ID.

    A record start has: ID + header bytes. The header varies by element type:
      - Type 0x02: ID + 0x00000000 + 0x00000002 + ...
      - Type 0x04: ID + 0x00000000 + 0x00000004 + ...
      - Type 0x01: ID + 0x00000000 + 0x01010000 + ...
      - Variant:   ID + 0x00010000 + 0x00000002 + ...

    The key discriminator is: the ID is followed by bytes that are NOT another
    valid element ID. We check that the u32 at off+4 is small (< 0x10000) or 0,
    which rules out most false positives from packed ID references.
    """
    if off + 8 > len(data):
        return False
    if struct.unpack_from("<I", data, off)[0] != element_id:
        return False
    # The second u32 should be small (0, 1, or a type indicator)
    second = struct.unpack_from("<I", data, off + 4)[0]
    return second <= 0x10000


def find_record_start(data: bytes, element_id: int, search_from: int = 0) -> int:
    """Find the record start for an element ID. Returns -1 if not found."""
    cb = struct.pack("<I", element_id)
    start = search_from
    while True:
        idx = data.find(cb, start)
        if idx == -1:
            return -1
        if is_record_start(data, idx, element_id):
            return idx
        start = idx + 1


def find_area(data: bytes, area_name: str) -> tuple[int, int] | None:
    """Find an area by name. Returns (offset, element_count) or None.

    Areas are found by their first occurrence in the file (after the header).
    The area record is: ID (4 bytes) + count (4 bytes) + element records.
    """
    area_id = crc32_name(area_name)
    # Search after the MAGMA header (first 0x100 bytes)
    off = data.find(struct.pack("<I", area_id), 0x100)
    if off == -1:
        return None
    count = struct.unpack_from("<I", data, off + 4)[0]
    # Sanity check: count should be reasonable (1-100)
    if not (1 <= count <= 100):
        return None
    return (off, count)


def get_label(data: bytes, record_off: int) -> tuple[str, int]:
    """Read the label from a button record. Returns (label_text, char_count)."""
    char_count = data[record_off + 0xAE]
    if char_count == 0:
        return ("", 0)
    label_bytes = data[record_off + 0xB2 : record_off + 0xB2 + char_count * 2]
    return (label_bytes.decode("utf-16-le", errors="replace"), char_count)


def get_nav_link(data: bytes, record_off: int, record_size: int) -> tuple[int, int]:
    """Read the nav link from the last 6 bytes of a record.

    Structure: [u8 flag][u32le up_id][u8 trailing]
    flag=0x01 means valid up-link.
    Returns (up_id, flag).
    """
    end = record_off + record_size
    nav = data[end - 6 : end]
    flag = nav[0]
    up_id = struct.unpack_from("<I", nav, 1)[0]
    return (up_id, flag)


def set_nav_link(buf: bytearray, record_off: int, record_size: int, up_id: int, flag: int):
    """Write the nav link (last 6 bytes) of a record."""
    end = record_off + record_size
    buf[end - 6] = flag
    struct.pack_into("<I", buf, end - 5, up_id)
    buf[end - 1] = 0x01  # trailing flag


# Common element names to look for
COMMON_NAMES = KNOWN_NAMES


def find_all_records(data: bytes, names: list[str] | None = None) -> list[tuple[int, int, str]]:
    """Find all element record starts in the file. Returns [(offset, id, name), ...] sorted."""
    if names is None:
        names = COMMON_NAMES
    results = []
    for name in names:
        eid = crc32_name(name)
        off = find_record_start(data, eid)
        if off != -1:
            results.append((off, eid, name))
    results.sort()
    return results


def record_size(data: bytes, records: list[tuple[int, int, str]], idx: int) -> int:
    """Determine the record size of records[idx] by finding the next record start."""
    if idx + 1 < len(records):
        return records[idx + 1][0] - records[idx][0]
    return -1


def cmd_dump(args: argparse.Namespace):
    """Dump an area's elements."""
    with open(args.file, "rb") as f:
        data = f.read()

    area = find_area(data, args.area)
    if area is None:
        print(f"error: area '{args.area}' not found in {args.file}", file=sys.stderr)
        return 1

    area_off, count = area
    area_id = crc32_name(args.area)
    records = find_all_records(data)
    id_to_name = {eid: name for _, eid, name in records}

    # Filter to records in this area (after the area header)
    elem_start = area_off + 8
    area_records = [(off, eid, name) for off, eid, name in records if off >= elem_start]

    print(f"File: {args.file} ({len(data)} bytes)")
    print(f"Area: {args.area} (ID 0x{area_id:08x}) at 0x{area_off:x}, {count} elements")
    print(f"Known elements in area: {len(area_records)}")
    print()

    hdr = (
        f"{'#':>2s}  {'Offset':>6s}  {'Name':25s}  "
        f"{'ID':>10s}  {'Size':>5s}  {'Label':20s}  "
        f"{'Nav up':25s}  {'Flag':>4s}"
    )
    print(hdr)
    print("-" * 110)

    for i, (off, eid, name) in enumerate(area_records):
        rsize = record_size(data, area_records, i)
        label, _ = get_label(data, off)

        if 100 < rsize < 2000:
            up_id, flag = get_nav_link(data, off, rsize)
            up_name = id_to_name.get(up_id, f"0x{up_id:08x}")
            flag_str = f"{flag:4d}"
            size_str = f"{rsize:5d}"
        else:
            up_name = "?"
            flag_str = "   ?"
            size_str = f"{rsize:5d}" if rsize > 0 else "  ?"

        print(f"{i:2d}  0x{off:04x}  {name:25s}  0x{eid:08x}  {size_str}  {label:20s}  {up_name:25s}  {flag_str}")

    return 0


def cmd_insert(args: argparse.Namespace):
    """Insert a new button after a given element in an area."""
    with open(args.file, "rb") as f:
        data = bytearray(f.read())

    area = find_area(data, args.area)
    if area is None:
        print(f"error: area '{args.area}' not found", file=sys.stderr)
        return 1

    area_off, count = area
    new_id = crc32_name(args.name)
    template_id = crc32_name(args.template)
    after_id = crc32_name(args.after)

    # Find all records to determine boundaries
    all_records = find_all_records(data)

    # Find the template record
    template_off = find_record_start(data, template_id)
    if template_off == -1:
        print(f"error: template '{args.template}' not found", file=sys.stderr)
        return 1

    # Find the 'after' element
    after_off = find_record_start(data, after_id)
    if after_off == -1:
        print(f"error: element '{args.after}' not found", file=sys.stderr)
        return 1

    # Determine record sizes by finding the next record after each
    def next_record_after(off: int) -> int:
        """Find the next record start after the given offset."""
        best = -1
        for roff, _, _ in all_records:
            if roff > off and (best == -1 or roff < best):
                best = roff
        return best

    template_next = next_record_after(template_off)
    after_next = next_record_after(after_off)

    if template_next == -1:
        print("error: cannot determine template record size (no next element)", file=sys.stderr)
        return 1
    if after_next == -1:
        print(f"error: cannot determine '{args.after}' record size (no next element)", file=sys.stderr)
        return 1

    template_rec_size = template_next - template_off
    after_rec_size = after_next - after_off

    # Sanity check
    if not (150 < template_rec_size < 2000):
        print(f"error: template record size {template_rec_size} is unreasonable", file=sys.stderr)
        return 1
    if not (150 < after_rec_size < 2000):
        print(f"error: '{args.after}' record size {after_rec_size} is unreasonable", file=sys.stderr)
        return 1

    # Create the new record: copy of template
    new_record = bytearray(data[template_off : template_off + template_rec_size])

    # 1. Set the element ID
    struct.pack_into("<I", new_record, 0, new_id)
    # ID is also embedded at +0x0C (high byte) and +0x10 (low 3 bytes)
    struct.pack_into("<I", new_record, 0x0C, (new_id & 0xFF) << 24)
    struct.pack_into("<I", new_record, 0x10, (new_id >> 8) & 0x00FFFFFF)

    # 2. Set the label
    label = args.label.encode("utf-16-le")
    char_count = len(label) // 2
    old_char_count = new_record[0xAE]
    old_label_padded = ((old_char_count * 2 + 3) // 4) * 4
    new_label_padded = ((len(label) + 3) // 4) * 4

    if new_label_padded > old_label_padded:
        print(
            f"error: new label ({new_label_padded} bytes) exceeds template label "
            f"({old_label_padded} bytes). Use a longer template or shorter label.",
            file=sys.stderr,
        )
        return 1

    label_padded = label + b"\x00" * (old_label_padded - len(label))
    new_record[0xAE] = char_count
    new_record[0xB2 : 0xB2 + old_label_padded] = label_padded

    # 3. Set the nav link: up = the 'after' element
    set_nav_link(new_record, 0, template_rec_size, after_id, 0x01)

    # Insert the new record after the 'after' element
    insert_point = after_off + after_rec_size
    data[insert_point:insert_point] = bytes(new_record)

    # 4. Update the nav link of the element that was after 'after'
    # That element (after_next) now starts at insert_point + len(new_record)
    # because we inserted before it. Its record size is the gap to the record
    # after it. In the original file, after_next was at `after_next` and the
    # record after it was at `after_next_next`. After insertion, both are
    # shifted by len(new_record).
    next_off = insert_point + len(new_record)
    # Find the record after after_next in the original file
    after_next_next = next_record_after(after_next)
    if after_next_next != -1:
        # After insertion, after_next_next is shifted by len(new_record)
        next_rec_size = (after_next_next + len(new_record)) - next_off
        if 100 < next_rec_size < 2000:
            set_nav_link(data, next_off, next_rec_size, new_id, 0x01)
            print(f"  Updated nav link of element at 0x{next_off:x} → {args.name}")
        else:
            print(f"  warning: next element record size {next_rec_size} unreasonable", file=sys.stderr)
    else:
        print(f"  warning: no record after '{args.after}''s successor; cannot update nav link", file=sys.stderr)

    # 5. Update the area's element count
    struct.pack_into("<I", data, area_off + 4, count + 1)

    # Write the output
    with open(args.out, "wb") as f:
        f.write(data)

    print(f"Inserted {args.name} (0x{new_id:08x}) after {args.after} in {args.area}")
    print(f"  Template: {args.template} at 0x{template_off:x} (size {template_rec_size})")
    print(f"  After:    {args.after} at 0x{after_off:x} (size {after_rec_size})")
    print(f"  Insert at: 0x{insert_point:x}")
    print(f"  Label: {args.label!r} ({char_count} chars)")
    print(f"  Area count: {count} → {count + 1}")
    print(f"  Output: {args.out} ({len(data)} bytes)")
    return 0


def cmd_rename(args: argparse.Namespace):
    """Rename an existing element: change its ID and label in place.

    This is safer than insert because the record structure is already valid.
    The ID is replaced everywhere it appears in the record (as a u32le),
    and the label is updated if the new label fits in the existing space.
    Nav links in other records that reference the old ID are also updated.
    """
    with open(args.file, "rb") as f:
        data = bytearray(f.read())

    old_id = crc32_name(args.old)
    new_id = crc32_name(args.new)

    # Find the record start
    record_off = find_record_start(data, old_id)
    if record_off == -1:
        print(f"error: element '{args.old}' (0x{old_id:08x}) not found", file=sys.stderr)
        return 1

    # Determine record size: find the next known record after this one.
    # If there is no next record, use the end of the data as the boundary.
    all_records = find_all_records(data)
    next_off = -1
    for roff, _, _ in all_records:
        if roff > record_off and (next_off == -1 or roff < next_off):
            next_off = roff
    if next_off == -1:
        next_off = len(data)
    record_size = next_off - record_off

    # 1. Replace the old ID with the new ID everywhere in the data.
    # This handles both the record's own ID fields and any nav links in
    # other records that reference the old ID.
    old_id_bytes = struct.pack("<I", old_id)
    new_id_bytes = struct.pack("<I", new_id)
    count = data.count(old_id_bytes)
    if count == 0:
        print(f"error: old ID 0x{old_id:08x} not found in data", file=sys.stderr)
        return 1
    data = data.replace(old_id_bytes, new_id_bytes)

    # 2. Update the label if provided
    old_label = ""
    if args.label:
        old_label, old_char_count = get_label(data, record_off)
        new_label_bytes = args.label.encode("utf-16-le")
        new_char_count = len(new_label_bytes) // 2
        old_label_padded = ((old_char_count * 2 + 3) // 4) * 4
        new_label_padded = ((len(new_label_bytes) + 3) // 4) * 4

        if new_label_padded > old_label_padded:
            print(
                f"error: new label '{args.label}' ({new_label_padded} bytes) exceeds "
                f"existing label '{old_label}' ({old_label_padded} bytes)",
                file=sys.stderr,
            )
            return 1

        label_padded = new_label_bytes + b"\x00" * (old_label_padded - len(new_label_bytes))
        data[record_off + 0xAE] = new_char_count
        data[record_off + 0xB2 : record_off + 0xB2 + old_label_padded] = label_padded

    # Write the output
    with open(args.out, "wb") as f:
        f.write(data)

    print(f"Renamed {args.old} (0x{old_id:08x}) → {args.new} (0x{new_id:08x})")
    print(f"  Record at 0x{record_off:x} (size {record_size})")
    print(f"  ID occurrences replaced: {count}")
    if args.label:
        print(f"  Label: '{old_label}' → '{args.label}'")
    print(f"  Output: {args.out} ({len(data)} bytes)")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Parse and patch MAGMA .mgb files")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_dump = sub.add_parser("dump", help="Dump an area's elements")
    p_dump.add_argument("file", help="Path to .mgb file")
    p_dump.add_argument("area", help="Area name (e.g. P_Main)")
    p_dump.set_defaults(func=cmd_dump)

    p_ins = sub.add_parser("insert", help="Insert a new button")
    p_ins.add_argument("file", help="Path to .mgb file")
    p_ins.add_argument("--area", required=True, help="Area name")
    p_ins.add_argument("--after", required=True, help="Element to insert after")
    p_ins.add_argument("--template", required=True, help="Template element to copy")
    p_ins.add_argument("--name", required=True, help="New element name")
    p_ins.add_argument("--label", required=True, help="Display label")
    p_ins.add_argument("--out", required=True, help="Output file")
    p_ins.set_defaults(func=cmd_insert)

    p_ren = sub.add_parser("rename", help="Rename an existing element in place")
    p_ren.add_argument("file", help="Path to .mgb file")
    p_ren.add_argument("--old", required=True, help="Current element name")
    p_ren.add_argument("--new", required=True, help="New element name")
    p_ren.add_argument("--label", default=None, help="New display label (optional)")
    p_ren.add_argument("--out", required=True, help="Output file")
    p_ren.set_defaults(func=cmd_rename)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
