"""tools/dat2.py on a tiny synthetic DAT2 pair (no game data): list, entry, extract"""

import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import dat2  # noqa: E402


def build_dat2(tmp_path, files):
    """files: {"name": (payload_bytes, hash_int)} -> (fat_path, dat_path).

    Layouts a real DAT2: magic + u32 count + count*(offset,size,hash), payloads
    packed sequentially into the .dat at their offsets (all little-endian).
    """
    payloads = []
    entries = []
    offset = 0
    for _name, (buf, h) in files.items():
        entries.append((offset, len(buf), h))
        payloads.append(buf)
        offset += len(buf)
    dat = b"".join(payloads)
    (tmp_path / "test.dat").write_bytes(dat)

    fat = bytearray()
    fat += b"DAT2"
    fat += struct.pack("<I", len(entries))
    for off, size, h in entries:
        fat += struct.pack("<III", off, size, h)
    (tmp_path / "test.fat").write_bytes(bytes(fat))
    return tmp_path / "test.fat", tmp_path / "test.dat"


FILES = {
    "alpha": (b"hello alpha payload", 0x1111_1111),
    "beta": (b"world beta payload", 0x2222_2222),
    "gamma": (b"<xml>gamma</xml>", 0x3333_3333),
}


def test_fat_parses_entry_count_and_math(tmp_path):
    fat_path, dat_path = build_dat2(tmp_path, FILES)
    fat = dat2.Fat(str(fat_path), str(dat_path))
    assert fat.count == 3
    # every entry's [offset, size] lands inside the .dat
    data = dat_path.read_bytes()
    for off, size, _ in fat.entries:
        assert off + size <= len(data)
    # offsets are sequential and non-overlapping
    assert fat.entries[0][0] == 0
    for prev, cur in zip(fat.entries, fat.entries[1:]):
        assert cur[0] == prev[0] + prev[1]


def test_payload_roundtrip(tmp_path):
    fat_path, _ = build_dat2(tmp_path, FILES)
    fat = dat2.Fat(str(fat_path), str(fat_path.with_suffix(".dat")))
    for i, (name, (buf, _)) in enumerate(FILES.items()):
        assert fat.payload(i) == buf, name


def test_extract_by_hash(tmp_path):
    fat_path, _ = build_dat2(tmp_path, FILES)
    out = tmp_path / "out.bin"
    dat2.main(["extract", str(fat_path), "--hash", "0x22222222", str(out)])
    assert out.read_bytes() == FILES["beta"][0]


def test_extract_by_find(tmp_path):
    fat_path, _ = build_dat2(tmp_path, FILES)
    out = tmp_path / "out.bin"
    dat2.main(["extract", str(fat_path), "--find", "gamma", str(out)])
    assert out.read_bytes() == FILES["gamma"][0]


def test_find_is_ambiguous_when_two_match(tmp_path):
    two = {"a": (b"shared token here", 0xAAAA), "b": (b"another shared token", 0xBBBB)}
    fat_path, _ = build_dat2(tmp_path, two)
    try:
        dat2.main(["extract", str(fat_path), "--find", "shared token", str(tmp_path / "o")])
    except SystemExit:
        return
    raise AssertionError("expected an ambiguity SystemExit")


def test_entry_by_index(tmp_path, capsys):
    fat_path, _ = build_dat2(tmp_path, FILES)
    dat2.main(["entry", str(fat_path), "1"])
    assert capsys.readouterr().out == FILES["beta"][0].decode()


def test_hash_not_in_fat_is_an_error(tmp_path):
    fat_path, _ = build_dat2(tmp_path, FILES)
    try:
        dat2.main(["extract", str(fat_path), "--hash", "0xDEADBEEF", str(tmp_path / "o")])
    except SystemExit:
        return
    raise AssertionError("expected a SystemExit for a missing hash")


def test_name_resolves_by_crc32_of_normalised_path(tmp_path):
    # the hash is crc32 of the normalised path (lower-case, / -> \\, leading \\)
    import zlib

    name = "Magma/CryPregame.mgb"
    norm = "\\magma\\crypregame.mgb"
    assert dat2.normalize_name(name) == norm
    h = zlib.crc32(norm.encode()) & 0xFFFFFFFF
    assert dat2.name_hash(name) == h

    payload = b"mgb payload"
    fat_path, _ = build_dat2(tmp_path, {"mgb": (payload, h)})
    out = tmp_path / "out.bin"
    dat2.main(["extract", str(fat_path), "--name", name, str(out)])
    assert out.read_bytes() == payload


def test_name_hash_normalisation(tmp_path):
    # leading backslash, case, and slash direction all normalise the same
    a = dat2.name_hash("\\magma\\crypregame.mgb")
    b = dat2.name_hash("MAGMA/CRYPREGAME.MGB")
    c = dat2.name_hash("magma/crypregame.mgb")
    d = dat2.name_hash("\\Magma\\CryPregame.mgb")
    assert a == b == c == d


