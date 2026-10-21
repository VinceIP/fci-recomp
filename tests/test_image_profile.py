"""The map editor's compile profile, hand pins, and the launch helpers in src/fci_launch.c"""

import ctypes as c
import json
from pathlib import Path
import subprocess

import pytest

import image_profile
import sigmatch

REPO = Path(__file__).resolve().parents[1]
PROFILE = REPO / "data" / "ige.profile.json"


def test_editor_header_and_override_are_independent():
    header = image_profile.profile_header(json.loads(PROFILE.read_text()))
    assert "0x0005AD50u" in header
    assert "fci_override_0005FBA4" in header
    assert "0x004AB7E0u" not in header


def test_unregistered_image_rejected():
    profile = json.loads(PROFILE.read_text())
    profile["image_name"] = "update.xbe"
    with pytest.raises(ValueError):
        image_profile.profile_header(profile)


def test_other_image_does_not_get_main_pins():
    assert sigmatch.image_pins(b"different original executable", "signatures.toml") == {}


def test_explicit_pins_must_match_image():
    with pytest.raises(ValueError):
        sigmatch.image_pins(b"wrong image", "signatures.toml", REPO / "data" / "ige.pins.json")


@pytest.fixture(scope="module")
def launch_dll(msvc, tmp_path_factory):
    out = tmp_path_factory.mktemp("launch")
    argv = [
        msvc["cl"],
        "/nologo",
        "/O2",
        "/MT",
        "/LD",
        *["/I" + p for p in msvc["include"]],
        str(REPO / "src" / "fci_launch.c"),
        "/Fo:" + str(out / "launch.obj"),
        "/Fe:" + str(out / "launch.dll"),
        "/link",
        *["/LIBPATH:" + p for p in msvc["lib"]],
        "/EXPORT:fci_launch_target",
        "/EXPORT:fci_quote_arg",
    ]
    done = subprocess.run(argv, cwd=out, capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stdout + done.stderr
    dll = c.CDLL(str(out / "launch.dll"))
    dll.fci_launch_target.argtypes = [c.c_char_p]
    dll.fci_launch_target.restype = c.c_char_p
    dll.fci_quote_arg.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t), c.c_char_p]
    dll.fci_quote_arg.restype = c.c_int
    return dll


def test_original_disc_targets(launch_dll):
    for path, expected in [
        (r"\Device\CdRom0;default.xbe", b"default.xbe"),
        (r"\Device\CdRom0;ige.xbe", b"ige.xbe"),
        (r"\device\cdrom0\IGE.XBE", b"ige.xbe"),
    ]:
        assert launch_dll.fci_launch_target(path.encode()) == expected


def test_uncompiled_and_non_disc_targets_rejected(launch_dll):
    for path in [
        "",
        r"D:\ige.xbe",
        r"\Device\CdRom0;update.xbe",
        r"\Device\Harddisk0;ige.xbe",
        r"\Device\CdRom0;..\ige.xbe",
        r"\Device\CdRom0;ige.xbe.extra",
        r"\Device\CdRom00;ige.xbe",
    ]:
        assert launch_dll.fci_launch_target(path.encode()) is None, path


def test_windows_argument_roundtrip(launch_dll):
    parse = c.windll.shell32.CommandLineToArgvW
    parse.argtypes = [c.c_wchar_p, c.POINTER(c.c_int)]
    parse.restype = c.POINTER(c.c_wchar_p)
    free = c.windll.kernel32.LocalFree
    free.argtypes = [c.c_void_p]
    free.restype = c.c_void_p
    args = [
        "program.exe",
        "",
        "folder with spaces",
        'embedded"quote',
        "trailing\\",
        'slashes\\\\"quote',
        "RECOMP_FCI_XBR=1",
    ]
    buf, used = c.create_string_buffer(4096), c.c_size_t()
    for arg in args:
        assert launch_dll.fci_quote_arg(buf, len(buf), c.byref(used), arg.encode()) == 1
    count = c.c_int()
    parsed = parse(buf.value.decode(), c.byref(count))
    try:
        assert [parsed[i] for i in range(count.value)] == args
    finally:
        free(parsed)


def test_argument_overflow_fails_without_overwrite(launch_dll):
    buf, used = c.create_string_buffer(b"ABCDEFGH", 9), c.c_size_t()
    assert launch_dll.fci_quote_arg(buf, 4, c.byref(used), b"too long") == 0
    assert buf.raw[4:] == b"EFGH\0"
    assert used.value < 4


def test_title_names_reach_the_header():
    import titles

    profile = json.loads(PROFILE.read_text())
    evo = titles.TITLES["evolution"]
    profile.update(title_name=evo.name, game_exe=evo.game_exe, editor_exe=evo.editor_exe, appdata=evo.appdata)
    header = image_profile.profile_header(profile)
    assert '#define FCI_TITLE_NAME "Far Cry Instincts Evolution"' in header
    assert '#define FCI_EDITOR_EXE_NAME "FarCryEvolutionEditor.exe"' in header
    assert r'#define FCI_APPDATA "fci-recomp\\evolution"' in header  # a C string's escaped backslash
    profile["game_exe"] = 'x.exe" injected'
    with pytest.raises(ValueError):
        image_profile.profile_header(profile)


def test_evolution_profile_constants_and_hook_names():
    evo = json.loads((REPO / "data" / "evolution" / "default.profile.json").read_text())
    header = image_profile.profile_header(evo)
    assert "#define FCI_GAMEVIEW_INPUT 0x000008C8u" in header
    assert "fci_override_00362594" in header
    assert evo["replacements"]["sub_00196D41"][:3:2] == ["gate", "00220200"]  # Instincts' movie gate
    bad = dict(evo, constants={"OTHER": ["0x00000001", "not an FCI_ name"]})
    with pytest.raises(ValueError):
        image_profile.profile_header(bad)
    bad = dict(evo, replacements=dict(evo["replacements"], sub_00075C3E=["prehook", "why", "not hex"]))
    with pytest.raises(ValueError):
        image_profile.profile_header(bad)
