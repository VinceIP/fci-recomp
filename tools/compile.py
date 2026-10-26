"""Compile one group of a staged build (tools/build.py stages it and runs this once per group).

  python -B <case>/inputs/compile.py cc <group>   runtime | host | xbr | stubs | dispatch | supp | supp:<n>
                                                  | gen:<first>:<last>
  python -B <case>/inputs/compile.py kinit        build fci-kinit.dll
  python -B <case>/inputs/compile.py link         link <case>/inputs/obj/*.obj into work/fci-whole.exe

Objects go to <case>/work/obj, and each step writes a JSON record of what it compiled to <case>/work.
Generated files are rewritten before they compile (replace_bodies): hooks, stops for functions that did
not translate, port I/O and privileged instructions as runtime calls, and yield points at loop edges.
"""

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

RUNTIME_SOURCES = {
    "kernel": [
        "kernel_path.c",
        "kernel_pool.c",
        "kernel_rtl.c",
        "kernel_memory.c",
        "kernel_file.c",
        "kernel_thread.c",
        "kernel_sync.c",
        "kernel_hal.c",
        "kernel_xbox.c",
        "kernel_ob.c",
        "kernel_io.c",
        "kernel_crypto.c",
        "kernel_thunks.c",
        "kernel_bridge.c",
        "xbox_memory_layout.c",
        "guest_vmem.c",
        "icall_feedback.c",
        "recomp_trace.c",
        "nv2a_pb_scan.c",
        "nv2a_pb_exec.c",
        "nv2a_vsh_interp.c",
        "nv2a_combiner.c",
    ],
    "d3d": [
        "d3d8_device.c",
        "d3d8_gamma.c",
        "d3d8_resources.c",
        "d3d8_shaders.c",
        "d3d8_states.c",
        "d3d8_combiners.c",
        "d3d8_vsh.c",
    ],
    # only the decoder: the register model and pusher are src/fci_nv2a.c
    "nv2a": ["nv2a_mmio_hook.c"],
    "apu": ["apu_core.c", "apu_vp.c", "apu_dsp.c", "apu_mmio_hook.c", "apu_xaudio2.c"],
    "audio": ["dsound_device.c", "wma_decoder.c"],
    "input": ["xinput_device.c"],
    "usb": ["ohci.c", "usb_gamepad.c"],
    "video": ["video_player.c", "video_pump.c", "fb_present.c"],
}
SYSTEM_LIBS = [
    "kernel32.lib",
    "user32.lib",
    "gdi32.lib",
    "advapi32.lib",
    "ole32.lib",
    "shell32.lib",
    "winmm.lib",
    "dbghelp.lib",
    "bcrypt.lib",
    "d3d11.lib",
    "dxgi.lib",
    "dxguid.lib",
    "d3dcompiler.lib",
    "xinput.lib",
    "mfplat.lib",
    "mfreadwrite.lib",
    "mfuuid.lib",
    "mf.lib",
    "propsys.lib",
    "uuid.lib",
    "d3d12.lib",
]
GEN_OPT = "/O1"  # the generated C, optimised for size

# Generated bodies replaced at build time, each with the reason (see also re/notes):
#  quarantine: data the detector took for code, not valid C; the body becomes a visible stop (exit 85)
#  override: instructions the lifter cannot express (privileged CPU state); src/fci_cpu.c implements them
#  prehook: fci_prehook_<va>() runs, then the unchanged lifted body
#  gate: fci_gate_<va>() runs first; nonzero means it returned for the function and the body is skipped
# The map editor's and other games' replacements come from their image profiles (data/ige.profile.json,
# data/<title>/*.profile.json). There a third element names the runtime's hook by the address it was written
# for, when it is the same function in another executable: ["prehook", "why", "001F6C00"] calls
# fci_prehook_001F6C00.
REPLACE = {
    "sub_000674C6": (
        "quarantine",
        "data decoded as code (edi=edi; repe scasd; ebp-0x7A0000ED); lifter emitted undeclared _cf",
    ),
    "sub_004B03B4": (
        "override",
        "sgdt/cli/xchg GDT/sti/ljmp 8 descriptor helper; lifter emits RECOMP_UNIMPL for 4 of 11 instructions",
    ),
    "sub_00580363": (
        "prehook",
        "DirectSound 5849 effects-image download: writes command 3 to the "
        "DSP doorbell at image + 0x810 (image = [[[ecx+8]+0x10]]) and spins "
        "until it reads 0; the hook registers that address with the APU "
        "acknowledgement (fci_manual.c)",
    ),
    "sub_001F6C00": ("prehook", "Input_PollPads (re/symbols): counts polls for RECOMP_FCI_STATUS"),
    "sub_000F0500": (
        "prehook",
        "Camera_BuildViews (re/symbols): counts 3D frames for RECOMP_FCI_STATUS, a line per frame for "
        "RECOMP_FCI_TRACE (fci_trace.c)",
    ),
    "sub_00098AE0": (
        "prehook",
        "Player_ProcessInput (re/symbols): adds direct mouse aim, in degrees, to the "
        "frame's turn (fci_aim.c, re/notes/INPUT-CAMERA.md)",
    ),
    "sub_001FD9E0": (
        "prehook",
        "Console_CreateVariable (re/symbols): replaces the default string with RECOMP_CVAR_<name> (fci_cvars.c)",
    ),
    "sub_001FDCD0": (
        "prehook",
        "Console_RegisterFloat (re/symbols): records the variable and replaces its "
        "default with RECOMP_CVAR_<name> (fci_cvars.c)",
    ),
    "sub_001FDED0": ("prehook", "Console_RegisterInt (re/symbols): as Console_RegisterFloat"),
    "sub_0021FFA0": (
        "gate",
        "a menu background video's update (eax = video, ret 4): skipped until its next frame is due by the "
        "Bink header's rate, as the game does only under 25 fps (fci_bink.c, re/notes/MOVIES.md)",
    ),
    "sub_003741E0": (
        "gate",
        "the physics world's step (world, frame time; ret 8), Far Cry's CPhysicalWorld::TimeStep: stepped at "
        "30 a second whatever the frame rate (fci_physics.c, re/notes/FRAME-RATE.md)",
    ),
    "sub_00220200": (
        "gate",
        "the title's movie player (path, ...), cdecl; RECOMP_FCI_SKIP_INTROS / "
        "RECOMP_FCI_SKIP_MOVIES skip listed movies as if they had played (fci_disk.c, "
        "re/notes/MOVIES.md)",
    ),
}

