#!/usr/bin/env python3
"""menu_items.py — apply menu item patches to a game install folder.

This tool takes a game install folder (with .fat/.dat pairs and .mgb files),
applies all configured menu item patches, and writes the patched files to an
output folder.

The approach is to **rename** an existing element rather than inserting a new
one. This is safer because the record structure is already valid — the game
loader has already proven it can parse the record. We only change the element
ID (CRC-32) and the display label.

For the main menu, we rename B_CheatsDebug (which is hidden at runtime by the
game code) to B_Quit. The game code looks for B_CheatsDebug by name to hide
it; after the rename, it won't find B_CheatsDebug, so B_Quit stays visible.

Menu items are defined in a table below. Each item specifies:
  - rename_from: the existing element to rename
  - label: the new display text
  - mgb_file: which .mgb file to patch (path within the DAT2 package)
  - stringtable: which string table XML to patch
  - string_section: which section in the string table

Usage:
  menu_items.py <install_folder> <output_folder>
  menu_items.py <install_folder> <output_folder> --item B_Quit
"""

import argparse
import os
import struct
import sys

# Add the tools directory to the path so we can import sibling modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dat2 import Fat, name_hash
from mgb import (
    crc32_name,
    find_all_records,
    find_record_start,
    get_label,
)
from stringtable import rename_string_row

# --------------------------------------------------------------------------
# Menu item table
# --------------------------------------------------------------------------

MENU_ITEMS = {
    "B_Quit": {
        "rename_from": "B_CheatsDebug",
        "label": "QUIT",
        "mgb_file": "magma_crypregame.mgb",
        "area": "P_Main",
        "stringtable": "stringtable-mainmenu-eng.xml",
        "string_section": "P_Main",
        # The DAT2 path for the .mgb file (used to locate it in common.dat)
        "dat2_name": "magma/crypregame.mgb",
    },
    # Future items:
    # "B_Quit_to_Windows": {
    #     "rename_from": "B_CheatDebug",
    #     "label": "QUIT TO WINDOWS",
    #     "mgb_file": "magma_cryingame.mgb",
    #     "area": "P_StartMenu",
    #     "stringtable": "stringtable-ingamemenu-eng.xml",
    #     "string_section": "P_StartMenu",
    #     "dat2_name": "magma/cryingame.mgb",
    # },
}


# --------------------------------------------------------------------------
# .mgb patching (rename approach)
# --------------------------------------------------------------------------


