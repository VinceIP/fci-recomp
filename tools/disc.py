"""Check a game disc (Far Cry Instincts or Evolution) and copy the game files from it.

  python -B tools/disc.py verify <disc>              check every file against the known release
  python -B tools/disc.py extract <disc> <folder>    check, then copy the game files into <folder>

<disc> is an extracted copy of the disc (a folder with default.xbe in it) or a disc image: an XISO, or a
full dump of the disc (redump style), which has the game partition further in. Every file is compared by
size and SHA-256 with the title's disc.json (tools/titles.py; the title is found from default.xbe), the one
release of each game this project supports. Nothing is written unless all of it matches. Three executables
on the disc are not copied: dashupdate.xbe (a Microsoft dashboard update) and two content updaters the game
never runs here.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import events  # noqa: E402
import titles  # noqa: E402

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# where the game partition starts in a disc image: an XISO, then the full dumps of the three disc layouts
PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)


class DiscError(Exception):
    pass


class Image:
    """the files of an Xbox game partition (XDVDFS) in a disc image"""

    def __init__(self, path):
        self.f = open(path, "rb")
        for base in PARTITIONS:
            self.f.seek(base + 32 * SECTOR)
            head = self.f.read(SECTOR)
            if head[:20] == MAGIC and head[0x7EC : 0x7EC + 20] == MAGIC:
                self.base = base
                root_sector, root_size = struct.unpack_from("<II", head, 0x14)
                self.files = {}
                self._walk(root_sector, root_size, "")
                return
        raise DiscError(f"{path} is not an Xbox disc image")

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def _walk(self, sector, size, prefix):
        table = self._read(sector, size)
        pending = [0]
        while pending:
            at = pending.pop()
            if at + 14 > len(table):
                continue
            left, right, start, length, attrs, name_len = struct.unpack_from("<HHIIBB", table, at)
            if left == 0xFFFF:  # padding at the end of a sector
                continue
            name = table[at + 14 : at + 14 + name_len].decode("ascii")
            if left:
                pending.append(left * 4)
            if right:
                pending.append(right * 4)
            path = prefix + name
            if attrs & 0x10:
                if length:
                    self._walk(start, length, path + "/")
            else:
                self.files[path] = (start, length)

    def open(self, path):
        start, length = self.files[path]
        self.f.seek(self.base + start * SECTOR)
        return _Bounded(self.f, length)


class _Bounded:
    """one file inside the image, read like an open file"""

    def __init__(self, f, left):
        self.f, self.left = f, left

    def read(self, n):
        data = self.f.read(min(n, self.left))
        self.left -= len(data)
        return data

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class Folder:
    """an extracted copy of the disc"""

    def __init__(self, path):
        self.root = Path(path)
        if not (self.root / "default.xbe").is_file():
            raise DiscError(f"{path} has no default.xbe: not an extracted game disc")
        self.files = {p.relative_to(self.root).as_posix(): None for p in self.root.rglob("*") if p.is_file()}

    def open(self, path):
        return open(self.root / path, "rb")


def open_disc(path):
    p = Path(path)
    if p.is_dir():
        return Folder(p)
    if p.is_file():
        return Image(p)
    raise DiscError(f"{path} does not exist")


def hash_file(f, progress):
    h = hashlib.sha256()
    n = 0
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
        n += len(block)
        progress(len(block))
    return h.hexdigest(), n


def identify(disc):
    """the title whose supported default.xbe this disc has, or None"""
    name = next((k for k in disc.files if k.lower() == "default.xbe"), None)
    if name is None:
        return None
    with disc.open(name) as f:
        digest, _ = hash_file(f, lambda n: None)
    return titles.of_digest("default.xbe", digest)


REGIONS = {1: "North America", 2: "Japan", 4: "PAL: Europe and Australia"}


def certificate(disc):
    """what default.xbe's certificate says it is: {"title_id": hex, "name", "version", "region"}, or None
    when there is no readable default.xbe"""
    name = next((k for k in disc.files if k.lower() == "default.xbe"), None)
    if name is None:
        return None
    with disc.open(name) as f:
        head = f.read(64 * 1024)
    if head[:4] != b"XBEH" or len(head) < 0x120:
        return None
    base, cert = struct.unpack_from("<I", head, 0x104)[0], struct.unpack_from("<I", head, 0x118)[0]
    at = cert - base
    if at < 0 or at + 0xB0 > len(head):
        return None
    title_id, region, version = (struct.unpack_from("<I", head, at + o)[0] for o in (0x08, 0xA0, 0xAC))
    raw = head[at + 0x0C : at + 0x0C + 80].decode("utf-16-le", errors="replace")
    regions = [n for bit, n in REGIONS.items() if region & bit]
    return {
        "title_id": f"{title_id:08X}",
        "name": raw.split("\0")[0].strip(),
        "version": version,
        "region": ", ".join(regions) or f"0x{region:08X}",
    }


def why_unsupported(disc):
    """why this disc is not a supported release, in a sentence for the player"""
    cert = certificate(disc)
    if cert is None:
        return "its default.xbe is missing or unreadable; use a complete, unmodified dump of the disc"
    for title in titles.TITLES.values():
        if cert["title_id"] != title.title_id.upper():
            continue
        supported = manifest_of(title)["version"]
        if cert["version"] != supported:
            return (
                f"it is {title.name} version {cert['version']} ({cert['region']}), and only version "
                f"{supported} is supported for now"
            )
        return (
            f"it is {title.name} version {supported}, but its default.xbe was modified (a patched XISO, "
            "for example); use an unmodified dump of the disc"
        )
    named = f"{cert['name']} " if cert["name"] else ""
    return f"it is {named}(title {cert['title_id']}), not Far Cry Instincts or Far Cry Instincts Evolution"


def manifest_of(title):
    return json.loads((title.data / "disc.json").read_text())


def verify(disc, quiet=False, title=None):
    """the problems with this disc; an empty list means it is the supported release of title (by default,
    the one its default.xbe belongs to)"""
    title = title or identify(disc)
    if title is None:
        return ["default.xbe: not the supported release of any game this project builds"]
    manifest = manifest_of(title)
    expected = manifest["files"]
    have = {k.lower(): k for k in disc.files}
    problems = []
    total = sum(v["size"] for v in expected.values())
    done = [0]

    def progress(n):
        before = done[0] * 100 // total
        done[0] += n
        if not quiet and done[0] * 100 // total != before:
            print(f"\r  checking {done[0] * 100 // total:3d}%", end="", flush=True)
        events.progress("disc", done[0], 2 * total, "Checking the disc's files")

    for name, want in expected.items():
        actual = have.get(name.lower())
        if actual is None:
            problems.append(f"missing: {name}")
            done[0] += want["size"]
            continue
        with disc.open(actual) as f:
            digest, size = hash_file(f, progress)
        if size != want["size"] or digest != want["sha256"]:
            problems.append(f"different: {name}")
    if not quiet:
        print()
    extra = sorted(set(have) - {k.lower() for k in expected})
    problems += [f"unexpected file: {have[e]}" for e in extra]
    return problems


def extract(disc, dst, title=None):
    manifest = manifest_of(title or identify(disc))
    skip = {s.lower() for s in manifest["not_copied"]}
    have = {k.lower(): k for k in disc.files}
    dst = Path(dst)
    copied = [n for n in manifest["files"] if n.lower() not in skip]
    total, done = sum(manifest["files"][n]["size"] for n in copied), 0
    for name in copied:
        target = dst / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with disc.open(have[name.lower()]) as src, open(target, "wb") as out:
            for block in iter(lambda: src.read(1 << 20), b""):
                out.write(block)
                done += len(block)
                events.progress("disc", total + done, 2 * total, "Copying the game's files")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="action", required=True)
    v = sub.add_parser("verify")
    v.add_argument("disc")
    e = sub.add_parser("extract")
    e.add_argument("disc")
    e.add_argument("folder")
    a = ap.parse_args()
    try:
        disc = open_disc(a.disc)
    except DiscError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    title = identify(disc)
    problems = verify(disc, title=title)
    if problems:
        print("This is not a supported release:", file=sys.stderr)
        for p in problems[:20]:
            print("  " + p, file=sys.stderr)
        return 1
    print(f"The disc matches the supported release of {title.name}.")
    if a.action == "extract":
        if Path(a.folder).exists() and any(Path(a.folder).iterdir()):
            print(f"error: {a.folder} is not empty", file=sys.stderr)
            return 1
        extract(disc, a.folder, title)
        print(f"Game files copied to {a.folder}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