STUB = re.compile(rb"void (sub_([0-9A-F]{8}))\(void\) \{ g_esp \+= 4; /\* 0x[0-9A-F]{8}: not detected \*/ \}")
FAILED = re.compile(rb"void (sub_([0-9A-F]{8}))\(void\) \{ /\* translation failed \*/ \}")
# Port I/O becomes a call into fci_port.c, which stops on unregistered ports. The string forms (ins/outs)
# only occur in data decoded as code and stay stops.
PORT_OUT = re.compile(rb'RECOMP_UNIMPL\("out (dx|0x[0-9a-f]+), (al|ax|eax)", (0x[0-9A-F]{8}u)\);')
PORT_IN = re.compile(rb'RECOMP_UNIMPL\("in (al|ax|eax), (dx|0x[0-9a-f]+)", (0x[0-9A-F]{8}u)\);')
WIDTH = {b"al": (b"LO8(eax)", b"1"), b"ax": (b"LO16(eax)", b"2"), b"eax": (b"eax", b"4")}
# EFLAGS, CPUID and the interrupt flag (fci_cpu.c); WBINVD is a full memory barrier on the host (fci_port.c)
PUSHFD = re.compile(rb'RECOMP_UNIMPL\("pushfd", (0x[0-9A-F]{8}u)\);')
POPFD = re.compile(rb'RECOMP_UNIMPL\("popfd", (0x[0-9A-F]{8}u)\);')
CPUID = re.compile(rb'RECOMP_UNIMPL\("cpuid", (0x[0-9A-F]{8}u)\);')
CLI = re.compile(rb'RECOMP_UNIMPL\("cli", (0x[0-9A-F]{8}u)\);')
STI = re.compile(rb'RECOMP_UNIMPL\("sti", (0x[0-9A-F]{8}u)\);')
WBINVD = re.compile(rb'RECOMP_UNIMPL\("wbinvd", (0x[0-9A-F]{8}u)\);')
# A loop that waits on another guest thread without a kernel call would never let it run: every backward
# goto checks the yield request raised by threads waiting for the guest CPU
LABEL = re.compile(rb"^loc_([0-9A-F]{8}): ;", re.M)
GOTO = re.compile(rb"goto (loc_([0-9A-F]{8}));")
# A direct branch whose label the translator could not place: reaching it stops instead of falling through
MISSING_GOTO = re.compile(rb"\(void\)0; /\* goto loc_([0-9A-Fa-f]{8}) - dead code, label not in function \*/")
ERROR_LINE = re.compile(r"\((\d+)\): (?:fatal )?error (C\d+): (.*)")
DEFINITION = re.compile(rb"^void (sub_[0-9A-F]{8})\(void\)(?:\r?\n\{| \{ /\* translation failed)", re.M)
REFERENCE = re.compile(rb"\bsub_[0-9A-F]{8}\b")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_manifest_res(manifest, out):
    """a compiled resource file (.res) holding the manifest as the executable's RT_MANIFEST 1. The linker takes
    it through cvtres, which comes with the compiler; /MANIFESTINPUT would need the SDK's mt.exe, which the
    downloaded compiler lacks"""

    def entry(rtype, name, data, flags):
        header = struct.pack("<IIHHHHIHHII", len(data), 32, 0xFFFF, rtype, 0xFFFF, name, 0, flags, 0, 0, 0)
        return header + data + b"\0" * (-len(data) % 4)

    data = Path(manifest).read_bytes()
    Path(out).write_bytes(entry(0, 0, b"", 0) + entry(24, 1, data, 0x1030))  # the empty first entry every .res has


