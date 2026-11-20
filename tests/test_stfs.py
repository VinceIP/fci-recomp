"""Tests for tools/stfs.py: build a small synthetic STFS CON package and
check the extractor reads the header, file table, and payload back correctly.

No game files are used; the package is constructed in-test from the publicly
documented layout (header, L0 hash-table blocks, file table, data). The block
mapping uses the standard STFS ``fix_blocknum`` with ``table_size_shift = 1``
(two-block hash tables), which interleaves a hash-table block after every 170
data blocks.
"""

import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import stfs  # noqa: E402

BLOCK = 0x1000
FT_OFFSET = 0xC000
SHIFT = stfs.table_size_shift(0x971A)  # 1, the value the Predator packages use
GROUP0 = 0xAA  # 170


def _hash_table_block(block_hashes):
    """Build one L0 hash-table block: 170 records of SHA1(20)+status(1)+next(3)."""
    out = bytearray()
    for i in range(170):
        if i < len(block_hashes) and block_hashes[i] is not None:
            out += block_hashes[i] + bytes([0xC0]) + b"\xff\xff\xff"
        else:
            out += b"\x00" * 24
    out += b"\x00" * (BLOCK - len(out))  # pad to a full block
    return bytes(out)


def build_package(name: str, payload: bytes, title_id: int = 0x555307DC, display: str = "Test Map") -> bytes:
    """Assemble a minimal STFS CON package holding one file ``name``.

    Data blocks are placed at the standard non-contiguous positions (a
    hash-table block is interleaved after every 170 data blocks), and the L0
    hash tables are placed where ``l0_table_offset`` says they belong.
    """
    nblocks = (len(payload) + BLOCK - 1) // BLOCK
    data = payload + b"\x00" * (nblocks * BLOCK - len(payload))

    # Total clusters: file table (0) + data (1..nblocks).
    total_clusters = nblocks + 1
    # The file space spans cluster 0 .. total_clusters-1, plus the interleaved
    # hash-table blocks. The largest physical offset we need:
    max_phys = stfs.fix_blocknum(total_clusters - 1, SHIFT) + 2  # +2 for table blocks
    pkg = bytearray(FT_OFFSET + (max_phys + 1) * BLOCK)

    # --- header ---
    pkg[0:4] = b"CON "
    struct.pack_into(">I", pkg, 0x340, 0x971A)  # header size -> shift 1
    struct.pack_into(">i", pkg, 0x344, 1)  # content type: Saved Game
    struct.pack_into(">q", pkg, 0x34C, len(data))  # content size
    struct.pack_into(">I", pkg, 0x360, title_id)  # title id
    dn = display.encode("utf-16-be")
    pkg[0x411 : 0x411 + len(dn)] = dn

    # --- volume descriptor @0x379 ---
    vd = 0x379
    struct.pack_into(">H", pkg, vd + 0x03, 1)  # file table block count
    pkg[vd + 0x05 : vd + 0x08] = (0).to_bytes(3, "big")  # file table block number
    struct.pack_into(">i", pkg, vd + 0x1C, total_clusters)  # total allocated
    struct.pack_into(">i", pkg, vd + 0x20, 0)  # total unallocated

    # --- file table @0xC000 (cluster 0) ---
    entry = bytearray(64)
    raw = name.encode("ascii")
    entry[: len(raw)] = raw
    entry[0x28] = 0x40 | len(raw)  # flags: consecutive | len
    entry[0x29:0x2C] = nblocks.to_bytes(3, "little")  # block count
    entry[0x2C:0x2F] = nblocks.to_bytes(3, "little")  # block count (copy)
    entry[0x2F:0x32] = (1).to_bytes(3, "little")  # starting block = 1
    entry[0x32:0x34] = b"\xff\xff"  # path index: root
    struct.pack_into(">I", entry, 0x34, len(payload))  # file size
    pkg[FT_OFFSET : FT_OFFSET + 64] = bytes(entry)

    # --- data blocks at their non-contiguous positions (cluster 1..) ---
    for b in range(nblocks):
        off = stfs.block_offset(1 + b, SHIFT)
        pkg[off : off + BLOCK] = data[b * BLOCK : (b + 1) * BLOCK]

    # --- L0 hash tables: one per 170-cluster group ---
    # Collect the SHA1 of every cluster, then write each L0 table block.
    cluster_hashes = {}
    for c in range(total_clusters):
        off = stfs.block_offset(c, SHIFT)
        cluster_hashes[c] = hashlib.sha1(bytes(pkg[off : off + BLOCK])).digest()
    # Write the L0 table for each group that has any of our clusters.
    written = set()
    for c in range(total_clusters):
        t_off = stfs.l0_table_offset(c, SHIFT)
        if t_off in written:
            continue
        written.add(t_off)
        # This table covers clusters [c - c%170, c - c%170 + 170).
        base = c - c % GROUP0
        recs = [None] * 170
        for i in range(170):
            cc = base + i
            if cc in cluster_hashes:
                recs[i] = cluster_hashes[cc]
        pkg[t_off : t_off + BLOCK] = _hash_table_block(recs)

    return bytes(pkg)


