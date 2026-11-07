"""Shared fixtures. Tests that need the toolkit or a compiler are skipped when there is none.

FCI_TOOLKIT    an xboxrecomp checkout at the pinned commit (default: the release's deps/xboxrecomp, else
               build/deps/xboxrecomp, which tools/deps.py fetches)
The compiler is the pinned llvm-mingw in toolchain/ (python -B tools/toolchain.py fetch).
"""

import os
from pathlib import Path
import sys

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))


@pytest.fixture(scope="session")
def lifter_toolkit(tmp_path_factory):
    """a copy of the toolkit with the lifter patches, importable as `tools.recomp`"""
    import lift

    source = toolkit_dir()
    if not (source / "tools" / "recomp").is_dir():
        pytest.skip(f"no xboxrecomp toolkit at {source} (set FCI_TOOLKIT or run tools/deps.py build/deps)")
    dst = tmp_path_factory.mktemp("toolkit") / "xboxrecomp"
    lift.prepare_toolkit(source, dst)
    sys.path.insert(0, str(dst))
    return dst


def toolkit_dir():
    if "FCI_TOOLKIT" in os.environ:
        return Path(os.environ["FCI_TOOLKIT"])
    bundled = REPO / "deps" / "xboxrecomp"
    return bundled if bundled.is_dir() else REPO / "build" / "deps" / "xboxrecomp"


@pytest.fixture(scope="session")
def compiler():
    """the pinned llvm-mingw (tools/toolchain.py)"""
    import toolchain

    try:
        return toolchain.discover()
    except toolchain.ToolchainError as e:
        pytest.skip(str(e))