def port_operand(text):
    return b"LO16(edx)" if text == b"dx" else text + b"u"


def rewrite_ports(text):
    def out(m):
        value, size = WIDTH[m.group(2)]
        return b"fci_port_out(" + port_operand(m.group(1)) + b", " + value + b", " + size + b", " + m.group(3) + b");"

    def inp(m):
        size = WIDTH[m.group(1)][1]
        call = b"fci_port_in(" + port_operand(m.group(2)) + b", " + size + b", " + m.group(3) + b")"
        if m.group(1) == b"eax":
            return b"eax = " + call + b";"
        return (b"SET_LO8(eax, " if m.group(1) == b"al" else b"SET_LO16(eax, ") + call + b");"

    count = sum(len(p.findall(text)) for p in (PORT_OUT, PORT_IN, WBINVD, PUSHFD, POPFD, CPUID, CLI, STI))
    text = WBINVD.sub(rb"fci_wbinvd(\1);", PORT_IN.sub(inp, PORT_OUT.sub(out, text)))
    text = CLI.sub(rb"fci_cli(\1);", STI.sub(rb"fci_sti(\1);", text))
    text = PUSHFD.sub(rb"PUSH32(esp, fci_pushfd(\1));", text)
    text = POPFD.sub(rb"{ uint32_t _fci_fl; POP32(esp, _fci_fl); fci_popfd(_fci_fl, \1); }", text)
    text = CPUID.sub(rb"fci_cpuid(\1);", text)
    return text, count


def add_yield_points(text):
    out, last, count = [], 0, 0
    for line in text.split(b"\n"):
        m = LABEL.match(line)
        if m:
            last = int(m.group(1), 16)
        elif b"goto loc_" in line:

            def wrap(g):
                nonlocal count
                if int(g.group(2), 16) <= last:  # noqa: B023 (used within this iteration)
                    count += 1
                    return b"{ if (g_fci_yield) guest_cpu_yield(); goto " + g.group(1) + b"; }"
                return g.group(0)

            line = GOTO.sub(wrap, line)
        if line.startswith(b"void sub_") or line.startswith(b"static void sub_"):
            last = 0
        out.append(line)
    return b"\n".join(out), count


def xbr_listing():
    """what the host D3D8 and DirectSound layers implement (written by the `xbr` group)"""
    path = Path(__file__).resolve().parent / "xbr-replace.json"
    return json.loads(path.read_text()) if path.exists() else {}


def xbr_functions():
    return xbr_listing().get("replace", [])


def xbr_audio_functions():
    return set(xbr_listing().get("audio", []))


