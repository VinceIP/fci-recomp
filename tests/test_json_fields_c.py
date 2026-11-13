"""Unit test for the host list's JSON field readers and the connect parser, run natively.

The Worker pulls a field out of a host-list object with four small readers (fci_json_str,
fci_json_str_val, fci_json_bool, fci_json_u64, in src/fci_json.c) and parses one connect with
fci_connect_parse (src/fci_connect.c). This compiles that shipping code with the host compiler
against the driver (tests/json_fields_test.c) and runs it: each reader with the field present,
absent, the wrong type, spaces around the colon, and a key that is a prefix of another key
("seq" vs "seqx"); fci_json_u64 with 0, a 15-digit number and a non-number; and
fci_connect_parse with a full direct entry, a relay entry, an entry with local, a missing field,
a 31-char nonce, a bad mac, and a seq-less entry.

    pytest tests/test_json_fields_c.py
"""

from pathlib import Path

import pytest

import cc_helper

REPO = cc_helper.REPO
CC = cc_helper.find_cc()

CHECKS = [
    "str-present",
    "str-absent",
    "str-wrong-type",
    "str-spaces",
    "str-key-prefix",
    "strval-present",
    "strval-absent",
    "strval-wrong-type",
    "strval-spaces",
    "strval-key-prefix",
    "strval-empty",
    "bool-true",
    "bool-false",
    "bool-absent",
    "bool-wrong-type",
    "bool-spaces-after",
    "u64-present",
    "u64-zero",
    "u64-15-digits",
    "u64-absent",
    "u64-non-number",
    "u64-negative",
    "u64-spaces-after",
    "connect-direct",
    "connect-relay",
    "connect-local",
    "connect-missing-mac",
    "connect-nonce-31",
    "connect-bad-mac",
    "connect-no-seq",
]


@pytest.mark.skipif(not CC, reason="no C compiler (cc, gcc or the bundled clang) on this machine")
def test_json_fields_c(tmp_path: Path) -> None:
    exe = tmp_path / "json_fields_test.exe"
    build, run = cc_helper.build_run(
        [REPO / "src" / "fci_json.c", REPO / "src" / "fci_connect.c", REPO / "tests" / "json_fields_test.c"],
        exe,
        cc=CC,
    )
    assert build.returncode == 0, "the JSON fields check failed to compile:\n" + build.stderr

    assert run.returncode == 0, "the JSON fields check failed:\n" + run.stdout + run.stderr
    for name in CHECKS:
        line = next((ln for ln in run.stdout.splitlines() if ln.endswith(f" {name}:")), None)
        assert line is not None, f"no result line for {name}:\n" + run.stdout
        assert line.startswith("ok"), line
    assert f"{len(CHECKS)} checks, 0 failed" in run.stdout
