"""Q22c exit wait: the save log's quiet/busy decision, from the toolkit diff.

The C lives in the toolkit (patches/runtime/0026-save-log.patch creates savelog.c,
patches/runtime/0027-exit-wait.patch adds savelog_busy_decision), so the test
extracts savelog.c from both diffs, compiles it with the host compiler and runs
a small harness over the pure decision. Skips when a diff or a host C compiler
is absent; fails when the decision behaves differently from the exit wait's
promise (quiet: no UDATA file open for writing and no line within the window).
"""

from pathlib import Path
import shutil
import subprocess

import pytest

import patches

REPO = Path(__file__).resolve().parents[1]
DIFF = REPO / "patches" / "runtime" / "0026-save-log.patch"
DIFF_BUSY = REPO / "patches" / "runtime" / "0027-exit-wait.patch"

HARNESS = r"""
#include <stdio.h>
#include "savelog.h"

static void show_n(const char* tag, uint64_t v)
{
    printf("%s %llu\n", tag, (unsigned long long)v);
}

int main(void)
{
    /* now, last_event, open_writes, quiet_ms */
    show_n("B1", savelog_busy_decision(1000, 500, 0, 250));   /* 500 ms since the line: quiet */
    show_n("B2", savelog_busy_decision(1000, 800, 0, 250));   /* 200 ms since the line: busy */
    show_n("B3", savelog_busy_decision(1000, 750, 0, 250));   /* exactly the window: quiet */
    show_n("B4", savelog_busy_decision(1000, 751, 0, 250));   /* one ms inside: busy */
    show_n("B5", savelog_busy_decision(1000, 0, 1, 250));     /* a write file open, no line yet */
    show_n("B6", savelog_busy_decision(1000, 990, 2, 250));   /* write files open, line just now */
    show_n("B7", savelog_busy_decision(100, 200, 0, 250));    /* tick went backwards: no wraparound busy */
    show_n("B8", savelog_busy_decision(100, 200, 1, 250));    /* tick went backwards, a write open */
    show_n("B9", savelog_busy_decision(10000, 0, 0, 250));    /* no line ever: quiet once past the window */
    return 0;
}
"""


@pytest.fixture(scope="module")
def busy_output(tmp_path_factory):
    if not DIFF.exists() or not DIFF_BUSY.exists():
        pytest.skip(f"no save log diffs at {DIFF} / {DIFF_BUSY}")
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no host C compiler")
    src = tmp_path_factory.mktemp("savelog_busy")
    files = {}
    for path, hunks in patches.parse(DIFF.read_bytes()):
        if path in ("src/kernel/savelog.c", "src/kernel/savelog.h"):
            files[path] = patches.apply_hunks(b"", hunks, path)
    if "src/kernel/savelog.c" not in files:
        pytest.fail("savelog.c not in the save log diff")
    for path, hunks in patches.parse(DIFF_BUSY.read_bytes()):
        if path in files:
            files[path] = patches.apply_hunks(files[path], hunks, path)
    if b"savelog_busy_decision" not in files["src/kernel/savelog.c"]:
        pytest.fail("savelog_busy_decision not in the exit wait diff")
    (src / "savelog.c").write_bytes(files["src/kernel/savelog.c"])
    (src / "savelog.h").write_bytes(files["src/kernel/savelog.h"])
    (src / "harness.c").write_text(HARNESS)
    exe = src / "harness"
    subprocess.run([cc, "-o", str(exe), str(src / "harness.c"), str(src / "savelog.c"), "-I", str(src)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout


def _n(out, tag):
    for line in out.splitlines():
        if line.startswith(tag + " "):
            return int(line[len(tag) + 1 :])
    raise AssertionError(f"no {tag} line in the harness output")


def test_quiet_when_the_line_is_old(busy_output):
    assert _n(busy_output, "B1") == 0


def test_busy_when_the_line_is_within_the_window(busy_output):
    # the value is the ms since the last line: the caller can bound the wait from it
    assert _n(busy_output, "B2") == 200


def test_the_window_boundary_is_quiet(busy_output):
    # exactly quiet_ms since the line is past the window
    assert _n(busy_output, "B3") == 0


def test_one_ms_inside_the_window_is_busy(busy_output):
    assert _n(busy_output, "B4") == 249


def test_busy_while_a_write_file_is_open(busy_output):
    # the mid-save case: the file is open for writing and no line has come yet
    assert _n(busy_output, "B5") == 1000


def test_busy_counts_the_open_writes(busy_output):
    assert _n(busy_output, "B6") == 10


def test_a_tick_anomaly_does_not_wrap_into_busy(busy_output):
    # now < last_event: the subtraction wraps to a large since, which is quiet
    assert _n(busy_output, "B7") == 0


def test_a_tick_anomaly_stays_busy_with_an_open_write(busy_output):
    assert _n(busy_output, "B8") > 0


def test_no_line_ever_is_quiet_past_the_window(busy_output):
    # last_event 0 (no line since boot) and now well past the window
    assert _n(busy_output, "B9") == 0
