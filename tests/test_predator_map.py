"""tools/predator_map.py: embedded-index parse, 12-byte .fat writer, size rule, sig.

The 12-byte .fat writer is proven byte-identical against the three Evolution
samples in build/qwen-in/ (test_fat_writer_reproduces_evolution_samples).
"""

import hashlib
import hmac
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import predator_map  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_stfs import build_package as build_stfs_package  # noqa: E402


# ---------------------------------------------------------------------------
# helpers: build a synthetic FC2_Save with a valid 16-byte index
# ---------------------------------------------------------------------------
def make_index(entries):
    """Build the 16-byte DAT2 index bytes from (offset, size, hash) rows.

    ``entries`` must be in hash-ascending order and tile from 0.
    """
    out = bytearray()
    out += b"DAT2"
    out += struct.pack("<I", len(entries))
    for off, sz, h in entries:
        out += struct.pack("<IIII", off, sz, sz, h)  # size repeated
    return bytes(out)


def make_fc2_save(name, index, cry):
    """Build a synthetic FC2_Save: header + name (UTF-16BE) + index + CRY."""
    out = bytearray()
    out += struct.pack(">I", 1)  # version
    out += b"\x00" * 0x10  # pad to 0x14
    out += name.encode("utf-16-be") + b"\x00\x00"
    out += index
    out += cry
    return bytes(out)


def build_pkg(tmp_path, fc2, title_id=0x555307DC, display="Test Map"):
    data = build_stfs_package("FC2_Save", fc2, title_id, display)
    p = tmp_path / "P20080150000000000000001"
    p.write_bytes(data)
    return p


def tiling_entries(chunks):
    """chunks: list of payload bytes -> (offset, size, hash) rows in hash order.

    Assigns a distinct ascending hash to each chunk (by index) and lays the
    chunks out back-to-back from 0. Returns (entries, dat).
    """
    rows = []
    dat = bytearray()
    for i, buf in enumerate(chunks):
        rows.append((len(dat), len(buf), 0x1000 + i))  # hash ascending by index
        dat += buf
    return rows, bytes(dat)


# ---------------------------------------------------------------------------
# signature
# ---------------------------------------------------------------------------
def test_sig_is_double_hmac_sha1_zero_key():
    data = b"test payload"
    sig = predator_map.make_sig(data)
    assert len(sig) == 40
    K = b"\x00" * 16
    inner = hmac.new(K, data, hashlib.sha1).digest()
    outer = hmac.new(K, inner, hashlib.sha1).digest()
    assert sig == outer + inner


def test_sig_differs_per_input():
    assert predator_map.make_sig(b"a") != predator_map.make_sig(b"b")


def test_sig_matches_evolution_sample():
    sample = Path("build/qwen-in/b89/evolution/SyslinkMaps/aac01e6240c7805a101126205924374")
    dat = sample / "aac01e6240c7805a101126205924374.dat"
    sig = sample / "aac01e6240c7805a101126205924374.dat.sig"
    if not dat.exists():
        return
    assert predator_map.make_sig(dat.read_bytes()) == sig.read_bytes()


# ---------------------------------------------------------------------------
# 12-byte .fat writer: proven byte-identical against the Evolution samples
# ---------------------------------------------------------------------------
def test_fat_writer_reproduces_evolution_samples():
    base = Path("build/qwen-in/b89/evolution/SyslinkMaps")
    if not base.exists():
        return  # samples not present
    checked = 0
    for d in sorted(base.iterdir()):
        if not d.is_dir():
            continue
        fat = (d / f"{d.name}.fat").read_bytes()
        entries = predator_map.parse_fat(fat)
        assert predator_map.make_fat(entries) == fat, f"writer did not reproduce {d.name}.fat"
        checked += 1
    assert checked >= 3, f"expected at least 3 Evolution samples, found {checked}"


def test_parse_fat_roundtrip():
    entries = [(0, 10, 0xA), (10, 20, 0xB), (30, 5, 0xC)]
    raw = predator_map.make_fat(entries)
    assert predator_map.parse_fat(raw) == entries


def test_parse_fat_rejects_bad_magic():
    try:
        predator_map.parse_fat(b"XXXX" + struct.pack("<I", 0))
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for a bad magic")


# ---------------------------------------------------------------------------
# embedded index parse (16-byte entries)
# ---------------------------------------------------------------------------
def test_parse_embedded_index_valid():
    rows, dat = tiling_entries([b"CRY\x01" + b"x" * 50, b"chunk2", b"chunk3"])
    fc2 = make_fc2_save("M", make_index(rows), dat)
    entries, end = predator_map.parse_embedded_index(fc2)
    assert end == len(dat)
    # hashes ascending, tiling from 0
    hashes = [h for (_o, _s, h) in entries]
    assert hashes == sorted(hashes)
    byoff = sorted(entries, key=lambda e: e[0])
    prev = 0
    for off, sz, _h in byoff:
        assert off == prev
        prev = off + sz