def test_name_not_in_fat_is_an_error(tmp_path):
    fat_path, _ = build_dat2(tmp_path, FILES)
    try:
        dat2.main(["extract", str(fat_path), "--name", "nonexistent/path.mgb", str(tmp_path / "o")])
    except SystemExit:
        return
    raise AssertionError("expected a SystemExit for a name not in the FAT")


def test_rejects_a_non_dat2_fat(tmp_path):
    (tmp_path / "bad.fat").write_bytes(b"XXXX" + struct.pack("<I", 0))
    try:
        dat2.Fat(str(tmp_path / "bad.fat"), str(tmp_path / "bad.dat"))
    except SystemExit:
        return
    raise AssertionError("expected a SystemExit for a bad magic")


def test_truncated_fat_is_rejected(tmp_path):
    # declares 4 entries but only has room for 2
    fat = b"DAT2" + struct.pack("<I", 4) + struct.pack("<III", 0, 4, 1) * 2
    (tmp_path / "trunc.fat").write_bytes(fat)
    try:
        dat2.Fat(str(tmp_path / "trunc.fat"), str(tmp_path / "trunc.dat"))
    except SystemExit:
        return
    raise AssertionError("expected a SystemExit for a truncated fat")


# --------------------------------------------------------------------------
# patch subcommand
# --------------------------------------------------------------------------


def test_patch_same_size_replaces_entry(tmp_path):
    """patch replaces the entry's payload with the same-size file."""
    fat_path, dat_path = build_dat2(tmp_path, FILES)
    repl = b"REPLACED BETA DATA"  # same size as "world beta payload" (16 bytes)
    # "world beta payload" is 18 bytes, so we need exactly 18
    repl = b"REPLACED BETA DAT!"  # 18 bytes
    assert len(repl) == len(FILES["beta"][0]), f"{len(repl)} != {len(FILES['beta'][0])}"
    (tmp_path / "repl.bin").write_bytes(repl)
    dat2.main(["patch", str(fat_path), "--hash", "0x22222222", "--from", str(tmp_path / "repl.bin")])
    # verify: extract the entry and check it was replaced
    out = tmp_path / "out.bin"
    dat2.main(["extract", str(fat_path), "--hash", "0x22222222", str(out)])
    assert out.read_bytes() == repl


def test_patch_wrong_size_is_refused(tmp_path):
    """patch refuses if the replacement file is a different size."""
    fat_path, _ = build_dat2(tmp_path, FILES)
    (tmp_path / "short.bin").write_bytes(b"too short")
    try:
        dat2.main(["patch", str(fat_path), "--hash", "0x22222222", "--from", str(tmp_path / "short.bin")])
    except SystemExit as e:
        assert "same-size" in str(e) or "size" in str(e)
        return
    raise AssertionError("expected a SystemExit for a size mismatch")


def test_patch_refuses_hard_linked_file(tmp_path):
    """patch refuses if the .dat is hard-linked (likely the original)."""
    fat_path, dat_path = build_dat2(tmp_path, FILES)
    # create a hard link
    link_path = tmp_path / "link.dat"
    os.link(str(dat_path), str(link_path))
    repl = b"REPLACED BETA DATA  "
    (tmp_path / "repl.bin").write_bytes(repl)
    try:
        dat2.main(["patch", str(fat_path), "--hash", "0x22222222", "--from", str(tmp_path / "repl.bin")])
    except SystemExit as e:
        assert "hard-link" in str(e) or "nlink" in str(e)
        return
    raise AssertionError("expected a SystemExit for a hard-linked file")


def test_patch_refuses_media_path(tmp_path):
    """patch refuses if the .dat is under a media/reference path."""
    # create a directory structure that looks like a media path
    media_dir = tmp_path / "build" / "qwen-in"
    media_dir.mkdir(parents=True)
    fat_path, dat_path = build_dat2(media_dir, FILES)
    repl = b"REPLACED BETA DATA  "
    (tmp_path / "repl.bin").write_bytes(repl)
    try:
        dat2.main(["patch", str(fat_path), "--hash", "0x22222222", "--from", str(tmp_path / "repl.bin")])
    except SystemExit as e:
        assert "media" in str(e) or "reference" in str(e) or "qwen-in" in str(e)
        return
    raise AssertionError("expected a SystemExit for a media path")


def test_patch_by_name(tmp_path):
    """patch resolves the entry by --name (CRC-32 hash)."""
    import zlib

    name = "Magma/CryPregame.mgb"
    h = zlib.crc32(dat2.normalize_name(name).encode()) & 0xFFFFFFFF
    payload = b"ORIGINAL MGB DATA!!"  # 19 bytes
    fat_path, dat_path = build_dat2(tmp_path, {"mgb": (payload, h)})
    repl = b"PATCHED   MGB DATA!"  # 19 bytes (same size)
    assert len(repl) == len(payload)
    (tmp_path / "repl.mgb").write_bytes(repl)
    dat2.main(["patch", str(fat_path), "--name", name, "--from", str(tmp_path / "repl.mgb")])
    out = tmp_path / "out.bin"
    dat2.main(["extract", str(fat_path), "--name", name, str(out)])
    assert out.read_bytes() == repl