def test_header_fields():
    payload = b"hello stfs world" * 100
    pkg = build_package("MyFile", payload, title_id=0xABCDEF01, display="My Display")
    h = stfs.read_header(pkg)
    assert h["magic"] == "CON"
    assert h["content_type"] == 1
    assert h["title_id"] == 0xABCDEF01
    assert h["display_name"] == "My Display"
    assert h["header_size"] == 0x971A


def test_table_size_shift():
    # The Predator packages use header size 0x971A -> shift 1.
    assert stfs.table_size_shift(0x971A) == 1
    # A header size in the 0xBxxx range would give shift 0.
    assert stfs.table_size_shift(0xB000) == 0


def test_fix_blocknum():
    # Below 170, no separation.
    assert stfs.fix_blocknum(0, 1) == 0
    assert stfs.fix_blocknum(169, 1) == 169
    # At 170, the first hash-table block (2 blocks wide) is skipped.
    assert stfs.fix_blocknum(170, 1) == 174
    assert stfs.fix_blocknum(339, 1) == 343
    # At 340 (group 2), two hash-table blocks (4 blocks) are skipped.
    assert stfs.fix_blocknum(340, 1) == 346


def test_file_table():
    payload = os.urandom(5000)
    pkg = build_package("Payload.bin", payload)
    entries = stfs.read_file_table(pkg)
    assert len(entries) == 1
    e = entries[0]
    assert e["name"] == "Payload.bin"
    assert e["size"] == len(payload)
    assert e["start_block"] == 1
    assert e["block_count"] == (len(payload) + BLOCK - 1) // BLOCK
    assert e["flags"] & 0x40  # consecutive flag


def test_extract_roundtrip_small():
    payload = os.urandom(9000)  # spans two blocks
    pkg = build_package("data.bin", payload)
    entries = stfs.read_file_table(pkg)
    out = stfs.read_file(pkg, entries[0], SHIFT)
    assert out == payload


def test_extract_roundtrip_large():
    # > 170 blocks: the data is non-contiguous (hash-table blocks interleaved).
    payload = os.urandom(200 * BLOCK)  # 200 blocks, crosses the 170 boundary
    pkg = build_package("big.bin", payload)
    entries = stfs.read_file_table(pkg)
    out = stfs.read_file(pkg, entries[0], SHIFT)
    assert out == payload


def test_hash_check_passes_small():
    payload = os.urandom(3000)
    pkg = build_package("chk.bin", payload)
    entries = stfs.read_file_table(pkg)
    rows = stfs.verify_hashes(pkg, entries, SHIFT)
    assert rows[0]["ok"] is True
    assert rows[0]["checked"] == entries[0]["block_count"]


def test_hash_check_passes_large():
    # > 170 blocks: every block must verify against its (interleaved) L0 table.
    payload = os.urandom(200 * BLOCK)
    pkg = build_package("big.bin", payload)
    entries = stfs.read_file_table(pkg)
    rows = stfs.verify_hashes(pkg, entries, SHIFT)
    assert rows[0]["ok"] is True
    assert rows[0]["checked"] == entries[0]["block_count"] == 200


def test_hash_mismatch_fails():
    # Corrupt one data block past the 170 boundary; the check must catch it.
    payload = os.urandom(200 * BLOCK)
    pkg = bytearray(build_package("big.bin", payload))
    off = stfs.block_offset(1 + 180, SHIFT)  # cluster 181
    pkg[off] ^= 0xFF
    entries = stfs.read_file_table(bytes(pkg))
    rows = stfs.verify_hashes(bytes(pkg), entries, SHIFT)
    assert rows[0]["ok"] is False


def test_bad_magic_raises():
    pkg = bytearray(build_package("x", b"abcd" * 100))
    pkg[0:4] = b"XXXX"
    try:
        stfs.read_header(bytes(pkg))
    except stfs.StfsError:
        return
    raise AssertionError("expected StfsError for a bad magic")


def test_file_past_end_raises():
    # Claim a block count that runs past the end of the package.
    payload = b"small"
    pkg = bytearray(build_package("big", payload))
    # Bump the block count (at entry offset 0x29, i24le) to a huge value.
    struct.pack_into("<I", pkg, FT_OFFSET + 0x29, 0x3FFFF)
    entries = stfs.read_file_table(bytes(pkg))
    try:
        stfs.read_file(bytes(pkg), entries[0], SHIFT)
    except stfs.StfsError:
        return
    raise AssertionError("expected StfsError for a file past the end")


def test_main_list_and_extract(tmp_path):
    payload = os.urandom(7000)
    pkg_path = tmp_path / "P20080101000000000000001"
    pkg_path.write_bytes(build_package("save.bin", payload, display="T"))
    import contextlib
    import io

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = stfs.main(["list", str(pkg_path)])
    assert rc == 0
    assert "save.bin" in buf.getvalue()
    outdir = tmp_path / "out"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = stfs.main(["extract", str(pkg_path), str(outdir)])
    assert rc == 0
    extracted = outdir / pkg_path.stem / "save.bin"
    assert extracted.read_bytes() == payload


if __name__ == "__main__":
    from pathlib import Path
    import tempfile

    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        if fn.__code__.co_argcount:
            with tempfile.TemporaryDirectory() as td:
                fn(Path(td))
        else:
            fn()
        print(f"ok {fn.__name__}")
    print(f"{len(fns)} tests passed")
