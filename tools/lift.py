"""Recompile the game's or the map editor's x86 code to C, with the pinned xboxrecomp toolkit.

  python -B tools/lift.py <default.xbe | ige.xbe> --xbe <file> --toolkit <xboxrecomp> --out <dir>

  --xbe       the executable from the disc; checked against the release this project supports
  --toolkit   an xboxrecomp checkout at the pinned commit (tools/deps.py fetches it)
  --out       created; receives the lifter's copy of the toolkit, its analysis and the generated C

Steps: copy the toolkit and apply this project's lifter patches (patches/lifter); parse the XBE;
disassemble from the image's seed list (<title data>/<image>.seeds.json, tools/titles.py: function starts
the automatic analysis misses); identify functions and their calling conventions; translate every function
to C in <out>/gen.
For the map editor, the entries measured at run time (data/ige.profile.json "supplemental_entries") are
translated into <out>/supp. The result is the same, byte for byte, on any machine.

Needs Python 3.11+ and capstone (requirements.txt). About 8 minutes for the game.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
LIFTER_HELPER = REPO / "patches" / "lifter" / "fci_direct.py"
sys.path.insert(0, str(REPO / "tools"))
import events  # noqa: E402
import titles  # noqa: E402


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare_toolkit(toolkit, dst):
    """the toolkit with the lifter patches, in dst"""
    sys.path.insert(0, str(REPO / "tools"))
    import patches

    shutil.copytree(toolkit, dst, ignore=shutil.ignore_patterns(".git", "__pycache__"))
    applied = patches.apply_series("lifter", dst)
    shutil.copy2(LIFTER_HELPER, dst / "tools" / "recomp" / "fci_direct.py")
    return applied


def run_steps(toolkit, xbe, seeds, out, log, tag=None):
    analysis = out / "analysis.json"
    d = out / "disasm"
    steps = [
        ["tools.xbe_parser", str(xbe), "--json", str(analysis), "--quiet"],
        [
            "tools.disasm",
            str(xbe),
            "--analysis-json",
            str(analysis),
            "--force",
            "-o",
            str(d),
            "--seed-functions",
            str(seeds),
        ],
        [
            "tools.func_id",
            str(xbe),
            "--functions",
            str(d / "functions.json"),
            "--strings",
            str(d / "strings.json"),
            "--xrefs",
            str(d / "xrefs.json"),
            "-o",
            str(out / "func_id"),
        ],
        [
            "tools.abi_analysis",
            str(xbe),
            "--disasm-dir",
            str(d),
            "--func-id-dir",
            str(out / "func_id"),
            "--output-dir",
            str(out / "abi"),
        ],
        [
            "tools.recomp",
            str(xbe),
            "--all",
            "--split",
            "250",
            "--disasm-dir",
            str(d),
            "--func-id-dir",
            str(out / "func_id"),
            "--abi-dir",
            str(out / "abi"),
            "--gen-dir",
            str(out / "gen"),
        ],
    ]
    for number, step in enumerate(steps):
        events.progress("lift", number, len(steps), f"Recompiling to C: {step[0][6:]}", part=Path(xbe).name)
        started = time.time()
        if not tag:
            print(f"  {step[0][6:]} ...", end="", flush=True)
        done = subprocess.run([sys.executable, "-B", "-m", *step], cwd=toolkit, capture_output=True, text=True)
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"== {' '.join(step)}\n{done.stdout}{done.stderr}\n")
        if tag:  # lifts running side by side: whole lines, named
            print(f"  {tag}: {step[0][6:]} {time.time() - started:.0f} s", flush=True)
        else:
            print(f" {time.time() - started:.0f} s", flush=True)
        if done.returncode:
            raise SystemExit(f"error: {step[0]} failed (exit {done.returncode}); see {log}")
    events.progress("lift", len(steps), len(steps), part=Path(xbe).name)


def supplement(toolkit, xbe, profile, out):
    """the map editor's measured entries, translated into out/supp"""
    entries = profile.get("supplemental_entries") or []
    if not entries:
        return 0
    sys.path.insert(0, str(toolkit))
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator

    config.configure_from_xbe(str(xbe))
    raw = Path(xbe).read_bytes()
    functions = json.loads((out / "disasm" / "functions.json").read_text())
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16), _addr=int(f["start"], 16)) for f in functions}
    for item in entries:
        va = int(item["start"], 16)
        db[va] = dict(
            item, end=int(item["end"], 16), _addr=va, name=f"sub_{va:08X}", detection_method="measured_indirect_entry"
        )
    abi = {int(v["address"], 16): v for v in json.loads((out / "abi" / "abi_functions.json").read_text())}
    t = profile["supplement_translator"]
    translator = FunctionTranslator(
        raw,
        db,
        abi_db=abi,
        seh_prolog=tuple(int(v, 16) for v in t["seh_prolog"]),
        seh_epilog=int(t["seh_epilog"], 16),
        setjmp_fn=int(t["setjmp"], 16),
        longjmp_fn=int(t["longjmp"], 16),
    )
    bodies = []
    for item in sorted(entries, key=lambda v: int(v["start"], 16)):
        va = int(item["start"], 16)
        generated = translator.translate_function(va, db[va])
        if not generated:
            raise SystemExit(f"error: supplemental entry {va:08X} did not translate")
        bodies.append(generated)
    supp = out / "supp"
    supp.mkdir()
    (supp / "recomp_0000.c").write_text(
        '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include <math.h>\n' + "\n".join(bodies),
        encoding="utf-8",
        newline="\n",
    )
    header = (out / "gen" / "recomp_funcs.h").read_text()
    header += "\n" + "\n".join(f"void sub_{int(v['start'], 16):08X}(void);" for v in entries) + "\n"
    (supp / "recomp_funcs.h").write_text(header, encoding="utf-8", newline="\n")
    shutil.copy2(out / "gen" / "recomp_types.h", supp / "recomp_types.h")
    return len(entries)