def replace_bodies(source, work, record, prefix=""):
    """the generated file with its rewrites, in work/gen-r (or source itself when nothing applies)"""
    original = source.read_bytes()
    text = original.replace(b"\r\n", b"\n")  # the rewrites match LF text
    hits = [name for name in REPLACE if b"\nvoid " + name.encode() + b"(void)\n{" in text]
    xbr = [name for name in xbr_functions() if b"\nvoid " + name.encode() + b"(void)\n{" in text]
    failed = [m.group(1).decode() for m in FAILED.finditer(text)]
    text, ports = rewrite_ports(text)
    # FCI_PROBE=va,...: wrap the functions for fci_probe.c's snapshots (tools/oracle.py); diagnostic builds only
    probes = [
        p
        for p in os.environ.get("FCI_PROBE", "").split(",")
        if p and b"\nvoid sub_" + p.upper().encode() + b"(void)\n{" in text
    ]
    text, yields = add_yield_points(text)
    text, missing_gotos = MISSING_GOTO.subn(rb"fci_unresolved_stub(0x\1u); /* missing original branch label */", text)
    if not (hits or failed or ports or probes or yields or xbr or missing_gotos) and text == original:
        return source
    protos = []
    if missing_gotos:
        protos += [b"void fci_unresolved_stub(uint32_t va);"]
    if yields:
        protos += [b"extern volatile long g_fci_yield;", b"void guest_cpu_yield(void);"]
    for va in probes:
        name = b"sub_" + va.upper().encode()
        text = text.replace(
            b"\nvoid " + name + b"(void)\n{",
            b"\nvoid "
            + name
            + b"_fci_body(void);\nvoid "
            + name
            + b"(void)\n{ fci_probe_enter(0x"
            + va.encode()
            + b"u); "
            + name
            + b"_fci_body(); fci_probe_leave(0x"
            + va.encode()
            + b"u); }\nvoid "
            + name
            + b"_fci_body(void)\n{",
            1,
        )
        protos += [b"void fci_probe_enter(uint32_t va);", b"void fci_probe_leave(uint32_t va);"]
    if ports:
        protos += [
            b"void fci_wbinvd(uint32_t va);",
            b"uint32_t fci_pushfd(uint32_t va);",
            b"void fci_cli(uint32_t va);",
            b"void fci_sti(uint32_t va);",
            b"void fci_popfd(uint32_t value, uint32_t va);",
            b"void fci_cpuid(uint32_t va);",
            b"void fci_port_out(uint32_t port, uint32_t value, unsigned size, uint32_t va);",
            b"uint32_t fci_port_in(uint32_t port, unsigned size, uint32_t va);",
        ]
    if failed:  # the translator's empty body would return silently: stop instead (exit 85)
        text = FAILED.sub(rb"void \1(void) { fci_quarantined_lift(0x\2u); }", text)
        protos.append(b"void fci_quarantined_lift(uint32_t va);")
    for name in hits:
        kind = REPLACE[name][0]
        head = b"\nvoid " + name.encode() + b"(void)\n{"
        n = name.encode()
        hook = REPLACE[name][2].encode() if len(REPLACE[name]) > 2 else n[4:]
        if kind == "prehook":
            text = text.replace(head, b"\nvoid " + n + b"_fci_body(void)\n{", 1)
            text += b"\nvoid " + n + b"(void) { fci_prehook_" + hook + b"(); " + n + b"_fci_body(); }\n"
            protos += [b"void fci_prehook_" + hook + b"(void);", b"void " + n + b"_fci_body(void);"]
            continue
        if kind == "gate":
            text = text.replace(head, b"\nvoid " + n + b"_fci_body(void)\n{", 1)
            text += b"\nvoid " + n + b"(void) { if (fci_gate_" + hook + b"()) return; " + n + b"_fci_body(); }\n"
            protos += [b"int fci_gate_" + hook + b"(void);", b"void " + n + b"_fci_body(void);"]
            continue
        start = text.index(head) + len(head)
        end = text.index(b"\n}\n", start)
        call = b"fci_quarantined_lift(0x" + n[4:] + b"u)" if kind == "quarantine" else b"fci_override_" + n[4:] + b"()"
        text = text[:start] + b"\n    " + call + b";" + text[end:]
        protos.append(
            b"void fci_quarantined_lift(uint32_t va);"
            if kind == "quarantine"
            else b"void fci_override_" + n[4:] + b"(void);"
        )
    # host Direct3D and DirectSound (src/xbr): the lifted body becomes xbr_orig_<name>_c, and <name> picks
    # one at run time (fci_xbr_enabled, or fci_xbr_audio_enabled for DirectSound)
    audio = xbr_audio_functions() if xbr else set()
    for name in xbr:
        n = name.encode()
        switch = b"fci_xbr_audio_enabled" if name in audio else b"fci_xbr_enabled"
        text = text.replace(b"\nvoid " + n + b"(void)\n{", b"\nvoid xbr_orig_" + n + b"_c(void)\n{", 1)
        text += b"\nvoid " + n + b"(void) { if (" + switch + b"()) xbr_" + n + b"(); else xbr_orig_" + n + b"_c(); }\n"
        protos += [b"int " + switch + b"(void);", b"void xbr_" + n + b"(void);", b"void xbr_orig_" + n + b"_c(void);"]
    text = text.replace(
        b'#include "recomp_funcs.h"', b'#include "recomp_funcs.h"\n' + b"\n".join(sorted(set(protos))), 1
    )
    out = work / "gen-r" / (prefix + source.name)
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(text)
    record.setdefault("replaced", []).append(
        {
            "source": prefix + source.name,
            "functions": hits,
            "xbr": xbr,
            "probes": probes,
            "yield_points": yields,
            "normalized_crlf": original != original.replace(b"\r\n", b"\n"),
            "missing_gotos_stopped": missing_gotos,
            "failed_translations": failed,
            "port_io_rewritten": ports,
            "source_sha256": digest(source),
            "rewritten_sha256": digest(out),
        }
    )
    return out


