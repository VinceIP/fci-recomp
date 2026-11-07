"""Direct-branch recovery in the patched lifter (patches/lifter/fci_direct.py), on synthetic code"""

from pathlib import Path
import subprocess

import pytest

BASE = 0x11000


@pytest.fixture(autouse=True)
def _toolkit(lifter_toolkit):
    return lifter_toolkit


def translate(raw, entry, end, others=()):
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator

    config._install(
        [config.Section(".text", BASE, len(raw), 0, len(raw), True)],
        entry_point=BASE + entry,
        kernel_thunk_addr=BASE,
        origin="direct-branch-test",
    )
    db = {
        BASE + entry: {"_addr": BASE + entry, "start": f"0x{BASE + entry:08X}", "end": BASE + end, "size": end - entry}
    }
    db.update(
        {BASE + a: {"_addr": BASE + a, "start": f"0x{BASE + a:08X}", "end": BASE + b, "size": b - a} for a, b in others}
    )
    t = FunctionTranslator(raw, db, seh_prolog=0, seh_epilog=0)
    return t.translate_function(BASE + entry, db[BASE + entry]), t


class TestDirectBranches:
    def test_forward_tail_keeps_live_flags(self):
        # cmp eax,1; je tail; mov eax,9; ret; [padding]; tail: jne fail; mov eax,7; ret
        raw = bytes.fromhex("83F801740BBB09000000C3CCCCCCCCCC7506B807000000C3B808000000C3")
        c, t = translate(raw, 0, 11)
        assert "loc_00011010:" in c
        assert "sub_00011010();" not in c
        assert "CMP_NE(_fa, _fb)" in c
        assert t._fci_direct_extended
        assert "dead code, label not in function" not in c

    def test_backward_tail_enters_at_original_entry(self):
        # tail: dec ecx; jnz tail; mov eax,7; ret; [padding]; entry: mov ecx,4; jmp tail
        raw = bytes.fromhex("4975FDB807000000C3CCCCCCCCCCCCCCB904000000EBE9")
        c, t = translate(raw, 16, len(raw))
        assert "goto loc_00011010; /* original entry */" in c
        assert "goto loc_00011000" in c
        assert "sub_00011000();" not in c
        assert "dead code, label not in function" not in c
        assert t._fci_direct_extended

    def test_existing_callable_entry_is_preserved(self):
        raw = bytes.fromhex("EB06CCCCCCCCCCCCB807000000C3")
        c, t = translate(raw, 0, 2, [(8, len(raw))])
        assert "sub_00011008();" in c
        assert not t._fci_direct_extended

    def test_overlap_is_left_unresolved(self):
        # target 1 is inside mov eax,imm32, not an instruction boundary.
        c, t = translate(bytes.fromhex("B801000000EBFAC3"), 0, 8)
        assert not t._fci_direct_extended

    def test_out_of_section_stays_unresolved(self):
        c, t = translate(bytes.fromhex("E9FBFF0100"), 0, 5)
        assert not t._fci_direct_extended
        assert "sub_00031000();" in c

    def test_trap_is_not_promoted_to_working_code(self):
        c, t = translate(bytes.fromhex("EB02CCCC0F0B"), 0, 2)
        assert not t._fci_direct_extended
        assert "sub_00011004();" in c

    def test_compiled_results_match_reference_x86(self, lifter_toolkit, compiler, tmp_path):
        unicorn = pytest.importorskip("unicorn")
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_ESP

        Uc, UC_ARCH_X86, UC_MODE_32 = unicorn.Uc, unicorn.UC_ARCH_X86, unicorn.UC_MODE_32
        scratch = tmp_path
        cases = [
            (bytes.fromhex("83F801740BBB09000000C3CCCCCCCCCC7506B807000000C3B808000000C3"), 0, 11),
            (bytes.fromhex("4975FDB807000000C3CCCCCCCCCCCCCCB904000000EBE9"), 16, 23),
        ]
        for index, (raw, entry, end) in enumerate(cases):
            generated, _ = translate(raw, entry, end)
            source = scratch / f"case{index}.c"
            exe = scratch / f"case{index}.exe"
            source.write_text(
                '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                "#include <stdio.h>\n#include <stdlib.h>\n"
                "RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_ebp,g_seh_ebp;\n"
                + generated
                + "\nint main(int argc,char **argv) { g_eax=(uint32_t)strtoul(argv[1],0,10); "
                + f"g_esp=0x100000; sub_{BASE + entry:08X}(); "
                + 'printf("%u %u %u %u",g_eax,g_ecx,g_ebx,g_esp); return 0; }\n'
            )
            command = [
                str(Path(compiler["bin"]) / "clang.exe"),
                "-O2",
                "-std=gnu11",
                "-fno-strict-aliasing",
                "-fwrapv",
                "-I" + str(lifter_toolkit / "templates" / "runtime"),
                str(source),
                "-o",
                str(exe),
            ]
            cc = subprocess.run(command, capture_output=True, text=True, timeout=60)
            assert cc.returncode == 0, cc.stdout + cc.stderr
            for value in (0, 1, 2, 0xFFFFFFFF):
                uc = Uc(UC_ARCH_X86, UC_MODE_32)
                uc.mem_map(BASE, 0x1000)
                uc.mem_write(BASE, raw)
                uc.mem_map(0x100000, 0x1000)
                uc.mem_write(0x100000, (0x200000).to_bytes(4, "little"))
                uc.reg_write(UC_X86_REG_EAX, value)
                uc.reg_write(UC_X86_REG_ESP, 0x100000)
                uc.emu_start(BASE + entry, 0x200000, count=1000)
                expected = [uc.reg_read(r) for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EBX, UC_X86_REG_ESP)]
                run = subprocess.run([str(exe), str(value)], capture_output=True, text=True, timeout=3)
                assert run.returncode == 0, run.stderr
                assert list(map(int, run.stdout.split())) == expected, (index, value, generated)

    def test_unknown_original_fallthrough_rejects_recovery(self):
        # je tail has an unknown fallthrough at 4. Do not accidentally run the appended tail.
        raw = bytes.fromhex("85C07404CCCCCCCCB807000000C3")
        c, t = translate(raw, 0, 4)
        assert not t._fci_direct_extended
        assert "sub_00011008();" in c

    def test_span_limit_keeps_distant_destination_unresolved(self):
        raw = bytes.fromhex("E9FBFF0000") + b"\xcc" * (0x10000 - 5) + b"\xc3"
        c, t = translate(raw, 0, 5)
        assert not t._fci_direct_extended
        assert "sub_00021000();" in c

    def test_added_stream_falls_into_existing_entry(self):
        # entry jumps over padding to mov, whose fallthrough is an independent entry.
        raw = bytes.fromhex("EB06CCCCCCCCCCCCB807000000C3")
        c, t = translate(raw, 0, 2, [(13, 14)])
        assert t._fci_direct_extended
        assert "sub_0001100D(); return; /* original fallthrough */" in c