def inputs_digest(image, title):
    """what a lift of title's image takes from this repository: the toolkit pin, the lifter patches, the seeds
    and the image profile (line endings normalised)"""
    sys.path.insert(0, str(REPO / "tools"))
    import patches

    stem = Path(image).stem
    h = hashlib.sha256()
    for p in (
        REPO / "tools" / "deps.py",
        Path(__file__),
        REPO / "tools" / "patches.py",
        *patches.series("lifter"),
        LIFTER_HELPER,
        title.data / f"{stem}.seeds.json",
        title.data / f"{stem}.profile.json",
    ):
        if p.exists():
            h.update(p.name.encode() + b"\0" + p.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def current(image, title, out):
    """out holds a finished lift of title's image from the repository's present inputs"""
    try:
        record = json.loads((Path(out) / "lift.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return record.get("inputs_sha256") == inputs_digest(image, title)


def lift(image, xbe, toolkit, out, tag=None):
    """tag: the lift runs beside another one; its progress lines carry this name"""
    if image not in titles.IMAGES:
        raise SystemExit(f"error: unknown image {image} (default.xbe or ige.xbe)")
    title = titles.of_image(image, xbe)
    if not title:
        raise SystemExit(f"error: {xbe} is not the supported release of {image} of any title")
    if not (Path(toolkit) / "tools" / "recomp").is_dir():
        raise SystemExit(f"error: {toolkit} is not an xboxrecomp checkout")
    out = Path(out)
    if out.exists():
        raise SystemExit(f"error: {out} exists; lift into a new folder")
    out.mkdir(parents=True)
    started = time.time()
    lifter = out / "toolkit"
    fixes = prepare_toolkit(Path(toolkit), lifter)
    print(f"lifting {title.name} {image}: toolkit prepared ({len(fixes)} lifter patches)")
    run_steps(lifter, Path(xbe).resolve(), title.data / f"{Path(image).stem}.seeds.json", out, out / "lift.log", tag)
    profile_path = title.data / f"{Path(image).stem}.profile.json"
    extra = supplement(lifter, xbe, json.loads(profile_path.read_text()), out) if profile_path.exists() else 0
    record = {
        "image": image,
        "title": title.key,
        "xbe_sha256": title.images[image],
        "inputs_sha256": inputs_digest(image, title),
        "lifter_patches": fixes,
        "generated": {p.name: sha256(p) for p in sorted((out / "gen").iterdir())},
        "supplemental_entries": extra,
        "seconds": round(time.time() - started),
    }
    if extra:
        record["supplement"] = {p.name: sha256(p) for p in sorted((out / "supp").iterdir())}
    (out / "lift.json").write_text(json.dumps(record, indent=1) + "\n", encoding="utf-8")
    print(
        f"done in {record['seconds']} s: {len(record['generated'])} files in {out / 'gen'}"
        + (f", {extra} supplemental entries in {out / 'supp'}" if extra else "")
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", choices=titles.IMAGES)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--toolkit", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    lift(a.image, a.xbe, a.toolkit, a.out)


if __name__ == "__main__":
    main()
