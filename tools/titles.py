"""The games this project builds: each title's names, data folder and the executables it supports.

A title is recognised by its executable's SHA-256, so the tools take a disc's default.xbe or ige.xbe and
find the rest here. Far Cry Instincts keeps its data in data/; every other title has data/<key>/.
"""

from dataclasses import dataclass, field
import hashlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Title:
    key: str
    name: str  # in windows and messages
    folder: str  # the install folder
    title_id: str  # the XBE certificate's, hex
    data: Path  # disc.json and <image>.seeds/pins/profile.json
    game_exe: str
    editor_exe: str
    appdata: str  # under %APPDATA%: profile, logs, shader-cache
    images: dict = field(default_factory=dict)  # image name -> SHA-256

    @property
    def install(self):
        """the install folder's name"""
        return self.folder

    def exe(self, image):
        return self.editor_exe if image == "ige.xbe" else self.game_exe


TITLES = {
    "instincts": Title(
        key="instincts",
        name="Far Cry Instincts",
        folder="FarCryInstincts",
        title_id="55530008",
        data=REPO / "data",
        game_exe="FarCryInstincts.exe",
        editor_exe="FarCryInstinctsEditor.exe",
        appdata="fci-recomp",
        images={
            "default.xbe": "9796c818c350a05e6756bd6f87e7cbe5e028e9b7b30563c674df9ea019fc4ae1",
            "ige.xbe": "8d6f9da1c2d9e588f8e4eaf4ad8a68a0212ca1310adb76616bf31e8f489b1968",
        },
    ),
    "evolution": Title(
        key="evolution",
        name="Far Cry Instincts Evolution",
        folder="FarCryEvolution",
        title_id="55530060",
        data=REPO / "data" / "evolution",
        game_exe="FarCryEvolution.exe",
        editor_exe="FarCryEvolutionEditor.exe",
        appdata="fci-recomp\\evolution",
        images={
            "default.xbe": "ced4d535bbfcc66567d0f95772d14f965f970fdeaf39cfbd556142c7c2ada68d",
            "ige.xbe": "0c8611a365e1106d4cc34b46d42829ca5b0744597972e572c90d1b4dd02b1e25",
        },
    ),
}
IMAGES = ("default.xbe", "ige.xbe")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def of_image(image, xbe):
    """the title whose supported release xbe is (as image), or None"""
    digest = sha256(xbe)
    return next((t for t in TITLES.values() if t.images.get(image) == digest), None)


def of_digest(image, digest):
    return next((t for t in TITLES.values() if t.images.get(image) == digest), None)
