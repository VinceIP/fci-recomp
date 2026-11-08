/* Guest CPU state the lifter does not translate: the GDT descriptor helper 004B03B4 (sgdt, cli, a far
 * jump), EFLAGS, CPUID, cli/sti and 80-bit x87 operands. Anything else stops (86). */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <windows.h>

#include "recomp_types.h"
#include "fci_image.h"

#define FCI_GDT_VA    0x00740F00u
#define FCI_FLAT_LOW  0x0000FFFFu
#define FCI_FLAT_HIGH 0x00CF9B00u
#define FCI_IF        0x200u

static struct {
    uint32_t cached_low, cached_high, eflags, calls;
    int live;
} cpu;

static void cpu_stop(const char *why, uint32_t a, uint32_t b)
{
    fprintf(stderr, "[FCI] STOP 86 guest CPU: %s (%08X %08X) esp=%08X\n", why, a, b, g_esp);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 86);
}

static int code_descriptor(uint32_t low, uint32_t high, uint32_t *limit)
{
    uint32_t base = (low >> 16) | ((high & 0xFFu) << 16) | (high & 0xFF000000u);
    unsigned access = (high >> 8) & 0xFFu;
    if (base || (access != 0x9Au && access != 0x9Bu) || (high & 0x00600000u) != 0x00400000u)
        return 0;
    *limit = (low & 0xFFFFu) | (high & 0x000F0000u);
    if (high & 0x00800000u)
        *limit = (*limit << 12) | 0xFFFu;
    return 1;
}

/* Called once by the host after the memory layout exists. */
void fci_cpu_init(void)
{
    uint32_t i;
    for (i = 0; i < 16; i++)
        if (MEM8(FCI_GDT_VA + i))
            cpu_stop("GDT storage not zero at init", FCI_GDT_VA + i, MEM8(FCI_GDT_VA + i));
    MEM32(FCI_GDT_VA + 8) = FCI_FLAT_LOW;
    MEM32(FCI_GDT_VA + 12) = FCI_FLAT_HIGH;
    cpu.cached_low = FCI_FLAT_LOW;
    cpu.cached_high = FCI_FLAT_HIGH;
    cpu.eflags = 0x202u;
    cpu.live = 1;
}

void fci_override_004B03B4(void)
{
    uint32_t sp = g_esp, low, high, base, limit, old_low, old_high, ret;
    if (!cpu.live || MEM32(FCI_GDT_VA) || MEM32(FCI_GDT_VA + 4))
        cpu_stop("virtual GDT state", MEM32(FCI_GDT_VA), MEM32(FCI_GDT_VA + 4));
    low = MEM32(sp + 4);  /* mov ecx,[esp+4] */
    high = MEM32(sp + 8); /* mov edx,[esp+8] */
    MEM16(sp + 6) = 15;   /* sgdt [esp+6]: limit, then base */
    MEM32(sp + 8) = FCI_GDT_VA;
    base = MEM32(sp + 8);  /* mov eax,[esp+8] */
    cpu.eflags &= ~FCI_IF; /* cli */
    if (base != FCI_GDT_VA)
        cpu_stop("descriptor table write outside entry 1", base, 0);
    old_low = (uint32_t)InterlockedExchange((volatile LONG *)XBOX_PTR(base + 8), (LONG)low);
    old_high = (uint32_t)InterlockedExchange((volatile LONG *)XBOX_PTR(base + 12), (LONG)high);
    cpu.eflags |= FCI_IF;                    /* sti; the shadow covers ljmp only */
    if (!code_descriptor(low, high, &limit)) /* ljmp 8:004B03D4 reloads cached CS */
        cpu_stop("unsupported code descriptor", low, high);
    if (FCI_DESCRIPTOR_JUMP > limit)
        cpu_stop("far jump target beyond new CS limit", FCI_DESCRIPTOR_JUMP, limit);
    if (!(high & 0x100u)) {
        high |= 0x100u; /* accessed bit set on load */
        MEM32(base + 12) = high;
    }
    cpu.cached_low = low;
    cpu.cached_high = high;
    ret = MEM32(sp);
    if (ret < g_xbox_code_lo || ret >= g_xbox_code_hi || ret > limit)
        cpu_stop("return outside cached CS", ret, limit);
    g_eax = old_low; /* mov eax,ecx */
    g_ecx = old_low;
    g_edx = old_high;
    g_esp = sp + 12; /* ret 8 */
    cpu.calls++;
    fprintf(stderr, "[FCI] cpu descriptor jump %08X #%u new %08X/%08X limit %08X old %08X/%08X ret %08X\n",
            FCI_DESCRIPTOR_JUMP, cpu.calls, low, high, limit, old_low, old_high, ret);
}

