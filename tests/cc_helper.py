"""Shared compile-and-run helper for the tests that exercise the shipping C natively.

Every C test (test_stun_c, test_json_c, test_frame_sanity, test_net_fuzz, test_savelog,
test_browse_status_c, ...) finds the compiler the same way: cc, then gcc, then the bundled
clang of a Windows checkout (toolchain/*/bin/clang.exe), so a test still runs on a machine
with no system C compiler. build_run compiles the sources with the standard flags and runs
the driver, returning both subprocess results.
"""

from pathlib import Path
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[1]

# the flags every C test driver is built with
FLAGS = ["-std=gnu11", "-O2", "-Wall", "-Werror"]


def find_cc():
    """The host compiler, or the bundled clang of a Windows checkout (so the test runs there too)."""
    bundled = sorted(REPO.glob("toolchain/*/bin/clang.exe"))
    return shutil.which("cc") or shutil.which("gcc") or (str(bundled[0]) if bundled else None)


def build_run(sources, exe, cc=None, includes=None, flags=None, timeout=30):
    """Compile sources into exe and run it. Returns (build, run): two subprocess results.

    sources: the .c files to compile (Path or str).
    includes: -I directories (default: src/); pass the temp dir for a test that compiles a
              file extracted from a patch (test_savelog).
    flags: the compiler flags (default: FLAGS); pass a quieter set to change behaviour.
    """
    cc = cc or find_cc()
    inc = [str(REPO / "src")] if includes is None else [str(i) for i in includes]
    fl = FLAGS if flags is None else flags
    cmd = [cc, *fl]
    for d in inc:
        cmd += ["-I", d]
    cmd += [str(s) for s in sources]
    cmd += ["-o", str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    return build, run