def test_parse_embedded_index_rejects_size_mismatch():
    rows, dat = tiling_entries([b"CRY\x01abc", b"def"])
    # corrupt: make the third field differ from the second in the first entry
    idx = bytearray(make_index(rows))
    # first entry starts at offset 8; fields are off(4) size(4) size(4) hash(4)
    idx[8 + 4] ^= 0xFF  # flip a byte of the first size field's partner
    fc2 = make_fc2_save("M", bytes(idx), dat)
    try:
        predator_map.parse_embedded_index(fc2)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for a size-field mismatch")


def test_parse_embedded_index_rejects_non_ascending_hashes():
    # two chunks but hashes out of order
    rows = [(0, 3, 0xB), (3, 3, 0xA)]
    dat = b"CRY"
    fc2 = make_fc2_save("M", make_index(rows), dat)
    try:
        predator_map.parse_embedded_index(fc2)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for non-ascending hashes")


def test_parse_embedded_index_rejects_tiling_gap():
    # gap: second entry starts at 5, not 3
    rows = [(0, 3, 0xA), (5, 3, 0xB)]
    dat = b"CRYxxxx"
    fc2 = make_fc2_save("M", make_index(rows), dat)
    try:
        predator_map.parse_embedded_index(fc2)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for a tiling gap")


def test_parse_embedded_index_rejects_end_past_cry():
    rows, _ = tiling_entries([b"CRY\x01" + b"x" * 50])
    fc2 = make_fc2_save("M", make_index(rows), b"CRY\x01" + b"x" * 5)  # too short
    try:
        predator_map.parse_embedded_index(fc2)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError when the index end is past the CRY region")


# ---------------------------------------------------------------------------
# size rule (0x1F0000 cap)
# ---------------------------------------------------------------------------
def test_fit_pads_small_dat_with_0x01_and_extends_last_entry():
    # a dat smaller than MAP_SIZE: pad with 0x01, extend the highest-offset entry
    rows, dat = tiling_entries([b"CRY\x01" + b"a" * 100, b"bb"])
    new_dat, new_entries = predator_map.fit_to_map_size(dat, rows)
    assert len(new_dat) == predator_map.MAP_SIZE
    assert new_dat[len(dat) :] == bytes([predator_map.PAD_BYTE]) * (predator_map.MAP_SIZE - len(dat))
    # the extended entry (highest offset) now reaches MAP_SIZE
    byoff = sorted(new_entries, key=lambda e: e[0])
    assert byoff[-1][0] + byoff[-1][1] == predator_map.MAP_SIZE
    # hashes unchanged and still ascending
    hashes = [h for (_o, _s, h) in new_entries]
    assert hashes == sorted(hashes)


def test_fit_unchanged_at_map_size():
    rows = [(0, predator_map.MAP_SIZE, 0xA)]
    dat = b"\x00" * predator_map.MAP_SIZE
    new_dat, new_entries = predator_map.fit_to_map_size(dat, rows)
    assert new_dat is dat and new_entries == rows


def test_fit_refuses_oversize_by_default():
    rows = [(0, predator_map.MAP_SIZE + 1, 0xA)]
    dat = b"\x00" * (predator_map.MAP_SIZE + 1)
    try:
        predator_map.fit_to_map_size(dat, rows)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for an oversized dat")


def test_fit_allows_oversize_when_flagged():
    rows = [(0, predator_map.MAP_SIZE + 1, 0xA)]
    dat = b"\x00" * (predator_map.MAP_SIZE + 1)
    new_dat, new_entries = predator_map.fit_to_map_size(dat, rows, allow_oversize=True)
    assert new_dat is dat and new_entries == rows


def test_check_size_refuses_oversize():
    try:
        predator_map.check_size(predator_map.MAP_SIZE + 1, allow_oversize=False)
    except predator_map.MapError as e:
        assert hex(predator_map.MAP_SIZE) in str(e)
        return
    raise AssertionError("expected MapError for an oversized end")


def test_check_size_allows_when_flagged():
    predator_map.check_size(predator_map.MAP_SIZE + 1, allow_oversize=True)  # no raise


