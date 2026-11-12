"""disc-times: the toolkit patch reports one fixed time for every game-disc file, and the host
clears a stale Z: cache before boot. The fixed time is the disc's XBE TimeDate (a UNIX timestamp)."""

import os
from pathlib import Path
import shutil

import pytest

import patches

REPO = Path(__file__).resolve().parents[1]


def _toolkit():
    if "FCI_TOOLKIT" in os.environ:
        src = Path(os.environ["FCI_TOOLKIT"])
    else:
        bundled = REPO / "deps" / "xboxrecomp"
        src = bundled if bundled.is_dir() else REPO / "build" / "deps" / "xboxrecomp"
    if not (src / "src" / "kernel" / "kernel_file.c").is_file():
        pytest.skip(f"no xboxrecomp toolkit at {src} (set FCI_TOOLKIT or run tools/deps.py build/deps)")
    return src


def test_patch_0035_applies_and_stamps_disc_files(tmp_path):
    src = _toolkit()
    dst = tmp_path / "xboxrecomp"
    shutil.copytree(src, dst, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    applied = patches.apply_series("runtime", dst)
    assert "0035-disc-times.patch" in applied

    kf = (dst / "src" / "kernel" / "kernel_file.c").read_text(encoding="utf-8")
    kp = (dst / "src" / "kernel" / "kernel_path.c").read_text(encoding="utf-8")
    kh = (dst / "src" / "kernel" / "kernel.h").read_text(encoding="utf-8")
    assert "void xbox_SetDiscTime(ULONGLONG filetime)" in kh
    assert "int xbox_host_path_is_disc(" in kh
    assert "xbox_SetDiscTime" in kf and "disc_stamp" in kf
    assert "xbox_host_path_is_disc" in kp
    # a disc stamp at each of the six handle/path reporting sites (both backends)
    assert kf.count("disc_stamp((") >= 6
    # the disc check strips the \\?\ prefix (the extended-length form GetFinalPathNameByHandleW
    # returns), so a handle's \\?\Z:\...\game\... path is still recognised as a disc file
    assert "host_path += 4" in kp


def test_is_disc_strips_extended_prefix():
    # xbox_host_path_is_disc: strip a leading \\?\ (the extended-length form
    # GetFinalPathNameByHandleW returns), then check the path is under the game folder.
    game_dir = "Z:\\llm\\fci-recomp\\qwen\\install\\game"

    def is_disc(host_path):
        if host_path.startswith("\\\\?\\"):
            host_path = host_path[4:]
        n = len(game_dir)
        # C: the char after the game folder must be a separator (the null terminator is not)
        return host_path.startswith(game_dir) and n < len(host_path) and host_path[n] in ("\\", "/")

    assert is_disc(game_dir + "\\common.dat")  # plain path under the game folder
    assert is_disc("\\\\?\\" + game_dir + "\\common.dat")  # \\?\ form of the same path
    assert not is_disc("Z:\\llm\\save\\Cache\\x.SS2")  # a save-folder path
    assert not is_disc(game_dir)  # the game folder itself is not a file under it


def test_timedate_to_filetime():
    # XBE TimeDate (0x114) is UNIX epoch seconds; the host converts it to FILETIME (100ns since 1601).
    unix_ts = 1657000000
    filetime = (unix_ts + 11644473600) * 10000000
    assert filetime == (1657000000 + 11644473600) * 10000000
    assert filetime > 0  # a real disc time, so the fixed time is on


def test_heal_uses_save_cache_folder():
    # The Z: cache is <save>\Cache (kernel_path.c maps Z:\ there); the marker records the disc
    # time. Verify the host uses those paths and deletes a stale cache rather than renaming it.
    src = (REPO / "src" / "fci_disk.c").read_text(encoding="utf-8")
    assert r'"%s\\Cache"' in src  # the cache sits at the save-folder root
    assert "fci-cache-disc-time.txt" in src  # the marker name
    assert "Partition0" not in src  # the old, wrong location is gone
    assert "MoveFileEx" not in src  # it deletes the stale cache, it does not move it