def auto_quarantine(chunk, syntax_argv, work, record):
    """a function whose body is not valid C (data decoded as code) becomes a visible stop (exit 85);
    the compiler's error is recorded as the reason"""
    text = chunk.read_bytes()
    quarantined = {}
    for _ in range(6):
        done = subprocess.run(syntax_argv + [str(chunk)], capture_output=True)
        if done.returncode == 0:
            break
        errors = ERROR_LINE.findall((done.stdout + done.stderr).decode(errors="replace"))
        lines = text.split(b"\n")
        names = {}
        for number, code, message in errors:
            i = int(number) - 1
            while i >= 0 and not re.match(rb"void sub_[0-9A-F]{8}\(void\)$", lines[i]):
                i -= 1
            if i < 0:
                raise ValueError(f"error outside a function in {chunk.name}: {code} {message}")
            names.setdefault(lines[i][5:17].decode(), f"{code} {message.strip()}")
        if not names:
            raise ValueError(f"unparsed compile failure in {chunk.name}")
        for name, why in names.items():
            head = b"\nvoid " + name.encode() + b"(void)\n{"
            start = text.index(head) + len(head)
            end = text.index(b"\n}\n", start)
            text = text[:start] + b"\n    fci_quarantined_lift(0x" + name[4:].encode() + b"u);" + text[end:]
            quarantined[name] = why
        if b"void fci_quarantined_lift(uint32_t va);" not in text:
            text = text.replace(
                b'#include "recomp_funcs.h"', b'#include "recomp_funcs.h"\nvoid fci_quarantined_lift(uint32_t va);', 1
            )
        chunk = work / "gen-q" / chunk.name  # same name, so the object keeps its name
        chunk.parent.mkdir(exist_ok=True)
        chunk.write_bytes(text)
    else:
        raise ValueError(f"auto-quarantine did not converge for {chunk.name}")
    if quarantined:
        record.setdefault("auto_quarantine", {}).update(quarantined)
    return chunk


def lift_symbols(inputs):
    """what each lift defines: (base definitions, base unresolved stubs, supplemental definitions the base
    lacks, supplemental definitions the base has too, names the supplement uses that nothing defines)"""

    def scan(files, pattern):
        found = set()
        for f in files:
            found.update(
                m.group(1).decode() if m.groups() else m.group(0).decode() for m in pattern.finditer(f.read_bytes())
            )
        return found

    gen, supp = inputs / "gen", inputs / "supp"
    base_defs = scan(sorted(gen.glob("recomp_0*.c")), DEFINITION)
    base_stubs = scan([gen / "recomp_stubs_unresolved.c"], STUB)
    if not (supp / "recomp_funcs.h").exists():
        return base_defs, base_stubs, set(), set(), set()
    chunks = sorted(supp.glob("recomp_0*.c"))
    supp_defs = scan(chunks, DEFINITION)
    supp_new = supp_defs - base_defs
    missing = scan(chunks, REFERENCE) - base_defs - base_stubs - supp_new
    return base_defs, base_stubs, supp_new, supp_defs & base_defs, missing


def supplemental_names(supp):
    return lift_symbols(supp.parent)[2]


def toolchain(inputs):
    """the compiler, linker and paths from inputs/toolchain.json (tools/toolchain.py)"""
    tc = json.loads((inputs / "toolchain.json").read_text(encoding="utf-8"))
    return (
        Path(tc["cl"]),
        Path(tc["link"]),
        Path(tc["dumpbin"]),
        [Path(p) for p in tc["include"]],
        Path(tc["winrt"]),
        [Path(p) for p in tc["lib"]],
    )


