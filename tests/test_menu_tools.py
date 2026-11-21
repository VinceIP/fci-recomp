#!/usr/bin/env python3
"""Tests for mgb.py, stringtable.py, dat2.py repack, and menu_items.py.

These tests use synthetic data — no game files are needed.
Run: pytest tests/test_menu_tools.py
"""

import os
import struct
import sys
import zlib

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import dat2
import menu_items
import mgb
import stringtable

# --------------------------------------------------------------------------
# Synthetic .mgb builder
# --------------------------------------------------------------------------


def make_button_record(element_id: int, label: str = "TEST", up_id: int = 0, nav_flag: int = 0) -> bytes:
    """Build a minimal button record (223 bytes, like B_Credits)."""
    rec = bytearray(223)
    # ID at +0x00
    struct.pack_into("<I", rec, 0, element_id)
    # Header: 0, type=2
    struct.pack_into("<I", rec, 4, 0)
    struct.pack_into("<I", rec, 8, 2)
    # Packed ID ref at +0x0C
    struct.pack_into("<I", rec, 0x0C, (element_id & 0xFF) << 24)
    struct.pack_into("<I", rec, 0x10, (element_id >> 8) & 0x00FFFFFF)
    # Label char count at +0xAE
    label_utf16 = label.encode("utf-16-le")
    char_count = len(label_utf16) // 2
    rec[0xAE] = char_count
    # Label at +0xB2 (padded to 4-byte boundary)
    label_padded = label_utf16 + b"\x00" * ((len(label_utf16) + 3) // 4 * 4 - len(label_utf16))
    rec[0xB2 : 0xB2 + len(label_padded)] = label_padded
    # Nav link: last 6 bytes
    if nav_flag:
        rec[223 - 6] = nav_flag
        struct.pack_into("<I", rec, 223 - 5, up_id)
        rec[223 - 1] = 0x01
    return bytes(rec)


def make_mgb(area_name: str, buttons: list[tuple[str, str, str | None]]) -> bytes:
    """Build a minimal .mgb file with one area and the given buttons.

    buttons: list of (name, label, up_name) where up_name is None for the first.
    """
    data = bytearray()

    # MAGMA header (minimal — 0x15C bytes of zeros with magic)
    data.extend(b"MAGMA")
    data.extend(b"\x00" * (0x15C - 5))

    # Area record
    area_id = mgb.crc32_name(area_name)
    data.extend(struct.pack("<I", area_id))
    data.extend(struct.pack("<I", len(buttons)))

    # Build button records
    records = []
    for name, label, up_name in buttons:
        eid = mgb.crc32_name(name)
        up_id = mgb.crc32_name(up_name) if up_name else 0
        flag = 1 if up_name else 0
        records.append(make_button_record(eid, label, up_id, flag))

    for rec in records:
        data.extend(rec)

    return bytes(data)


# --------------------------------------------------------------------------
# mgb.py tests
# --------------------------------------------------------------------------


class TestMgb:
    def test_crc32_name(self):
        assert mgb.crc32_name("B_Quit") == 0x8BCDF569
        assert mgb.crc32_name("B_Credits") == 0x89ADE9D9
        assert mgb.crc32_name("P_Main") == 0x39514C08

    def test_find_area(self):
        mgb_data = make_mgb(
            "P_Test",
            [
                ("B_A", "A", None),
                ("B_B", "B", "B_A"),
                ("B_C", "C", "B_B"),
            ],
        )
        area = mgb.find_area(mgb_data, "P_Test")
        assert area is not None
        off, count = area
        assert count == 3

    def test_find_area_missing(self):
        mgb_data = make_mgb("P_Test", [("B_A", "A", None)])
        assert mgb.find_area(mgb_data, "P_Missing") is None

    def test_find_all_records(self):
        mgb_data = make_mgb(
            "P_Test",
            [
                ("B_StoryMode", "STORY MODE", None),
                ("B_Credits", "CREDITS", "B_StoryMode"),
            ],
        )
        recs = mgb.find_all_records(mgb_data)
        names = {mgb.name_for_id(eid) for _, eid, _ in recs}
        assert "B_StoryMode" in names
        assert "B_Credits" in names

    def test_get_label(self):
        rec = make_button_record(mgb.crc32_name("B_Credits"), "HELLO")
        label, char_count = mgb.get_label(rec, 0)
        assert label == "HELLO"
        assert char_count == 5

    def test_get_nav_link(self):
        a_id = mgb.crc32_name("B_A")
        b_id = mgb.crc32_name("B_B")
        rec = make_button_record(b_id, "B", a_id, 1)
        up_id, flag = mgb.get_nav_link(rec, 0, 223)
        assert up_id == a_id
        assert flag == 1

    def test_set_nav_link(self):
        a_id = mgb.crc32_name("B_A")
        b_id = mgb.crc32_name("B_B")
        c_id = mgb.crc32_name("B_C")
        rec = bytearray(make_button_record(b_id, "B", a_id, 1))
        mgb.set_nav_link(rec, 0, 223, c_id, 1)
        up_id, flag = mgb.get_nav_link(bytes(rec), 0, 223)
        assert up_id == c_id
        assert flag == 1


# --------------------------------------------------------------------------
# stringtable.py tests
# --------------------------------------------------------------------------

SAMPLE_XML_CRLF = (
    '<?xml version="1.0" encoding="UTF-8"?>\r\n'
    "<stringtable>\r\n"
    '  <section name="P_Main">\r\n'
    '    <string enum="B_StoryMode" value="STORY MODE"/>\r\n'
    '    <string enum="B_Credits" value="CREDITS"/>\r\n'
    '    <string enum="B_Cheats" value="CHEATS"/>\r\n'
    '    <string enum="B_CheatsDebug" value="CHEATS (DEBUG)"/>\r\n'
    "  </section>\r\n"
    "</stringtable>\r\n"
)

SAMPLE_XML_LF = SAMPLE_XML_CRLF.replace("\r\n", "\n")


class TestStringtable:
    def test_insert_crlf(self):
        result = stringtable.insert_string_row(SAMPLE_XML_CRLF, "P_Main", "B_Credits", "B_Quit", "QUIT")
        assert 'enum="B_Quit" value="QUIT"' in result
        assert "\r\n" in result
        credits_pos = result.index("B_Credits")
        quit_pos = result.index("B_Quit")
        cheats_pos = result.index("B_Cheats")
        assert credits_pos < quit_pos < cheats_pos

    def test_insert_lf(self):
        result = stringtable.insert_string_row(SAMPLE_XML_LF, "P_Main", "B_Credits", "B_Quit", "QUIT")
        assert 'enum="B_Quit" value="QUIT"' in result
        assert "\r\n" not in result

    def test_insert_missing_section(self):
        with pytest.raises(ValueError, match="section"):
            stringtable.insert_string_row(SAMPLE_XML_CRLF, "P_Missing", "B_Credits", "B_Quit", "QUIT")

    def test_insert_missing_enum(self):
        with pytest.raises(ValueError, match="enum"):
            stringtable.insert_string_row(SAMPLE_XML_CRLF, "P_Main", "B_Missing", "B_Quit", "QUIT")

    def test_count_strings(self):
        assert stringtable.count_strings(SAMPLE_XML_CRLF, "P_Main") == 4
        result = stringtable.insert_string_row(SAMPLE_XML_CRLF, "P_Main", "B_Credits", "B_Quit", "QUIT")
        assert stringtable.count_strings(result, "P_Main") == 5

    def test_rename_string_row(self):
        result = stringtable.rename_string_row(SAMPLE_XML_CRLF, "P_Main", "B_CheatsDebug", "B_Quit", "QUIT")
        assert 'enum="B_Quit" value="QUIT"' in result
        assert 'enum="B_CheatsDebug"' not in result
        assert 'value="CHEATS (DEBUG)"' not in result
        # CRLF preserved
        assert "\r\n" in result

    def test_rename_string_row_missing_section(self):
        with pytest.raises(ValueError, match="section"):
            stringtable.rename_string_row(SAMPLE_XML_CRLF, "P_Missing", "B_CheatsDebug", "B_Quit", "QUIT")

    def test_rename_string_row_missing_enum(self):
        with pytest.raises(ValueError, match="enum"):
            stringtable.rename_string_row(SAMPLE_XML_CRLF, "P_Main", "B_Missing", "B_Quit", "QUIT")


# --------------------------------------------------------------------------
# dat2.py repack tests
# --------------------------------------------------------------------------


def make_dat2(entries: list[tuple[bytes, str]]) -> tuple[bytes, bytes]:
    """Create a synthetic DAT2. Returns (fat_bytes, dat_bytes)."""

    def name_hash(name):
        p = name.lower().replace("/", "\\")
        if not p.startswith("\\"):
            p = "\\" + p
        return zlib.crc32(p.encode()) & 0xFFFFFFFF

    hashed = [(payload, name_hash(name), name) for payload, name in entries]
    hashed.sort(key=lambda x: x[1])

    dat = bytearray()
    fat_entries = []
    for payload, h, _ in hashed:
        while len(dat) % 4 != 0:
            dat.append(0)
        off = len(dat)
        dat.extend(payload)
        fat_entries.append((off, len(payload), h))

    fat = bytearray(b"DAT2")
    fat.extend(struct.pack("<I", len(fat_entries)))
    for off, size, h in fat_entries:
        fat.extend(struct.pack("<III", off, size, h))

    return bytes(fat), bytes(dat)


class TestDat2Repack:
    def test_repack_grow(self, tmp_path):
        fat_bytes, dat_bytes = make_dat2([(b"hello", "entry_a"), (b"world", "entry_b")])
        fat_path = tmp_path / "test.fat"
        dat_path = tmp_path / "test.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        repl = b"world - expanded"
        repl_path = tmp_path / "repl.bin"
        repl_path.write_bytes(repl)

        out_fat = tmp_path / "out.fat"
        out_dat = tmp_path / "out.dat"

        dat2.main(
            [
                "repack",
                str(fat_path),
                "--name",
                "entry_b",
                "--from",
                str(repl_path),
                "--out-fat",
                str(out_fat),
                "--out-dat",
                str(out_dat),
            ]
        )

        out_fat_bytes = out_fat.read_bytes()
        out_dat_bytes = out_dat.read_bytes()
        assert out_fat_bytes[:4] == b"DAT2"
        count = struct.unpack_from("<I", out_fat_bytes, 4)[0]
        assert count == 2

        h_b = zlib.crc32(b"\\entry_b") & 0xFFFFFFFF
        for i in range(count):
            off, size, h = struct.unpack_from("<III", out_fat_bytes, 8 + i * 12)
            if h == h_b:
                payload = out_dat_bytes[off : off + size]
                assert payload == repl
                break
        else:
            pytest.fail("entry_b not found in output")

    def test_repack_shrink(self, tmp_path):
        fat_bytes, dat_bytes = make_dat2([(b"hello world this is long", "entry_a"), (b"world", "entry_b")])
        fat_path = tmp_path / "test.fat"
        dat_path = tmp_path / "test.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        repl = b"hi"
        repl_path = tmp_path / "repl.bin"
        repl_path.write_bytes(repl)

        out_fat = tmp_path / "out.fat"
        out_dat = tmp_path / "out.dat"

        dat2.main(
            [
                "repack",
                str(fat_path),
                "--name",
                "entry_b",
                "--from",
                str(repl_path),
                "--out-fat",
                str(out_fat),
                "--out-dat",
                str(out_dat),
            ]
        )

        out_dat_bytes = out_dat.read_bytes()
        assert len(out_dat_bytes) < len(dat_bytes)

    def test_repack_preserves_other_entries(self, tmp_path):
        fat_bytes, dat_bytes = make_dat2(
            [(b"alpha content", "entry_a"), (b"beta content", "entry_b"), (b"gamma content", "entry_c")]
        )
        fat_path = tmp_path / "test.fat"
        dat_path = tmp_path / "test.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        repl = b"beta content REPLACED"
        repl_path = tmp_path / "repl.bin"
        repl_path.write_bytes(repl)

        out_fat = tmp_path / "out.fat"
        out_dat = tmp_path / "out.dat"

        dat2.main(
            [
                "repack",
                str(fat_path),
                "--name",
                "entry_b",
                "--from",
                str(repl_path),
                "--out-fat",
                str(out_fat),
                "--out-dat",
                str(out_dat),
            ]
        )

        out_fat_bytes = out_fat.read_bytes()
        out_dat_bytes = out_dat.read_bytes()
        count = struct.unpack_from("<I", out_fat_bytes, 4)[0]

        h_a = zlib.crc32(b"\\entry_a") & 0xFFFFFFFF
        h_c = zlib.crc32(b"\\entry_c") & 0xFFFFFFFF
        for i in range(count):
            off, size, h = struct.unpack_from("<III", out_fat_bytes, 8 + i * 12)
            if h == h_a:
                assert out_dat_bytes[off : off + size] == b"alpha content"
            if h == h_c:
                assert out_dat_bytes[off : off + size] == b"gamma content"

    def test_repack_refuses_media_path(self, tmp_path):
        fat_bytes, dat_bytes = make_dat2([(b"data", "entry_a")])
        fat_path = tmp_path / "test.fat"
        dat_path = tmp_path / "test.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        repl_path = tmp_path / "repl.bin"
        repl_path.write_bytes(b"data2")

        out_fat = tmp_path / "qwen-in" / "out.fat"
        out_dat = tmp_path / "qwen-in" / "out.dat"
        out_fat.parent.mkdir(exist_ok=True)

        with pytest.raises(SystemExit, match="media/reference"):
            dat2.main(
                [
                    "repack",
                    str(fat_path),
                    "--name",
                    "entry_a",
                    "--from",
                    str(repl_path),
                    "--out-fat",
                    str(out_fat),
                    "--out-dat",
                    str(out_dat),
                ]
            )


# --------------------------------------------------------------------------
# menu_items.py tests (rename approach)
# --------------------------------------------------------------------------


class TestMenuItems:
    def test_patch_mgb_rename(self):
        """Test renaming an element in a synthetic .mgb file."""
        mgb_data = make_mgb(
            "P_Main",
            [
                ("B_StoryMode", "STORY MODE", None),
                ("B_Credits", "CREDITS", "B_StoryMode"),
                ("B_CheatsDebug", "CHEATS (DEBUG)", "B_Credits"),
            ],
        )

        patched, changes = menu_items.patch_mgb_rename(mgb_data, "B_CheatsDebug", "B_Quit", "QUIT")

        # File size unchanged (rename, not insert)
        assert len(patched) == len(mgb_data)

        # B_Quit ID is present
        quit_id = mgb.crc32_name("B_Quit")
        assert struct.pack("<I", quit_id) in patched

        # B_CheatsDebug ID is gone
        cheatsdebug_id = mgb.crc32_name("B_CheatsDebug")
        # The ID should not appear as a record start
        off = mgb.find_record_start(patched, cheatsdebug_id)
        assert off == -1

        # B_Quit record exists
        off = mgb.find_record_start(patched, quit_id)
        assert off != -1

        # Label is updated
        label, _ = mgb.get_label(patched, off)
        assert label == "QUIT"

        # Changes are reported
        assert any("ID" in c for c in changes)
        assert any("label" in c for c in changes)

    def test_patch_mgb_rename_nav_chain(self):
        """Test that nav links pointing to the old ID are updated."""
        # In this layout, B_B has a nav link pointing UP to B_CheatsDebug.
        # After renaming B_CheatsDebug to B_Quit, B_B's nav link should
        # point to B_Quit instead.
        mgb_data = make_mgb(
            "P_Main",
            [
                ("B_StoryMode", "STORY MODE", None),
                ("B_CheatsDebug", "CHEATS (DEBUG)", "B_StoryMode"),
                ("B_B", "B", "B_CheatsDebug"),  # nav up = B_CheatsDebug
            ],
        )

        patched, changes = menu_items.patch_mgb_rename(mgb_data, "B_CheatsDebug", "B_Quit", "QUIT")

        # B_B's nav link should now point to B_Quit (was B_CheatsDebug)
        b_b_id = mgb.crc32_name("B_B")
        b_quit_id = mgb.crc32_name("B_Quit")
        off = mgb.find_record_start(patched, b_b_id)
        assert off != -1
        up_id, flag = mgb.get_nav_link(patched, off, 223)
        assert up_id == b_quit_id
        assert flag == 1

        # Nav link change is reported
        assert any("nav link" in c for c in changes)

    def test_patch_mgb_rename_missing(self):
        """Test renaming a non-existent element raises an error."""
        mgb_data = make_mgb("P_Main", [("B_A", "A", None)])
        with pytest.raises(ValueError, match="not found"):
            menu_items.patch_mgb_rename(mgb_data, "B_Missing", "B_Quit", "QUIT")

    def test_patch_mgb_rename_label_too_long(self):
        """Test that a label longer than the existing one is rejected."""
        mgb_data = make_mgb(
            "P_Main",
            [
                ("B_A", "A", None),
                ("B_Short", "AB", "B_A"),
            ],
        )
        # "AB" is 2 chars (4 bytes padded). "TOO LONG LABEL" is 14 chars (28 bytes padded).
        with pytest.raises(ValueError, match="exceeds"):
            menu_items.patch_mgb_rename(mgb_data, "B_Short", "B_Long", "TOO LONG LABEL")

    def test_menu_items_table(self):
        """The menu items table has the expected structure."""
        assert "B_Quit" in menu_items.MENU_ITEMS
        item = menu_items.MENU_ITEMS["B_Quit"]
        assert item["rename_from"] == "B_CheatsDebug"
        assert item["label"] == "QUIT"
        assert item["mgb_file"] == "magma_crypregame.mgb"
        assert item["area"] == "P_Main"
        assert item["stringtable"] == "stringtable-mainmenu-eng.xml"
        assert item["string_section"] == "P_Main"
        assert item["dat2_name"] == "magma/crypregame.mgb"

    def test_extract_from_dat2(self, tmp_path):
        """Test extracting a file from a synthetic DAT2 package."""
        # Create a synthetic .mgb
        mgb_data = make_mgb("P_Main", [("B_A", "A", None)])
        # Create a DAT2 with the .mgb
        fat_bytes, dat_bytes = make_dat2([(mgb_data, "magma/test.mgb")])
        fat_path = tmp_path / "common.fat"
        dat_path = tmp_path / "common.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        result = menu_items.extract_from_dat2(str(fat_path), str(dat_path), "magma/test.mgb")
        assert result is not None
        data, idx = result
        assert data == mgb_data

    def test_extract_from_dat2_missing(self, tmp_path):
        """Test extracting a non-existent file returns None."""
        fat_bytes, dat_bytes = make_dat2([(b"data", "entry_a")])
        fat_path = tmp_path / "common.fat"
        dat_path = tmp_path / "common.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        result = menu_items.extract_from_dat2(str(fat_path), str(dat_path), "nonexistent.mgb")
        assert result is None

    def test_write_dat2_output(self, tmp_path):
        """Test writing a repacked DAT2 with a replacement."""
        mgb_data = make_mgb("P_Main", [("B_A", "A", None)])
        other_data = b"other file content"
        fat_bytes, dat_bytes = make_dat2([(mgb_data, "magma/test.mgb"), (other_data, "other.txt")])
        fat_path = tmp_path / "common.fat"
        dat_path = tmp_path / "common.dat"
        fat_path.write_bytes(fat_bytes)
        dat_path.write_bytes(dat_bytes)

        # Find the index of the .mgb entry
        result = menu_items.extract_from_dat2(str(fat_path), str(dat_path), "magma/test.mgb")
        assert result is not None
        mgb_idx = result[1]

        # Replace the .mgb with a modified version
        new_mgb = mgb_data + b"EXTRA"
        menu_items.write_dat2_output(str(fat_path), str(dat_path), str(tmp_path / "out"), {mgb_idx: new_mgb})

        # Verify the output files exist
        out_fat = tmp_path / "out" / "common.fat"
        out_dat = tmp_path / "out" / "common.dat"
        assert out_fat.exists()
        assert out_dat.exists()

        # Verify the .mgb was replaced
        result = menu_items.extract_from_dat2(str(out_fat), str(out_dat), "magma/test.mgb")
        assert result is not None
        data, _ = result
        assert data == new_mgb

        # Verify the other file is unchanged
        result = menu_items.extract_from_dat2(str(out_fat), str(out_dat), "other.txt")
        assert result is not None
        data, _ = result
        assert data == other_data
