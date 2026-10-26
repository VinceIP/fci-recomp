"""Build the game or the map editor from a lift: the generated C, the runtime and this project's host code.

  python -B tools/build.py <default.xbe | ige.xbe> --xbe <file> --lift <dir> --deps <dir> --out <dir>
                           [--jobs N] [--toolchain <dir>] [--obj-cache <dir>]

  --xbe    the executable from the disc (checked; the map editor's profile is built from it)
  --lift   tools/lift.py's output for this executable
  --deps   tools/deps.py's folder (xboxrecomp, xbe-recomp)
  --out    created; the build happens in <out>/case, the result goes to <out>/bin
  --jobs   compilers at once (default: by the processor's cores and the memory, about 2 GB each)
  --toolchain  tools/msvc.py's folder, used when Visual Studio is not installed
  --obj-cache  a folder of compiled generated files kept between builds; a file whose rewritten source,
               headers, compiler and flags are unchanged is copied instead of compiled (runtime work)

Steps: find Visual Studio and the Windows SDK (tools/toolchain.py); stage the build (the runtime from
xboxrecomp with patches/runtime, src/, the generated C); match the XDK library functions the D3D and
DirectSound layer replaces (tools/sigmatch.py); compile every group with tools/compile.py, the generated
C one file per compiler, as many at once as --jobs; link fci-whole.exe and fci-kinit.dll.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))
import events  # noqa: E402
import image_profile  # noqa: E402
import patches  # noqa: E402
import preflight  # noqa: E402
import titles  # noqa: E402
import toolchain  # noqa: E402

DATA = REPO / "data"
HOST = REPO / "src"
BUILD_TOOLS = ("compile.py", "image_profile.py")


def default_jobs():
    """one compiler per core, as far as the memory goes: an /O1 compile of a generated file takes up to
    about 2 GB, and the rest of the machine keeps 3 GB"""
    return max(1, min(os.cpu_count() or 1, int((preflight.memory_gb() - 3) / 2)))


def stage(image, title, xbe, lift_dir, deps, case, tc):
    inputs = case / "inputs"
    inputs.mkdir(parents=True)
    (case / "work").mkdir()
    (inputs / "toolchain.json").write_text(json.dumps(tc, indent=2), encoding="utf-8")
    for part in ("src", "include"):
        shutil.copytree(deps / "xboxrecomp" / part, inputs / "toolkit" / part)
    applied = patches.apply_series("runtime", inputs / "toolkit")
    shutil.copytree(HOST, inputs / "host", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(lift_dir / "gen", inputs / "gen")
    if (lift_dir / "supp").exists():
        shutil.copytree(lift_dir / "supp", inputs / "supp")
    for tool in BUILD_TOOLS:
        shutil.copy2(TOOLS / tool, inputs / tool)
    profile = title.data / f"{Path(image).stem}.profile.json"
    if profile.exists():
        image_profile.stage_profile(profile, xbe, inputs, title)
    (inputs / "xbr-symbols").mkdir()
    signatures = deps / "xbe-recomp" / "xdk" / "5849" / "signatures.toml"
    done = subprocess.run(
        [
            sys.executable,
            "-B",
            str(TOOLS / "sigmatch.py"),
            str(xbe),
            str(signatures),
            str(case / "symbols.json"),
            str(inputs / "xbr-symbols" / "image_symbols.h"),
            str(title.data / f"{Path(image).stem}.pins.json"),
        ],
        capture_output=True,
        text=True,
    )
    if done.returncode:
        raise SystemExit(f"error: matching the XDK libraries failed:\n{done.stdout[-2000:]}{done.stderr[-2000:]}")
    return len(applied)


def compile_all(case, jobs, log, obj_cache=None):
    inputs, work = case / "inputs", case / "work"
    build = [sys.executable, "-B", str(inputs / "compile.py")]
    env = dict(os.environ, **({"FCI_OBJ_CACHE": str(Path(obj_cache).resolve())} if obj_cache else {}))

    def run(args, compilers):
        done = subprocess.run(
            build + list(args), capture_output=True, text=True, env=env | {"FCI_JOBS": str(compilers)}
        )
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"== {' '.join(args)}\n{done.stdout}{done.stderr}\n")
        if done.returncode:
            raise SystemExit(
                f"error: {' '.join(args)} failed; see {log}\n"
                "Run the build again; if it stops the same way, report it on Discord or GitHub with that log."
            )

    def step(*args):
        started = time.time()
        print(f"  {' '.join(args)} ...", end="", flush=True)
        run(args, jobs)
        print(f" {time.time() - started:.0f} s", flush=True)

    for group in ("runtime", "host", "xbr", "stubs", "dispatch"):
        step("cc", group)
        if group == "xbr":  # the functions it replaces get their switch in the generated files
            shutil.copy2(work / "xbr-replace.json", inputs / "xbr-replace.json")
    # the generated C, one file per compiler from a pool, largest first: a 5 to 11 MB file started late
    # would be the whole build's tail
    sources = sorted(inputs.glob("gen/recomp_[0-9][0-9][0-9][0-9].c"), key=lambda p: -p.stat().st_size)
    files = len(sources)
    started, finished, shown = time.time(), 0, -1
    console = sys.stdout.isatty()  # one line updated in place; in a log, a line every 10%
    with ThreadPoolExecutor(jobs) as pool:
        pending = [pool.submit(run, ("cc", f"gen:{int(p.stem[7:])}:{int(p.stem[7:])}"), 1) for p in sources]
        for future in as_completed(pending):
            future.result()
            finished += 1
            percent = finished * 100 // files if console else finished * 10 // files * 10
            events.progress("build", finished, files, f"Compiling ({finished} of {files} files)", part=case.parent.name)
            if percent != shown:
                print(
                    f"{chr(13) if console else ''}  the game's code: {percent}% ({finished} of {files} files)",
                    end="" if console else "\n",
                    flush=True,
                )
                shown = percent
    lead = ", " if console else "  the game's code: done in "
    print(f"{lead}{time.time() - started:.0f} s", flush=True)
    if (inputs / "supp").exists():
        step("cc", "supp")
        step("cc", "supp:0")
    (inputs / "obj").mkdir()
    for obj in (work / "obj").glob("*.obj"):
        if obj.name != "fci_kinit.obj":  # fci-kinit.dll's, not the executable's
            shutil.move(str(obj), str(inputs / "obj" / obj.name))
    step("link")
    step("kinit")


def build(image, xbe, lift_dir, deps, out, jobs=None, tc=None, obj_cache=None):
    title = titles.of_image(image, xbe)
    if not title:
        raise SystemExit(f"error: {xbe} is not the supported release of {image} of any title")
    record = json.loads((lift_dir / "lift.json").read_text())
    if record["image"] != image or record.get("xbe_sha256") != title.images[image]:
        raise SystemExit(f"error: {lift_dir} is a lift of another executable ({record['image']})")
    out = Path(out)
    if out.exists():
        raise SystemExit(f"error: {out} exists; build into a new folder")
    jobs = jobs or default_jobs()
    tc = tc or toolchain.discover()
    started = time.time()
    case = out / "case"
    patches = stage(image, title, Path(xbe).resolve(), lift_dir, deps, case, tc)
    print(f"building {title.name} {image}: staged ({patches} runtime patches), compiling with {jobs} jobs")
    compile_all(case, jobs, out / "build.log", obj_cache)
    (out / "bin").mkdir()
    for name in ("fci-whole.exe", "fci-whole.pdb", "fci-kinit.dll"):
        shutil.copy2(case / "work" / name, out / "bin" / name)
    (out / "build.json").write_text(
        json.dumps(
            {
                "image": image,
                "title": title.key,
                "inputs_sha256": inputs_digest(lift_dir, tc),
                "msvc": tc["msvc"],
                "windows_sdk": tc["windows_sdk"],
                "jobs": jobs,
                "seconds": round(time.time() - started),
            },
            indent=1,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"done in {(time.time() - started) / 60:.0f} min: {out / 'bin' / 'fci-whole.exe'}")


def inputs_digest(lift_dir, tc):
    """what a build takes: the lift (its record lists every generated file's digest), the compiler, the
    host code, the runtime patches and the build tools (line endings normalised)"""
    h = hashlib.sha256()
    h.update((Path(lift_dir) / "lift.json").read_bytes())
    h.update(json.dumps(tc, sort_keys=True).encode())
    files = sorted(p for p in HOST.rglob("*") if p.is_file() and "__pycache__" not in p.parts)
    files += patches.series("runtime") + sorted(DATA.rglob("*.json"))
    files += [TOOLS / n for n in ("build.py", "patches.py", "sigmatch.py", "titles.py", *BUILD_TOOLS)]
    for p in files:
        h.update(p.relative_to(REPO).as_posix().encode() + b"\0" + p.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def current(out, lift_dir, tc):
    """out holds a finished build of this lift from the repository's present code and this compiler"""
    try:
        record = json.loads((Path(out) / "build.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return (Path(out) / "bin" / "fci-whole.exe").exists() and record.get("inputs_sha256") == inputs_digest(lift_dir, tc)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", choices=titles.IMAGES)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--lift", required=True)
    ap.add_argument("--deps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, help="parallel compilers, about 2 GB of memory each")
    ap.add_argument("--toolchain", help="tools/msvc.py's folder, when Visual Studio is not installed")
    ap.add_argument("--obj-cache", help="compiled generated files kept between builds")
    a = ap.parse_args()
    try:
        tc = toolchain.discover(a.toolchain)
    except toolchain.ToolchainError as e:
        raise SystemExit(f"error: {e}")
    build(a.image, a.xbe, Path(a.lift), Path(a.deps), a.out, a.jobs, tc, a.obj_cache)


if __name__ == "__main__":
    main()
