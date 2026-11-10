"""Q25: silent-success stubs in the patched kernel bridge.

A `bridge_*` whose final implementation does nothing returns success without
doing the job. Routed, that is invisible in a log. The 0028 patch makes such
stubs either unbridged (so an import takes the toolkit's standard unimplemented
path) or, if a title actually imports them, named on first call
(bridge_stub_log, driven by g_stub_imported).

A stub is judged by its final implementation, not by the bridge's shape: the
five crypto bridges (345-349) look like no-ops in the toolkit's kernel_crypto.c
but run fci-recomp's real DES / Montgomery exponentiation once
0030-kernel-crypto wires them to src/fci_xcrypt.c. The tests below follow each
bridge to that final body, so a real-work stub can never slip into
g_stub_imported on the strength of its name.
"""

import re
import shutil

import pytest

from conftest import toolkit_dir
import patches

# 0030-kernel-crypto + src/fci_xcrypt.c: these ordinals run real crypto, so they
# are not stubs no matter what the toolkit's kernel_crypto.c says on its own.
CRYPTO_REAL_WORK = {345, 346, 347, 348, 349}


@pytest.fixture(scope="module")
def patched_toolkit(tmp_path_factory):
    """the patched toolkit tree (pristine deps + patches/runtime)"""
    toolkit = toolkit_dir()
    if not (toolkit / "src" / "kernel" / "kernel_bridge.c").is_file():
        pytest.skip(f"no xboxrecomp toolkit at {toolkit}")
    dst = tmp_path_factory.mktemp("toolkit") / "toolkit"
    for part in ("src", "include"):
        shutil.copytree(toolkit / part, dst / part)
    patches.apply_series("runtime", dst)
    return dst


@pytest.fixture(scope="module")
def bridge_source(patched_toolkit):
    return (patched_toolkit / "src" / "kernel" / "kernel_bridge.c").read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def kernel_sources(patched_toolkit):
    """the patched kernel_*.c sources, keyed by stem (for the xbox_* bodies)"""
    kernel_dir = patched_toolkit / "src" / "kernel"
    return {p.stem: p.read_text(encoding="utf-8") for p in kernel_dir.glob("*.c")}


def _function_body(src, name):
    """the `{...}` body of `static void name(void)`, or None"""
    m = re.search(r"static void %s\(void\)\s*\{" % re.escape(name), src)
    if not m:
        return None
    depth, start = 0, m.end() - 1
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start : i + 1]
    return None


def _xbox_body(kernel_sources, name):
    """the `{...}` body of `... __stdcall|__fastcall name(...)` in the kernel sources"""
    pat = r"(?:__stdcall|__fastcall)\s+" + re.escape(name) + r"\s*\((?:[^)]*)\)\s*\{"
    for src in kernel_sources.values():
        m = re.search(pat, src, re.S)
        if not m:
            continue
        depth, start = 0, m.end() - 1
        for i in range(start, len(src)):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    return src[start : i + 1]
    return None


def _is_empty_success(body):
    """True if the body is only (void)STACK_ARG(n); and g_eax = 0;"""
    t = re.sub(r"\(void\)\s*STACK_ARG\(\d+\);", "", body)
    t = re.sub(r"g_eax\s*=\s*0;", "", t)
    return t.strip() in ("{}", "")


def _routed_bridges(src):
    """{bridge name: ordinal} from bridge_for_ordinal"""
    m = re.search(r"static bridge_func_t bridge_for_ordinal.*?\n\}", src, re.S)
    assert m, "bridge_for_ordinal not found"
    return {n: int(o) for o, n in re.findall(r"case\s+(\d+):\s*return\s+(bridge_\w+)", m.group(0))}


def _stub_table(src, var):
    m = re.search(r"static const unsigned %s\[\]\s*=\s*\{(.*?)\}" % var, src, re.S)
    assert m, f"{var} not found"
    return {int(x) for x in re.findall(r"\d+", m.group(1))}