/* pushfd/popfd keep only EFLAGS' control bits (IF, DF, AC, ID; arithmetic flags read as 0), and CPUID
 * answers as the console's Pentium III for leaves 0 to 2 (the CRT's MMX check, 004FFED0; 004C3D20 on the
 * network path stores leaf 2 without asking for the highest leaf) */
#define FCI_EFLAGS_WRITABLE (0x400u | 0x40000u | 0x200000u) /* DF, AC, ID */
static RECOMP_TLS uint32_t eflags_ac_id;                    /* per thread, like IF and DF */

/* cli/sti (XPP's USB driver, 005C352C): IF is per thread; cli raises the thread to HIGH_LEVEL, which
 * holds off ISRs and DPCs on the other host threads, and sti restores the level */
extern int xbox_IrqlEnterInterrupt(int level);
extern void xbox_IrqlLeaveInterrupt(int saved);
static RECOMP_TLS int t_if_clear, t_if_saved_irql;

void fci_cli(uint32_t va)
{
    (void)va;
    if (!t_if_clear) {
        t_if_clear = 1;
        t_if_saved_irql = xbox_IrqlEnterInterrupt(31);
    }
}

void fci_sti(uint32_t va)
{
    (void)va;
    if (t_if_clear) {
        t_if_clear = 0;
        xbox_IrqlLeaveInterrupt(t_if_saved_irql);
    }
}

uint32_t fci_pushfd(uint32_t va)
{
    uint32_t ifl = (cpu.eflags & FCI_IF) && !t_if_clear ? FCI_IF : 0;
    uint32_t value = 0x2u | ifl | (g_df ? 0x400u : 0) | eflags_ac_id;
    (void)va;
    return value;
}

void fci_popfd(uint32_t value, uint32_t va)
{
    if (value & FCI_IF)
        fci_sti(va);
    else
        fci_cli(va);
    g_df = (value & 0x400u) ? 1 : 0;
    eflags_ac_id = value & (0x40000u | 0x200000u);
}

void fci_cpuid(uint32_t va)
{
    static unsigned calls;
    uint32_t leaf = g_eax;
    if (leaf == 0) {
        g_eax = 2;           /* highest leaf answered */
        g_ebx = 0x756E6547u; /* "Genu" */
        g_edx = 0x49656E69u; /* "ineI" */
        g_ecx = 0x6C65746Eu; /* "ntel" */
    } else if (leaf == 1) {
        g_eax = 0x00000680u; /* family 6, model 8, stepping not claimed */
        g_ebx = 0;
        g_ecx = 0;
        g_edx = 0x0383F9FFu; /* see above */
    } else if (leaf == 2) {  /* cache and TLB descriptors of a 128 KB L2 Coppermine; only stored */
        g_eax = 0x03020101u;
        g_ebx = 0;
        g_ecx = 0;
        g_edx = 0x0C040841u;
    } else {
        cpu_stop("CPUID leaf not modeled", leaf, va);
    }
    if (calls++ < 8)
        fprintf(stderr, "[FCI] cpuid leaf %u at %08X\n", leaf, va);
}

/* x87 80-bit memory operands (fld/fstp xword): the lifted FPU stack holds doubles, so a load rounds
 * the mantissa to 53 bits and a store writes the double exactly */
double fci_ld80(uint32_t va)
{
    uint64_t m = (uint64_t)MEM32(va) | (uint64_t)MEM32(va + 4) << 32;
    uint16_t se = MEM16(va + 8);
    int e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF)
        v = (m << 1) ? NAN : INFINITY;
    else
        v = ldexp((double)m, e - 16383 - 63); /* also zero and denormals */
    return (se & 0x8000) ? -v : v;
}

void fci_st80(uint32_t va, double v)
{
    uint64_t m = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        se |= 0x7FFF;
        m = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFF;
        m = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int ex;
        double f = frexp(fabs(v), &ex); /* f in [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);     /* exact: 53 significant bits */
        se |= (uint16_t)(ex - 1 + 16383);
    }
    MEM32(va) = (uint32_t)m;
    MEM32(va + 4) = (uint32_t)(m >> 32);
    MEM16(va + 8) = se;
}