def patch_mgb_rename(data: bytes, old_name: str, new_name: str, label: str | None) -> tuple[bytes, list[str]]:
    """Rename an element in .mgb data. Returns (patched_data, list_of_changes)."""
    data = bytearray(data)
    changes = []

    old_id = crc32_name(old_name)
    new_id = crc32_name(new_name)

    # Find the record start
    record_off = find_record_start(data, old_id)
    if record_off == -1:
        raise ValueError(f"element '{old_name}' (0x{old_id:08x}) not found")

    # Determine record size: find the next known record after this one.
    # If there is no next record, use the end of the data as the boundary.
    all_records = find_all_records(data)
    next_off = -1
    for roff, _, _ in all_records:
        if roff > record_off and (next_off == -1 or roff < next_off):
            next_off = roff
    if next_off == -1:
        next_off = len(data)

    # 1. Replace the old ID with the new ID everywhere in the data.
    # This handles both the record's own ID fields and any nav links in
    # other records that reference the old ID.
    old_id_bytes = struct.pack("<I", old_id)
    new_id_bytes = struct.pack("<I", new_id)
    count = data.count(old_id_bytes)
    if count == 0:
        raise ValueError(f"old ID 0x{old_id:08x} not found in data")
    data = data.replace(old_id_bytes, new_id_bytes)
    changes.append(f"replaced {count} ID occurrence(s) (record + nav links)")

    # 2. Update the label
    if label:
        old_label, old_char_count = get_label(data, record_off)
        new_label_bytes = label.encode("utf-16-le")
        new_char_count = len(new_label_bytes) // 2
        old_label_padded = ((old_char_count * 2 + 3) // 4) * 4
        new_label_padded = ((len(new_label_bytes) + 3) // 4) * 4

        if new_label_padded > old_label_padded:
            raise ValueError(
                f"label '{label}' ({new_label_padded} bytes) exceeds "
                f"existing label '{old_label}' ({old_label_padded} bytes)"
            )

        label_padded = new_label_bytes + b"\x00" * (old_label_padded - len(new_label_bytes))
        data[record_off + 0xAE] = new_char_count
        data[record_off + 0xB2 : record_off + 0xB2 + old_label_padded] = label_padded
        changes.append(f"label: '{old_label}' → '{label}'")

    # Note: nav links in other records that reference the old ID are already
    # updated by the global byte replacement in step 1.

    return bytes(data), changes


# --------------------------------------------------------------------------
# DAT2 package helpers
# --------------------------------------------------------------------------


def find_dat2_pair(install_dir: str) -> tuple[str, str] | None:
    """Find the common.fat/common.dat pair in the install directory."""
    for root, _dirs, files in os.walk(install_dir):
        lower_files = {f.lower(): f for f in files}
        if "common.fat" in lower_files and "common.dat" in lower_files:
            fat_path = os.path.join(root, lower_files["common.fat"])
            dat_path = os.path.join(root, lower_files["common.dat"])
            return fat_path, dat_path
    return None


def extract_from_dat2(fat_path: str, dat_path: str, name: str) -> tuple[bytes, int] | None:
    """Extract a file from a DAT2 package by name. Returns (data, entry_index) or None."""
    fat = Fat(fat_path, dat_path)
    h = name_hash(name)
    for i, (_off, _size, entry_hash) in enumerate(fat.entries):
        if entry_hash == h:
            return fat.payload(i), i
    return None


def find_stringtable_in_dat2(fat_path: str, dat_path: str, section: str, old_enum: str) -> tuple[int, bytes] | None:
    """Find a string table entry in a DAT2 package that contains the given section and enum.

    Returns (entry_index, data) or None if not found.
    """
    fat = Fat(fat_path, dat_path)
    section_bytes = section.encode("utf-8")
    enum_bytes = old_enum.encode("utf-8")
    for i, (_off, _size, _h) in enumerate(fat.entries):
        data = fat.payload(i)
        if data[:20].startswith(b"<stringtable") and section_bytes in data and enum_bytes in data:
            return i, data
    return None


def write_dat2_output(
    fat_path: str,
    dat_path: str,
    output_dir: str,
    replacements: dict[int, bytes],
) -> list[str]:
    """Write a repacked DAT2 pair to the output directory.

    replacements: dict mapping entry index → new data bytes.
    Returns a list of change descriptions.
    """
    fat = Fat(fat_path, dat_path)
    changes = []

    # Build the new entry list with replacements
    new_entries = []
    for i, (_off, size, hash_val) in enumerate(fat.entries):
        if i in replacements:
            new_data = replacements[i]
            changes.append(f"entry {i} (hash 0x{hash_val:08x}): {size} → {len(new_data)} bytes")
            new_entries.append((hash_val, new_data))
        else:
            new_entries.append((hash_val, fat.payload(i)))

    # Sort by hash (DAT2 requirement)
    new_entries.sort(key=lambda e: e[0])

    # Build the new .dat file
    dat_parts = []
    fat_entries = []
    offset = 0
    for hash_val, data in new_entries:
        # 4-byte alignment
        padding = (4 - (offset % 4)) % 4
        if padding:
            dat_parts.append(b"\x00" * padding)
            offset += padding
        fat_entries.append((offset, len(data), hash_val))
        dat_parts.append(data)
        offset += len(data)

    dat_data = b"".join(dat_parts)

    # Write the .dat file
    out_dat = os.path.join(output_dir, "common.dat")
    os.makedirs(os.path.dirname(out_dat) if os.path.dirname(out_dat) else ".", exist_ok=True)
    with open(out_dat, "wb") as f:
        f.write(dat_data)

    # Write the .fat file
    out_fat = os.path.join(output_dir, "common.fat")
    fat_data = struct.pack("<4sI", b"DAT2", len(fat_entries))
    for off, size, hash_val in fat_entries:
        fat_data += struct.pack("<III", off, size, hash_val)
    with open(out_fat, "wb") as f:
        f.write(fat_data)

    changes.append(f"common.dat: {os.path.getsize(dat_path)} → {len(dat_data)} bytes")
    changes.append(f"common.fat: {len(fat_entries)} entries")
    return changes


# --------------------------------------------------------------------------
# Patch application
# --------------------------------------------------------------------------


def apply_patches(install_dir: str, output_dir: str, item_names: list[str]) -> list[str]:
    """Apply all specified menu item patches. Returns a list of changes."""
    all_changes = []
    os.makedirs(output_dir, exist_ok=True)

    # Collect all .mgb replacements
    mgb_replacements: dict[str, bytes] = {}
    stringtable_patches: list[tuple[str, str, str, str, str]] = []

    for item_name in item_names:
        if item_name not in MENU_ITEMS:
            print(f"warning: unknown menu item '{item_name}', skipping", file=sys.stderr)
            continue

        item = MENU_ITEMS[item_name]
        old_name = item["rename_from"]
        new_name = item_name
        label = item["label"]
        mgb_file = item["mgb_file"]
        dat2_name = item.get("dat2_name", mgb_file)

        # 1. Find the DAT2 pair and extract the .mgb
        dat2_pair = find_dat2_pair(install_dir)
        mgb_entry_idx = None
        if dat2_pair is not None:
            result = extract_from_dat2(dat2_pair[0], dat2_pair[1], dat2_name)
            if result is None:
                result = extract_from_dat2(dat2_pair[0], dat2_pair[1], mgb_file)
            if result is None:
                print(f"warning: {dat2_name} not found in DAT2 package", file=sys.stderr)
                continue
            mgb_data, mgb_entry_idx = result
        else:
            print(f"warning: common.fat/common.dat not found in {install_dir}", file=sys.stderr)
            mgb_path = _find_file(install_dir, mgb_file)
            if mgb_path is None:
                print(f"warning: {mgb_file} not found", file=sys.stderr)
                continue
            with open(mgb_path, "rb") as f:
                mgb_data = f.read()

        # 2. Apply the rename patch
        try:
            patched_mgb, mgb_changes = patch_mgb_rename(mgb_data, old_name, new_name, label)
        except ValueError as e:
            print(f"error: {mgb_file}: {e}", file=sys.stderr)
            continue

        if mgb_entry_idx is not None:
            mgb_replacements[mgb_entry_idx] = patched_mgb
        else:
            mgb_replacements["loose:" + mgb_file] = patched_mgb
        for c in mgb_changes:
            all_changes.append(f"mgb {mgb_file}: {c}")

        # 3. String table patch: look for loose file first, then in DAT2
        st_name = item["stringtable"]
        st_path = _find_file(install_dir, st_name)
        if st_path is not None:
            stringtable_patches.append((st_path, st_name, item["string_section"], old_name, new_name, label))
        else:
            # Try to find the string table in the DAT2 package
            dat2_pair = find_dat2_pair(install_dir)
            if dat2_pair is not None:
                st_result = find_stringtable_in_dat2(dat2_pair[0], dat2_pair[1], item["string_section"], old_name)
                if st_result is not None:
                    st_idx, st_data = st_result
                    # Patch the string table data
                    st_text = st_data.decode("utf-8")
                    st_patched = rename_string_row(st_text, item["string_section"], old_name, new_name, label)
                    st_patched_bytes = st_patched.encode("utf-8")
                    mgb_replacements[st_idx] = st_patched_bytes
                    all_changes.append(f"stringtable in DAT2 (entry {st_idx}): {old_name} → {new_name}")
                else:
                    print(f"warning: {st_name} not found in DAT2 package", file=sys.stderr)
            else:
                print(f"warning: {st_name} not found in {install_dir}", file=sys.stderr)

    # Write DAT2 output if we have .mgb replacements
    if mgb_replacements:
        dat2_pair = find_dat2_pair(install_dir)
        if dat2_pair is not None:
            # Separate index-based and loose replacements
            idx_replacements = {k: v for k, v in mgb_replacements.items() if isinstance(k, int)}
            if idx_replacements:
                dat2_changes = write_dat2_output(dat2_pair[0], dat2_pair[1], output_dir, idx_replacements)
                all_changes.extend(dat2_changes)
        # Write any loose .mgb files
        for key, data in mgb_replacements.items():
            if isinstance(key, str) and key.startswith("loose:"):
                name = key[6:]
                out_path = os.path.join(output_dir, os.path.basename(name))
                os.makedirs(os.path.dirname(out_path) if os.path.dirname(out_path) else ".", exist_ok=True)
                with open(out_path, "wb") as f:
                    f.write(data)
                all_changes.append(f"wrote {os.path.basename(name)} ({len(data)} bytes)")

    # Write string table patches
    for st_path, st_name, section, old_enum, new_enum, label in stringtable_patches:
        with open(st_path, "r", encoding="utf-8") as f:
            text = f.read()
        patched = rename_string_row(text, section, old_enum, new_enum, label)
        out_st = os.path.join(output_dir, st_name)
        os.makedirs(os.path.dirname(out_st) if os.path.dirname(out_st) else ".", exist_ok=True)
        with open(out_st, "w", encoding="utf-8", newline="") as f:
            f.write(patched)
        all_changes.append(f"stringtable: {st_name} ({old_enum} → {new_enum})")

    return all_changes


def _find_file(directory: str, filename: str) -> str | None:
    """Find a file by name in the directory tree (case-insensitive)."""
    target = filename.lower()
    for root, _dirs, files in os.walk(directory):
        for f in files:
            if f.lower() == target:
                return os.path.join(root, f)
    return None


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="Apply menu item patches")
    parser.add_argument("install_dir", help="Game install folder")
    parser.add_argument("output_dir", help="Output folder for patched files")
    parser.add_argument("--item", action="append", help="Specific item to apply (default: all)")
    args = parser.parse_args()

    item_names = args.item if args.item else list(MENU_ITEMS.keys())

    if not os.path.isdir(args.install_dir):
        print(f"error: {args.install_dir} is not a directory", file=sys.stderr)
        return 1

    changes = apply_patches(args.install_dir, args.output_dir, item_names)

    if changes:
        print(f"Applied {len(changes)} changes:")
        for c in changes:
            print(f"  {c}")
        print(f"Output: {args.output_dir}")
    else:
        print("No patches applied (files not found)")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