# ---------------------------------------------------------------------------
# SaveMeta.xbx
# ---------------------------------------------------------------------------
def test_save_meta_structure():
    meta = predator_map.make_save_meta("Test Map")
    assert len(meta) == 168
    assert meta[:2] == b"\xff\xfe"
    text = meta[2:].decode("utf-16-le").strip("\x00 ")
    assert text.startswith("Name=Test Map")


def test_save_meta_too_long_raises():
    try:
        predator_map.make_save_meta("X" * 200)
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for a too-long name")


# ---------------------------------------------------------------------------
# full conversion (synthetic, small -> padded to MAP_SIZE)
# ---------------------------------------------------------------------------
def test_convert_small_pads_to_map_size(tmp_path):
    rows, dat = tiling_entries([b"CRY\x01" + b"a" * 100, b"bb", b"cc"])
    fc2 = make_fc2_save("Player Map", make_index(rows), dat)
    pkg = build_pkg(tmp_path, fc2)
    out = tmp_path / "out"
    result = predator_map.convert(str(pkg), str(out), stem="mymap")
    dest = out / "mymap"
    for fn in ("mymap.dat", "mymap.fat", "mymap.dat.sig", "mymap.fat.sig", "SaveMeta.xbx", "SaveImage.xbx"):
        assert (dest / fn).exists(), f"missing {fn}"
    new_dat = (dest / "mymap.dat").read_bytes()
    assert len(new_dat) == predator_map.MAP_SIZE
    # the fat tiles the padded dat and ends at MAP_SIZE
    fat = (dest / "mymap.fat").read_bytes()
    entries = predator_map.parse_fat(fat)
    byoff = sorted(entries, key=lambda e: e[0])
    prev = 0
    for off, sz, _h in byoff:
        assert off == prev
        prev = off + sz
    assert prev == predator_map.MAP_SIZE
    # sigs are correct
    assert (dest / "mymap.dat.sig").read_bytes() == predator_map.make_sig(new_dat)
    assert (dest / "mymap.fat.sig").read_bytes() == predator_map.make_sig(fat)
    assert result["name"] == "Player Map"


def test_convert_oversize_fails_without_flag(tmp_path):
    # make a dat larger than MAP_SIZE
    big = b"CRY\x01" + b"a" * (predator_map.MAP_SIZE + 10)
    rows = [(0, len(big), 0xA)]
    fc2 = make_fc2_save("Big", make_index(rows), big)
    pkg = build_pkg(tmp_path, fc2)
    try:
        predator_map.convert(str(pkg), str(tmp_path / "out"))
    except predator_map.MapError as e:
        assert "allow-oversize" in str(e)
        return
    raise AssertionError("expected MapError for an oversized map without the flag")


def test_convert_oversize_writes_with_flag(tmp_path):
    big = b"CRY\x01" + b"a" * (predator_map.MAP_SIZE + 10)
    rows = [(0, len(big), 0xA)]
    fc2 = make_fc2_save("Big", make_index(rows), big)
    pkg = build_pkg(tmp_path, fc2)
    result = predator_map.convert(str(pkg), str(tmp_path / "out"), stem="bigmap", allow_oversize=True)
    dest = tmp_path / "out" / "bigmap"
    assert (dest / "bigmap.dat").read_bytes() == big
    assert result["dat_size"] == len(big)


def test_convert_no_fc2_save_raises(tmp_path):
    data = build_stfs_package("OtherFile", b"payload", 0x555307DC, "X")
    p = tmp_path / "P20080150000000000000002"
    p.write_bytes(data)
    try:
        predator_map.convert(str(p), str(tmp_path / "out"))
    except predator_map.MapError:
        return
    raise AssertionError("expected MapError for a package without FC2_Save")


def test_convert_cli(tmp_path, capsys):
    rows, dat = tiling_entries([b"CRY\x01" + b"a" * 50, b"bb"])
    fc2 = make_fc2_save("CLI Map", make_index(rows), dat)
    pkg = build_pkg(tmp_path, fc2)
    out = tmp_path / "out"
    predator_map.main(["convert", str(pkg), str(out), "--stem", "climap"])
    assert (out / "climap" / "climap.dat").read_bytes()[:4] == b"CRY\x01"
    assert "CLI Map" in capsys.readouterr().out


def test_convert_cli_oversize_reports_error(tmp_path, capsys):
    big = b"CRY\x01" + b"a" * (predator_map.MAP_SIZE + 10)
    rows = [(0, len(big), 0xA)]
    fc2 = make_fc2_save("Big", make_index(rows), big)
    pkg = build_pkg(tmp_path, fc2)
    rc = predator_map.main(["convert", str(pkg), str(tmp_path / "out")])
    assert rc == 1
    assert "allow-oversize" in capsys.readouterr().err