def compile_groups(group, inputs, work, obj, base, inc, winrt, dumpbin, record):
    toolkit, gen = inputs / "toolkit", inputs / "gen"
    src = toolkit / "src"
    rt_inc = [
        toolkit / "include" / "xbox",
        src,
        src / "kernel",
        src / "d3d",
        src / "nv2a",
        src / "apu",
        src / "audio",
        src / "input",
        src / "usb",
        src / "video",
        src / "platform",
    ]
    groups = []  # (flags, sources)
    if group == "runtime":
        for lib, files in RUNTIME_SOURCES.items():
            extra = ["/GS-", "/Gy"] if lib == "kernel" else []
            groups.append(
                (["/O2", "/W3", "/Z7", *extra] + ["/I" + str(p) for p in rt_inc + inc], [src / lib / f for f in files])
            )
    elif group == "host":
        groups.append(
            (
                ["/O2", "/W4", "/Z7", "/we4013"] + ["/I" + str(p) for p in [gen] + rt_inc + inc],
                sorted((inputs / "host").glob("*.c")),
            )
        )
    elif group == "xbr":
        # Xbox XBE Recomp's D3D8 and DirectSound layers and the Direct3D 12 renderer (src/xbr), with the
        # addresses tools/sigmatch.py found in this image (inputs/xbr-symbols/image_symbols.h)
        xbr = inputs / "host" / "xbr"
        inc_xbr = [xbr, inputs / "xbr-symbols"] + inc + [winrt]
        for cpp in sorted(xbr.rglob("*.cpp")):
            groups.append(
                (
                    [
                        "/std:c++20",
                        "/EHsc",
                        "/O2",
                        "/W3",
                        "/Z7",
                        "/Zc:preprocessor",
                        "/Fo:" + str(obj / ("xbr_" + cpp.stem + ".obj")),
                    ]
                    + ["/I" + str(p) for p in inc_xbr],
                    [cpp],
                )
            )
    elif group == "supp":
        # the map editor's measured entries (tools/lift.py supplement), reached through fci_supp_lookup
        names = sorted(supplemental_names(inputs / "supp"))
        table = work / "fci_supp_table.c"
        lines = ["#include <stdint.h>", "typedef void (*recomp_func_t)(void);"]
        lines += [f"void {n}(void);" for n in names]
        lines += ["static const struct { uint32_t va; recomp_func_t fn; } supp[] = {"]
        lines += [f"    {{ 0x{n[4:]}u, {n} }}," for n in names] + ["    { 0u, 0 }", "};"]
        lines += [
            "recomp_func_t fci_supp_lookup(uint32_t va)",
            "{",
            "    unsigned i;",
            "    for (i = 0; supp[i].fn; i++)",
            "        if (supp[i].va == va) return supp[i].fn;",
            "    return 0;",
            "}",
        ]
        table.write_text("\n".join(lines) + "\n", encoding="utf-8")
        extra = sorted(lift_symbols(inputs)[4])  # used but defined nowhere: visible stops
        stubs = work / "supp_stubs_failvisible.c"
        stubs.write_text(
            "#include <stdint.h>\nvoid fci_unresolved_stub(uint32_t va);\n"
            + "".join(f"void {n}(void) {{ fci_unresolved_stub(0x{n[4:]}u); }}\n" for n in extra),
            encoding="utf-8",
        )
        record["supplement"] = {"functions": names, "table_sha256": digest(table), "new_unresolved_stubs": extra}
        groups.append((["/O1", "/W0"] + ["/I" + str(p) for p in inc], [table, stubs]))
    elif group.startswith("supp:"):
        index = int(group.split(":")[1])
        chunk = replace_bodies(inputs / "supp" / f"recomp_{index:04d}.c", work, record, prefix="supp-")
        duplicates = lift_symbols(inputs)[3]
        text = chunk.read_bytes()
        renamed = [n for n in duplicates if b"\nvoid " + n.encode() + b"(void)\n{" in text]
        for n in renamed:  # the base lift's definition wins; this copy is unused
            text = text.replace(
                b"\nvoid " + n.encode() + b"(void)\n{", b"\nstatic void fci_dup_" + n.encode() + b"(void)\n{", 1
            )
        if renamed:
            chunk = work / "gen-r" / ("d-" + chunk.name)
            chunk.parent.mkdir(exist_ok=True)
            chunk.write_bytes(text)
            record["duplicates_of_base"] = sorted(renamed)
        includes = ["/I" + str(p) for p in [inputs / "supp"] + inc]
        chunk = auto_quarantine(chunk, base + ["/W0", "/Zs"] + includes, work, record)
        groups.append((["/O1", "/W0", "/Fo:" + str(obj / f"supp_recomp_{index:04d}.obj")] + includes, [chunk]))
    elif group == "stubs":
        text = (gen / "recomp_stubs_unresolved.c").read_bytes()
        count = len(STUB.findall(text))
        supp_names = supplemental_names(inputs / "supp")
        dropped = [m.group(1).decode() for m in STUB.finditer(text) if m.group(1).decode() in supp_names]
        text = STUB.sub(
            lambda m: (
                b""
                if m.group(1).decode() in supp_names
                else b"void " + m.group(1) + b"(void) { fci_unresolved_stub(0x" + m.group(2) + b"u); }"
            ),
            text,
        )
        text = text.replace(
            b'#include "recomp_funcs.h"', b'#include "recomp_funcs.h"\nvoid fci_unresolved_stub(uint32_t va);', 1
        )
        if b"g_esp += 4; /*" in text:
            raise ValueError("unrecognized unresolved stub shape")
        out = work / "stubs_failvisible.c"
        out.write_bytes(text)
        record["stubs"] = {
            "rewritten": count - len(dropped),
            "defined_by_supplement": dropped,
            "source_sha256": digest(gen / "recomp_stubs_unresolved.c"),
            "rewritten_sha256": digest(out),
        }
        groups.append((["/O2", "/W0"] + ["/I" + str(p) for p in [gen] + inc], [out]))
    elif group == "dispatch":
        groups.append((["/O2", "/W0"] + ["/I" + str(p) for p in [gen] + inc], [gen / "recomp_dispatch.c"]))
    elif group.startswith("gen:"):
        first, last = (int(n) for n in group.split(":")[1:])
        sources = [replace_bodies(gen / f"recomp_{i:04d}.c", work, record) for i in range(first, last + 1)]
        syntax = base + ["/W0", "/Zs"] + ["/I" + str(p) for p in [gen] + inc]
        jobs = int(os.environ.get("FCI_JOBS", "4"))
        with ThreadPoolExecutor(jobs) as pool:
            sources = list(pool.map(lambda s: auto_quarantine(s, syntax, work, record), sources))
        record["gen_opt"] = GEN_OPT
        parallel = [f"/MP{jobs}"] if jobs > 1 and len(sources) > 1 else []
        # FCI_CALL_PROFILE=1 (diagnostic): every direct call counts its target (fci_probe.c)
        prof = ["/DRECOMP_CALL_PROFILE"] if os.environ.get("FCI_CALL_PROFILE") == "1" else []
        record["call_profile"] = bool(prof)
        cache = os.environ.get("FCI_OBJ_CACHE")
        if cache:  # build.py --obj-cache: an unchanged file's object from an earlier build
            sources = cached_objects(Path(cache), sources, gen, inputs, base + [GEN_OPT, "/W0", *prof], obj, record)
        if sources:
            groups.append(([GEN_OPT, "/W0", *parallel, *prof] + ["/I" + str(p) for p in [gen] + inc], sources))
    else:
        raise ValueError("unknown group")
    record["compiles"] = []
    for flags, sources in groups:
        out_flag = [] if any(f.startswith("/Fo:") for f in flags) else ["/Fo:" + str(obj) + "\\"]
        std = [] if any(f.startswith("/std:c++") for f in flags) else ["/std:c11"]
        argv = [a for a in base if a != "/std:c11"] + std + flags + ["/c"] + out_flag + [str(s) for s in sources]
        done = subprocess.run(argv, capture_output=True)
        record["compiles"].append(
            {"argv": argv, "exit": done.returncode, "sources": {s.name: digest(s) for s in sources}}
        )
        if done.returncode:
            print((done.stdout + done.stderr).decode(errors="replace")[-12000:])
            break
    if group == "xbr" and all(c["exit"] == 0 for c in record["compiles"]):
        list_xbr(inputs, work, obj, base, inc, dumpbin, record)
    if record.get("cache_keys") and all(c["exit"] == 0 for c in record["compiles"]):
        for name, key in record["cache_keys"].items():
            shutil.copy2(obj / (Path(name).stem + ".obj"), Path(os.environ["FCI_OBJ_CACHE"]) / (key + ".obj"))


