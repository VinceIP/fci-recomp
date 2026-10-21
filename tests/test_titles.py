"""tools/titles.py: each title's executables are told apart by their hashes, and its data is in place"""

import json

import titles


def test_every_executable_belongs_to_one_title():
    seen = [d for t in titles.TITLES.values() for d in t.images.values()]
    assert len(seen) == len(set(seen))
    for t in titles.TITLES.values():
        for image, digest in t.images.items():
            assert titles.of_digest(image, digest) == t
            assert image in titles.IMAGES


def test_each_title_has_its_disc_and_game_data():
    for t in titles.TITLES.values():
        disc = json.loads((t.data / "disc.json").read_text())
        assert disc["title_id"] == t.title_id
        assert {"default.xbe", "ige.xbe"} <= {n.lower() for n in disc["files"]}
        for image in ("default.xbe",):
            stem = image.split(".")[0]
            assert (t.data / f"{stem}.seeds.json").exists() and (t.data / f"{stem}.pins.json").exists()
        for image, digest in t.images.items():
            assert disc["files"][image]["sha256"] == digest


def test_names_differ_between_titles():
    for attr in ("name", "game_exe", "editor_exe", "appdata", "data"):
        values = [getattr(t, attr) for t in titles.TITLES.values()]
        assert len(values) == len(set(values)), attr
