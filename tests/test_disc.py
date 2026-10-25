"""tools/disc.py on a small synthetic disc image (no game data): reading XDVDFS, checking, copying"""

import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import disc  # noqa: E402
import titles  # noqa: E402

SECTOR = disc.SECTOR


def dir_table(entries):
    """entries: (name, sector, size, is_dir); a right-leaning tree, each entry's right child the next"""
    blobs = []
    for name, sector, size, is_dir in entries:
        blobs.append([name.encode(), sector, size, 0x10 if is_dir else 0x20])
    out, offsets, at = b"", [], 0
    for name, *_ in blobs:
        offsets.append(at)
        at += (14 + len(name) + 3) & ~3
    for i, (name, sector, size, attrs) in enumerate(blobs):
        right = offsets[i + 1] // 4 if i + 1 < len(blobs) else 0
        entry = struct.pack("<HHIIBB", 0, right, sector, size, attrs, len(name)) + name
        out += entry + b"\xff" * (((len(entry) + 3) & ~3) - len(entry))
    return out + b"\xff" * (-len(out) % SECTOR)


def build_image(path, files):
    """files: {"a.bin": bytes, "dir/b.bin": bytes}, one level of folders"""
    image = bytearray(34 * SECTOR)
    data_at = 40
    blobs = {}
    for name, content in files.items():
        blobs[name] = (data_at, len(content))
        image += b"\0" * ((data_at * SECTOR) - len(image)) + content
        image += b"\0" * (-len(image) % SECTOR)
        data_at = len(image) // SECTOR
    folders = sorted({n.split("/")[0] for n in files if "/" in n})
    sub_at = 34
    root = []
    sub_tables = {}
    for folder in folders:
        inner = [(n.split("/", 1)[1], *blobs[n], False) for n in files if n.startswith(folder + "/")]
        table = dir_table(inner)
        sub_tables[folder] = (sub_at, table)
        root.append((folder, sub_at, len(table), True))
        sub_at += len(table) // SECTOR
    root += [(n, *blobs[n], False) for n in files if "/" not in n]
    root_table = dir_table(root)
    head = bytearray(SECTOR)
    head[:20] = disc.MAGIC
    struct.pack_into("<II", head, 0x14, 33, len(root_table))
    head[0x7EC : 0x7EC + 20] = disc.MAGIC
    image[32 * SECTOR : 33 * SECTOR] = head
    image[33 * SECTOR : 33 * SECTOR + len(root_table)] = root_table
    for at, table in sub_tables.values():
        image[at * SECTOR : at * SECTOR + len(table)] = table
    Path(path).write_bytes(bytes(image))


FILES = {"default.xbe": b"x" * 5000, "Videos/intro.bik": b"v" * 3000, "skip.xbe": b"s" * 10}


def fake_title(tmp_path, monkeypatch, files):
    """a title whose data folder holds the manifest of files and whose default.xbe is files' one"""
    manifest(tmp_path, files)
    t = titles.Title(
        key="fake",
        name="Fake",
        folder="Fake",
        title_id="55530008",
        data=tmp_path,
        game_exe="Fake.exe",
        editor_exe="FakeEditor.exe",
        appdata="fake",
        images={"default.xbe": hashlib.sha256(files["default.xbe"]).hexdigest()},
    )
    monkeypatch.setitem(titles.TITLES, "fake", t)
    return t


def manifest(tmp_path, files):
    m = {
        "title_id": "55530008",
        "version": 1,
        "not_copied": ["skip.xbe"],
        "files": {n: {"size": len(c), "sha256": hashlib.sha256(c).hexdigest()} for n, c in files.items()},
    }
    p = tmp_path / "disc.json"
    p.write_text(json.dumps(m))
    return p


def test_image_files_and_contents(tmp_path):
    build_image(tmp_path / "game.iso", FILES)
    image = disc.Image(tmp_path / "game.iso")
    assert set(image.files) == set(FILES)
    with image.open("Videos/intro.bik") as f:
        assert f.read(1 << 20) == FILES["Videos/intro.bik"]


def test_verify_and_extract(tmp_path, monkeypatch):
    build_image(tmp_path / "game.iso", FILES)
    title = fake_title(tmp_path, monkeypatch, FILES)
    image = disc.Image(tmp_path / "game.iso")
    assert disc.identify(image) == title
    assert disc.verify(image, quiet=True) == []
    disc.extract(image, tmp_path / "out")
    assert (tmp_path / "out" / "Videos" / "intro.bik").read_bytes() == FILES["Videos/intro.bik"]
    assert not (tmp_path / "out" / "skip.xbe").exists()


def test_verify_reports_a_changed_file(tmp_path, monkeypatch):
    changed = dict(FILES, **{"Videos/intro.bik": b"w" * 3000})
    build_image(tmp_path / "game.iso", changed)
    title = fake_title(tmp_path, monkeypatch, FILES)
    image = disc.Image(tmp_path / "game.iso")
    assert disc.verify(image, quiet=True, title=title) == ["different: Videos/intro.bik"]


def test_an_unknown_default_xbe_is_no_supported_title(tmp_path):
    build_image(tmp_path / "game.iso", FILES)
    image = disc.Image(tmp_path / "game.iso")
    assert disc.identify(image) is None
    assert disc.verify(image, quiet=True) == ["default.xbe: not the supported release of any game this project builds"]


def test_not_a_disc(tmp_path):
    (tmp_path / "x.iso").write_bytes(b"\0" * 100000)
    try:
        disc.Image(tmp_path / "x.iso")
    except disc.DiscError:
        return
    raise AssertionError("expected DiscError")


def xbe(title_id, version, region=1, name="Far Cry"):
    """a default.xbe header with just a certificate"""
    head = bytearray(0x1000)
    head[:4] = b"XBEH"
    struct.pack_into("<II", head, 0x104, 0x10000, 0)
    struct.pack_into("<I", head, 0x118, 0x10000 + 0x200)
    struct.pack_into("<I", head, 0x200 + 0x08, title_id)
    head[0x20C : 0x20C + 2 * len(name)] = name.encode("utf-16-le")
    struct.pack_into("<I", head, 0x200 + 0xA0, region)
    struct.pack_into("<I", head, 0x200 + 0xAC, version)
    return bytes(head)


def test_certificate_says_what_the_disc_is(tmp_path):
    build_image(tmp_path / "game.iso", {"default.xbe": xbe(0x55530008, 4, region=4)})
    cert = disc.certificate(disc.Image(tmp_path / "game.iso"))
    assert cert == {"title_id": "55530008", "name": "Far Cry", "version": 4, "region": "PAL: Europe and Australia"}


def test_why_a_disc_is_not_supported(tmp_path):
    def why(files):
        build_image(tmp_path / "game.iso", files)
        return disc.why_unsupported(disc.Image(tmp_path / "game.iso"))

    assert "version 4 (PAL: Europe and Australia), and only version 1" in why({"default.xbe": xbe(0x55530008, 4, 4)})
    assert "was modified" in why({"default.xbe": xbe(0x55530060, 3)})
    assert "title 4D530004" in why({"default.xbe": xbe(0x4D530004, 1, name="Halo")})
    assert "missing or unreadable" in why({"default.xbe": b"MZ" + b"\0" * 100})