def cached_objects(cache, sources, gen, inputs, flags, obj, record):
    """copy the objects the cache has for these sources into obj and return the rest; the key is what
    the compile reads (the rewritten source, the generated headers, the profile header it force-includes,
    the compiler) and its flags without the build's own paths"""
    cache.mkdir(parents=True, exist_ok=True)
    shared = hashlib.sha256(record["compiler_sha256"].encode())
    shared.update("\0".join(f for f in flags if not f.startswith(("/FI", "/I", "/Fo"))).encode())
    for header in [*sorted(gen.glob("*.h")), inputs / "image-profile.h"]:
        if header.exists():
            shared.update(header.name.encode() + b"\0" + header.read_bytes())
    missing, record["cached"], record["cache_keys"] = [], [], {}
    for source in sources:
        h = shared.copy()
        h.update(source.read_bytes())
        key = h.hexdigest()
        if (cache / (key + ".obj")).exists():
            shutil.copy2(cache / (key + ".obj"), obj / (source.stem + ".obj"))
            os.utime(cache / (key + ".obj"))  # used by this build: kept when the cache is pruned
            record["cached"].append(source.name)
        else:
            missing.append(source)
            record["cache_keys"][source.name] = key
    return missing


def list_xbr(inputs, work, obj, base, inc, dumpbin, record):
    """the functions the D3D8 and DirectSound objects define (xbr_sub_X) and wrap (xbr_orig_sub_X_c): the
    generated files give those functions their run-time switch (replace_bodies)"""
    defined, wrapped, audio = set(), set(), set()
    for name in ("xbr_d3d8.obj", "xbr_dsound.obj"):
        dump = subprocess.run([str(dumpbin), "/symbols", str(obj / name)], capture_output=True)
        text = dump.stdout.decode(errors="replace")
        here = set(re.findall(r"SECT\w+\s+notype \(\)\s+External\s+\|\s+xbr_(sub_[0-9A-F]{8})\b", text))
        defined |= here
        wrapped |= set(re.findall(r"UNDEF\s+notype \(\)\s+External\s+\|\s+xbr_orig_(sub_[0-9A-F]{8})_c\b", text))
        if name == "xbr_dsound.obj":
            audio |= here
    present = lambda names: sorted(n for n in names if not n.startswith("sub_FFFF"))  # noqa: E731
    listing = {
        "replace": present(defined),
        "wrap": present(wrapped),
        "audio": present(audio),
        "symbols_sha256": digest(inputs / "xbr-symbols" / "image_symbols.h"),
    }
    for out in (inputs / "xbr-replace.json", work / "xbr-replace.json"):
        out.write_text(json.dumps(listing, indent=1), encoding="utf-8")
    # wraps of functions this image lacks (placeholder addresses) must link; reached anyway, they stop
    absent = [n for n in sorted(wrapped) if n.startswith("sub_FFFF")]
    stub = work / "xbr_absent.c"
    stub.write_text(
        "#include <stdint.h>\nvoid fci_quarantined_lift(uint32_t va);\n"
        + "".join(f"void xbr_orig_{n}_c(void) {{ fci_quarantined_lift(0x{n[4:]}u); }}\n" for n in absent)
    )
    done = subprocess.run(
        base + ["/O2", "/W3", "/c", "/Fo:" + str(obj / "xbr_absent.obj"), str(stub)] + ["/I" + str(p) for p in inc],
        capture_output=True,
    )
    record["compiles"].append({"argv": ["xbr_absent.c"], "exit": done.returncode, "sources": {}})
    record["xbr"] = {"replace": len(listing["replace"]), "wrap": listing["wrap"]}