def _final_body_is_noop(bridge_src, kernel_sources, ordinal):
    """True if the ordinal's bridge ends in a no-op, not real work.

    Follows the bridge to the xbox_* it calls and treats a body that calls an
    fci_* function as real work. The crypto bridges are overridden outright:
    they are real work in the shipped system even though the toolkit's
    kernel_crypto.c still shows them as no-ops.
    """
    if ordinal in CRYPTO_REAL_WORK:
        return False
    by_ordinal = {o: n for n, o in _routed_bridges(bridge_src).items()}
    name = by_ordinal.get(ordinal)
    if name is None:
        return False  # no bridge: not a routed stub
    body = _function_body(bridge_src, name)
    if body is None:
        return False
    if _is_empty_success(body):
        return True
    m = re.search(r"\b(xbox_\w+)\s*\(", body)
    if not m:
        # the bridge does its (no) work directly: a memset of zeros, a log, a
        # NULL out-pointer. Real work would show up as an fci_* call.
        return "fci_" not in body
    xbox_body = _xbox_body(kernel_sources, m.group(1))
    if xbox_body is None:
        return False  # cannot determine: be conservative
    return "fci_" not in xbox_body


def test_no_routed_empty_success_stub_is_silent(bridge_source):
    """every empty-success bridge that is routed must be in g_stub_imported (so it logs)"""
    routed = _routed_bridges(bridge_source)
    imported = _stub_table(bridge_source, "g_stub_imported")
    empty_success = [name for name in routed if _is_empty_success(_function_body(bridge_source, name))]
    silent = [n for n in empty_success if routed[n] not in imported]
    assert not silent, f"empty-success stubs routed without a g_stub_imported entry (silent success): {sorted(silent)}"


def test_unimported_stubs_are_unbridged(bridge_source):
    """every ordinal in g_stub_unimported is a stub that bridge_for_ordinal declines"""
    unimported = _stub_table(bridge_source, "g_stub_unimported")
    imported = _stub_table(bridge_source, "g_stub_imported")
    assert not (unimported & imported), "an ordinal is in both stub lists"
    m = re.search(r"static bridge_func_t bridge_for_ordinal.*?\n\}", bridge_source, re.S)
    assert "stub_unimported(ordinal)" in m.group(0), "bridge_for_ordinal lacks the unimported guard"
    by_ordinal = {o: n for n, o in _routed_bridges(bridge_source).items()}
    stale = [o for o in unimported if o not in by_ordinal]
    assert not stale, f"g_stub_unimported names ordinals with no bridge: {sorted(stale)}"


def test_dispatch_logs_imported_stubs(bridge_source):
    """the dispatch announces an imported stub before calling it, once per process"""
    m = re.search(r"static void kernel_thunk_dispatch_body\(void\)\s*\{.*?\n\}", bridge_source, re.S)
    assert m, "kernel_thunk_dispatch_body not found"
    assert "stub_imported(ordinal)" in m.group(0) and "bridge_stub_log(ordinal)" in m.group(0), (
        "dispatch does not call bridge_stub_log for imported stubs"
    )
    # one-shot per ordinal, not per call: a stub a title calls every frame must not log every frame
    assert "static uint8_t logged[XBOX_KERNEL_THUNK_TABLE_SIZE]" in bridge_source


def test_imported_stubs_are_final_body_noops(bridge_source, kernel_sources):
    """every ordinal in g_stub_imported ends in a no-op final body, not real work.

    This is the check a name-based test cannot do: a real-work stub (the crypto
    bridges, which run fci_* DES / modexp) must not be in g_stub_imported.
    """
    imported = _stub_table(bridge_source, "g_stub_imported")
    real_work = [o for o in imported if not _final_body_is_noop(bridge_source, kernel_sources, o)]
    assert not real_work, (
        f"g_stub_imported contains real-work stubs (their final body does the job): {sorted(real_work)}"
    )


def test_crypto_bridges_are_not_stubbed(bridge_source):
    """the crypto bridges do real work and must be in neither stub list"""
    unimported = _stub_table(bridge_source, "g_stub_unimported")
    imported = _stub_table(bridge_source, "g_stub_imported")
    leaked = CRYPTO_REAL_WORK & (unimported | imported)
    assert not leaked, f"crypto bridges (real DES / modexp via fci_*) are in a stub list: {sorted(leaked)}"


def test_stub_lists_match_the_imported_stubs(bridge_source):
    """g_stub_imported names exactly the no-op stubs a shipped title imports"""
    imported = _stub_table(bridge_source, "g_stub_imported")
    # the 13 no-op stubs both shipped titles import (build/lift/{default,evolution});
    # the crypto ones (345-349) are imported too but do real work, so they are absent
    expected = {47, 81, 83, 84, 85, 86, 87, 175, 178, 246, 253, 344, 359}
    assert imported == expected, (
        "g_stub_imported drifted from the shipped titles' imported no-op stubs: "
        f"missing={sorted(expected - imported)} extra={sorted(imported - expected)}"
    )
