"""The compile profile of an executable other than default.xbe (the map editor: data/ige.profile.json).

  python -B tools/image_profile.py <profile.json> <original.xbe> <case/inputs>

Writes image-profile.json and image-profile.h, the image's entry point, size and replacements, into a
staged build; tools/compile.py includes the header in every file it compiles.
"""

import hashlib
import json
from pathlib import Path
import re
import sys


def profile_header(profile):
    name = profile["image_name"]
    if name not in ("default.xbe", "ige.xbe"):
        raise ValueError("unregistered disc image")
    values = {
        "FCI_ENTRY_VA": int(profile["entry_va"], 16),
        "FCI_XBE_SIZE": int(profile["image_bytes"]),
        "FCI_DESCRIPTOR_JUMP": int(profile["descriptor_jump"], 16),
        "FCI_CODE_END": int(profile["code_end"], 16),
    }
    if any(v <= 0 or v > 0xFFFFFFFF for v in values.values()):
        raise ValueError("image value outside uint32")
    handler = int(profile["descriptor_helper"], 16)
    expected = f"sub_{handler:08X}"
    if expected not in profile["replacements"]:
        raise ValueError("descriptor override missing")
    for fn, action in profile["replacements"].items():
        if not re.fullmatch(r"sub_[0-9A-F]{8}", fn) or action[0] not in ("override", "prehook", "quarantine", "gate"):
            raise ValueError("invalid replacement")
        if len(action) > 2 and (action[0] not in ("prehook", "gate") or not re.fullmatch(r"[0-9A-F]{8}", action[2])):
            raise ValueError("invalid replacement hook")
    lines = [f"#define {k} 0x{v:08X}u" for k, v in values.items()]
    lines += [
        f'#define FCI_IMAGE_FILENAME "{name}"',
        f"#define fci_override_004B03B4 fci_override_{handler:08X}",
        f"#define FCI_HAS_SUPPLEMENT {int(bool(profile.get('supplemental_entries')))}",
    ]
    # the title's names (tools/titles.py, added by build.py; src/fci_image.h has Instincts')
    for key, define in (
        ("title_name", "FCI_TITLE_NAME"),
        ("game_exe", "FCI_GAME_EXE_NAME"),
        ("editor_exe", "FCI_EDITOR_EXE_NAME"),
        ("appdata", "FCI_APPDATA"),
    ):
        if key in profile:
            if not re.fullmatch(r"[A-Za-z0-9 ._\\-]+", profile[key]):
                raise ValueError(f"invalid {key}")
            lines.append(f'#define {define} "{profile[key].replace(chr(92), chr(92) * 2)}"')
    # addresses a runtime feature needs in this executable, each with its evidence: {"FCI_X": ["0x...", "why"]}
    for define, (value, _why) in sorted(profile.get("constants", {}).items()):
        if not re.fullmatch(r"FCI_[A-Z0-9_]+", define) or not re.fullmatch(r"0x[0-9A-F]{8}", value):
            raise ValueError(f"invalid constant {define}")
        lines.append(f"#define {define} {value}u")
    lines.append("")
    return "\n".join(lines)


def stage_profile(profile_path, image_path, inputs, title=None):
    profile = json.loads(Path(profile_path).read_text())
    if title:
        profile.update(
            title_name=title.name, game_exe=title.game_exe, editor_exe=title.editor_exe, appdata=title.appdata
        )
    image = Path(image_path).read_bytes()
    if len(image) != profile["image_bytes"] or hashlib.sha256(image).hexdigest() != profile["image_sha256"]:
        raise ValueError("XBE does not match compile profile")
    header = profile_header(profile)
    inputs = Path(inputs)
    inputs.mkdir(parents=True, exist_ok=True)
    (inputs / "image-profile.json").write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8", newline="\n")
    (inputs / "image-profile.h").write_text(header, encoding="utf-8", newline="\n")
    return profile


if __name__ == "__main__":
    stage_profile(*sys.argv[1:])
