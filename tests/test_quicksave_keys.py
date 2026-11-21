"""The quick save and quick load keys (quicksave-key, quickload-key in fci.ini)."""

from pathlib import Path
import re

REPO = Path(__file__).resolve().parent.parent


def test_ini_template_has_both_keys():
    ini = (REPO / "src" / "fci.ini").read_text(encoding="utf-8")
    assert "quicksave-key" in ini, "fci.ini template is missing quicksave-key"
    assert "quickload-key" in ini, "fci.ini template is missing quickload-key"


def test_config_table_maps_both_keys():
    src = (REPO / "src" / "fci_config.c").read_text(encoding="utf-8")
    # the keys table maps the ini keys to the RECOMP_* env vars the input path reads
    assert re.search(r'\{"quicksave-key",\s*"RECOMP_QUICKSAVE_KEY"\}', src), (
        "fci_config.c keys table is missing quicksave-key"
    )
    assert re.search(r'\{"quickload-key",\s*"RECOMP_QUICKLOAD_KEY"\}', src), (
        "fci_config.c keys table is missing quickload-key"
    )