def main():
    global REPLACE
    inputs = Path(__file__).resolve().parent
    work = inputs.parent / "work"
    obj = work / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    profile_path = inputs / "image-profile.json"
    profile = json.loads(profile_path.read_text()) if profile_path.exists() else None
    if profile:
        REPLACE = {name: tuple(value) for name, value in profile["replacements"].items()}
        from image_profile import profile_header

        if (inputs / "image-profile.h").read_text(encoding="utf-8") != profile_header(profile):
            raise ValueError("compile header does not match image profile")
    action = sys.argv[1]
    cl, link, dumpbin, inc, winrt, libpaths = toolchain(inputs)
    record = {"action": sys.argv[1:], "compiler_sha256": digest(cl), "linker_sha256": digest(link)}
    if profile:
        record["image_profile_sha256"] = digest(profile_path)
        record["image_header_sha256"] = digest(inputs / "image-profile.h")
    base = [
        str(cl),
        "/nologo",
        "/std:c11",
        "/MT",
        "/bigobj",
        "/DWIN32_LEAN_AND_MEAN",
        "/DNOMINMAX",
        "/D_CRT_SECURE_NO_WARNINGS",
        "/D_USE_MATH_DEFINES",
    ]
    if profile:
        base += ["/FI" + str(inputs / "image-profile.h")]
    if action == "cc":
        compile_groups(sys.argv[2], inputs, work, obj, base, inc, winrt, dumpbin, record)
    elif action == "kinit":
        # the kernel-image metadata DLL (src/kinit): no entry point, no CRT, no absolute fixups
        src = inputs / "host" / "kinit" / "fci_kinit.c"
        cc = base + ["/O2", "/W4", "/GS-", "/c", "/Fo:" + str(obj) + "\\", str(src)]
        done = subprocess.run(cc, capture_output=True)
        record["compiles"] = [{"argv": cc, "exit": done.returncode, "sources": {src.name: digest(src)}}]
        if done.returncode == 0:
            ld = [
                str(link),
                "/NOLOGO",
                "/DLL",
                "/NOENTRY",
                "/NODEFAULTLIB",
                "/DYNAMICBASE",
                "/FIXED:NO",
                "/INCREMENTAL:NO",
                "/MANIFEST:NO",
                "/SECTION:INIT,ERD",
                "/OUT:" + str(work / "fci-kinit.dll"),
                str(obj / "fci_kinit.obj"),
            ]
            linked = subprocess.run(ld, capture_output=True)
            record["link"] = {"argv": ld, "exit": linked.returncode}
            if linked.returncode == 0:
                record["link"]["dll_sha256"] = digest(work / "fci-kinit.dll")
            else:
                print((linked.stdout + linked.stderr).decode(errors="replace")[-4000:])
        else:
            print((done.stdout + done.stderr).decode(errors="replace")[-4000:])
    elif action == "link":
        objs = sorted((inputs / "obj").glob("*.obj"))
        out = work / "fci-whole.exe"
        manifest = work / "fci-whole-manifest.res"
        write_manifest_res(inputs / "host" / "fci-whole.manifest", manifest)
        argv = (
            [
                str(link),
                "/NOLOGO",
                "/OUT:" + str(out),
                "/SUBSYSTEM:WINDOWS",
                "/ENTRY:mainCRTStartup",
                "/LARGEADDRESSAWARE",
                "/DEBUG:FULL",
                "/PDB:" + str(work / "fci-whole.pdb"),
                "/MAP:" + str(work / "fci-whole.map"),
                "/INCREMENTAL:NO",
                "/MANIFEST:NO",  # src/fci-whole.manifest comes in as a resource
                "/STACK:16777216",
            ]
            + ["/LIBPATH:" + str(p) for p in libpaths]
            + SYSTEM_LIBS
            + [str(o) for o in objs]
            + [str(manifest)]
        )
        done = subprocess.run(argv, capture_output=True)
        log = (done.stdout + done.stderr).decode(errors="replace")
        (work / "link.log").write_text(log, encoding="utf-8")
        record["link"] = {"argv_head": argv[:12], "objects": len(objs), "exit": done.returncode}
        if done.returncode == 0:
            record["link"]["exe_sha256"] = digest(out)
        else:
            print(log[-12000:])
    else:
        raise ValueError("unknown action")
    name = "-".join(a.replace(":", "_") for a in sys.argv[1:])
    (work / f"build-{name}.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    ok = all(c["exit"] == 0 for c in record.get("compiles", [])) and record.get("link", {}).get("exit", 0) == 0
    print(json.dumps({"action": sys.argv[1:], "ok": ok}))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
