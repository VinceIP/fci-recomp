/* Debugging probes: snapshots of the guest state around one lifted call for tools/oracle.py, call logs,
 * the call profile and replay (a build with FCI_PROBE=<va>; tools/oracle.py reads the format). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"

/* Declared by recomp_types.h only for generated code; same layout (8 and 16
 * bytes) as RecompMmx/RecompXmm. */
typedef struct {
    uint8_t b[8];
} probe_mm;
extern RECOMP_TLS probe_mm g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

static volatile LONG calls;
static DWORD capturing_thread;
static uint32_t capturing_va;

static void dump_range(FILE *f, uint32_t lo, uint32_t hi)
{
    uint32_t va = lo;
    while (va < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        uint32_t run;
        if (!VirtualQuery((void *)XBOX_PTR(va), &mbi, sizeof mbi)) {
            va += 0x1000;
            continue;
        }
        run = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)XBOX_PTR(va));
        if (run > hi - va)
            run = hi - va;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            fwrite(&va, 4, 1, f);
            fwrite(&run, 4, 1, f);
            fwrite((void *)XBOX_PTR(va), 1, run, f);
        }
        va += run;
    }
}

static const char *snapshot_tag = "";

static void snapshot(uint32_t va, uint32_t phase)
{
    const char *prefix = getenv("RECOMP_FCI_PROBE_OUT");
    char path[MAX_PATH];
    uint32_t regs[11], zero[2] = {0, 0};
    FILE *f;
    if (!prefix)
        prefix = "probe";
    snprintf(path, sizeof path, "%s%s.%s", prefix, snapshot_tag, phase ? "post" : "pre");
    if (!(f = fopen(path, "wb"))) {
        fprintf(stderr, "[FCI] probe: cannot write %s\n", path);
        TerminateProcess(GetCurrentProcess(), 95);
    }
    regs[0] = g_eax;
    regs[1] = g_ecx;
    regs[2] = g_edx;
    regs[3] = g_ebx;
    regs[4] = g_esp;
    regs[5] = g_ebp;
    regs[6] = g_esi;
    regs[7] = g_edi;
    regs[8] = g_fs_base;
    regs[9] = g_seh_ebp;
    regs[10] = (uint32_t)g_df;
    fwrite("FCIPROBE", 1, 8, f);
    fwrite(&va, 4, 1, f);
    fwrite(&phase, 4, 1, f);
    fwrite(regs, 4, 11, f);
    fwrite(&g_mm0, 8, 1, f);
    fwrite(&g_mm1, 8, 1, f);
    fwrite(&g_mm2, 8, 1, f);
    fwrite(&g_mm3, 8, 1, f);
    fwrite(&g_mm4, 8, 1, f);
    fwrite(&g_mm5, 8, 1, f);
    fwrite(&g_mm6, 8, 1, f);
    fwrite(&g_mm7, 8, 1, f);
    fwrite(&g_xmm0, 16, 1, f);
    fwrite(&g_xmm1, 16, 1, f);
    fwrite(&g_xmm2, 16, 1, f);
    fwrite(&g_xmm3, 16, 1, f);
    fwrite(&g_xmm4, 16, 1, f);
    fwrite(&g_xmm5, 16, 1, f);
    fwrite(&g_xmm6, 16, 1, f);
    fwrite(&g_xmm7, 16, 1, f);
    dump_range(f, 0x00001000u, 0x04000000u);
    dump_range(f, 0x80000000u, 0x84000000u);
    fwrite(zero, 4, 2, f);
    fclose(f);
    fprintf(stderr, "[FCI] probe %08X %s written: %s (esp %08X)\n", va, phase ? "post" : "pre", path, g_esp);
}

/* Callee trace inside the captured call ("E" at entry, "R" at return, with hashes of the memory behind
 * the arguments): tools/oracle.py produces the same events, and the first mismatch brackets the bug */
static FILE *trace;
static struct {
    uint32_t va, a1, a2;
} nest[64];
static int depth, events, nested_event = -2, nested_depth = -1;

static uint32_t fnv(uint32_t h, uint32_t p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        h = (h ^ MEM8(p + i)) * 16777619u;
    return h;
}

static uint32_t block_hash(uint32_t base, uint32_t pitch)
{
    uint32_t h = 2166136261u, r;
    if (base < 0x10000u || base >= 0x03FF0000u || !pitch || pitch >= 0x1000u)
        return 0;
    for (r = 0; r < 8; r++)
        h = fnv(h, base + r * pitch, 16);
    return h;
}

static void trace_entry(uint32_t va)
{
    uint32_t k;
    if (!trace) {
        const char *prefix = getenv("RECOMP_FCI_PROBE_OUT");
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s.trace", prefix ? prefix : "probe");
        trace = fopen(path, "w");
        if (trace) /* unbuffered: a stop inside the call must not lose it */
            setvbuf(trace, NULL, _IONBF, 0);
    }
    if (depth < 64) {
        nest[depth].va = va;
        nest[depth].a1 = MEM32(g_esp + 4);
        nest[depth].a2 = MEM32(g_esp + 8);
    }
    depth++;
    /* RECOMP_FCI_PROBE_EVENT=<n>: snapshot this nested call on its own as
     * <prefix>.nested.pre/.post when it is trace event n (E and R lines count). */
    if (nested_event == -2) {
        const char *e = getenv("RECOMP_FCI_PROBE_EVENT");
        nested_event = e ? atoi(e) : -1;
    }
    if (events++ == nested_event) {
        snapshot_tag = ".nested";
        nested_depth = depth - 1;
        snapshot(va, 0);
    }
    if (!trace)
        return;
    fprintf(trace, "E %08X %08X %08X %08X %08X %08X %08X %08X", va, g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi,
            g_edi);
    for (k = 0; k < 8; k++)
        fprintf(trace, " %08X", MEM32(g_esp + 4 + k * 4));
    for (k = 0; k < 8; k++) {
        uint32_t p = MEM32(g_esp + 4 + k * 4);
        fprintf(trace, " %08X", p >= 0x10000u && p < 0x03FFFF00u ? fnv(2166136261u, p, 256) : 0u);
    }
    fprintf(trace, "\n");
}

static void trace_return(uint32_t va)
{
    if (--depth < 0 || depth >= 64 || nest[depth].va != va) {
        depth = depth < 0 ? 0 : depth;
        return;
    }
    events++;
    { /* RECOMP_FCI_PROBE_WATCH=<va>: report the first probed callee after
         * which that dword changed (a stack cookie overwritten, say) */
        static int init;
        static uint32_t watch, last;
        if (!init) {
            const char *w = getenv("RECOMP_FCI_PROBE_WATCH");
            init = 1;
            watch = w ? (uint32_t)strtoul(w, NULL, 16) : 0;
            if (watch)
                last = MEM32(watch);
        }
        if (watch && MEM32(watch) != last) {
            fprintf(stderr, "[FCI] probe watch %08X: %08X -> %08X after %08X returned (event %d, depth %d)\n",
                    watch, last, MEM32(watch), va, events, depth);
            last = MEM32(watch);
        }
    }
    if (depth == nested_depth) {
        snapshot(va, 1);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 95);
    }
    if (trace)
        fprintf(trace, "R %08X %08X\n", va, block_hash(nest[depth].a1, nest[depth].a2));
}

static _Thread_local uint32_t t_logged_va;
static _Thread_local uint32_t t_zero_va, t_zero_ret, t_zero_args[4];

void fci_probe_enter(uint32_t va)
{
    static LONG want = -1;
    static uint32_t only;
    static ULONGLONG after, start;
    if (want < 0) {
        const char *s = getenv("RECOMP_FCI_PROBE_CALL"), *v = getenv("RECOMP_FCI_PROBE_VA"),
                   *t = getenv("RECOMP_FCI_PROBE_AFTER_MS");
        only = v ? (uint32_t)strtoul(v, NULL, 16) : 0; /* several probed builds, one selected */
        after = t ? (ULONGLONG)atoi(t) : 0;            /* count calls only after this much run time */
        start = GetTickCount64();
        InterlockedExchange(&want, s ? atoi(s) : 1);
    }
    if (capturing_thread == GetCurrentThreadId() && va != capturing_va) {
        trace_entry(va);
        return;
    }
    if ((only && va != only) || (after && GetTickCount64() - start < after))
        return;
    { /* RECOMP_FCI_PROBE_LOG_ZERO=1: no snapshot; log only calls that return eax 0,
         * with their return address and 4 stack arguments (fci_probe_leave) */
        static int zero = -1;
        if (zero < 0)
            zero = getenv("RECOMP_FCI_PROBE_LOG_ZERO") != NULL;
        if (zero) {
            uint32_t i;
            t_zero_va = va;
            t_zero_ret = MEM32(g_esp);
            for (i = 0; i < 4; i++)
                t_zero_args[i] = MEM32(g_esp + 4 + i * 4);
            return;
        }
    }
    { /* RECOMP_FCI_PROBE_LOG=<n>: no snapshot; log the first n calls (then every
         * 10000th) with registers, 4 stack arguments and 8 dwords at esi/edi */
        static int log = -2;
        static volatile LONG logged;
        if (log == -2) {
            const char *l = getenv("RECOMP_FCI_PROBE_LOG");
            log = l ? atoi(l) : -1;
        }
        if (log >= 0) {
            LONG k = InterlockedIncrement(&logged);
            if (k <= log || k % 10000 == 0) {
                uint32_t esp = g_esp, i;
                fprintf(stderr,
                        "  [PROBE] #%ld %08X eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X "
                        "esp=%08X ebp=%08X ret=%08X args",
                        k, va, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, esp, g_ebp, MEM32(esp));
                for (i = 1; i <= 4; i++)
                    fprintf(stderr, " %08X", MEM32(esp + i * 4));
                { /* RECOMP_FCI_PROBE_DUMP=<va>: 8 dwords there too */
                    const char *d = getenv("RECOMP_FCI_PROBE_DUMP");
                    if (d) {
                        uint32_t dv = (uint32_t)strtoul(d, NULL, 16), j;
                        fprintf(stderr, " | [%08X]", dv);
                        for (j = 0; j < 8; j++)
                            fprintf(stderr, " %08X", MEM32(dv + j * 4));
                    }
                }
                for (i = 0; i < 2; i++) {
                    uint32_t p = i ? g_edi : g_esi, j;
                    if ((p >= 0x1000 && p < 0x04000000u) || (p >= 0x80000000u && p < 0x84000000u)) {
                        fprintf(stderr, " | [%s]", i ? "edi" : "esi");
                        for (j = 0; j < 12; j++)
                            fprintf(stderr, " %08X", MEM32(p + j * 4));
                    }
                }
                fprintf(stderr, "\n");
                t_logged_va = va;
            }
            return;
        }
    }
    if (InterlockedIncrement(&calls) == want) {
        capturing_thread = GetCurrentThreadId();
        capturing_va = va;
        snapshot(va, 0);
    }
}

void fci_probe_leave(uint32_t va)
{
    if (t_zero_va == va) { /* RECOMP_FCI_PROBE_LOG_ZERO */
        static volatile LONG shown;
        t_zero_va = 0;
        if (g_eax == 0 && InterlockedIncrement(&shown) <= 64)
            fprintf(stderr, "  [PROBE] %08X returned 0: ret=%08X args %08X %08X %08X %08X\n", va, t_zero_ret,
                    t_zero_args[0], t_zero_args[1], t_zero_args[2], t_zero_args[3]);
    }
    if (t_logged_va == va) { /* RECOMP_FCI_PROBE_LOG: the logged call's result */
        t_logged_va = 0;
        fprintf(stderr, "  [PROBE] ret %08X eax=%08X edx=%08X\n", va, g_eax, g_edx);
    }
    if (!capturing_thread || capturing_thread != GetCurrentThreadId())
        return;
    if (va != capturing_va) { /* a probed callee's return */
        trace_return(va);
        return;
    }
    if (trace)
        fclose(trace);
    snapshot(va, 1);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 95);
}

/* RECOMP_FCI_REPLAY=<prefix>: run the lifted function a snapshot names once on that state, write
 * <RECOMP_FCI_PROBE_OUT>.post and exit 95 */
typedef void (*fci_replay_fn)(void);
extern fci_replay_fn recomp_lookup(uint32_t va);

void fci_replay(void)
{
    const char *prefix = getenv("RECOMP_FCI_REPLAY");
    char path[MAX_PATH], magic[8];
    uint32_t va, phase, regs[11], rva, len;
    FILE *f;
    fci_replay_fn fn;
    if (!prefix)
        return;
    snprintf(path, sizeof path, "%s.pre", prefix);
    if (!(f = fopen(path, "rb")) || fread(magic, 1, 8, f) != 8 || memcmp(magic, "FCIPROBE", 8)
        || fread(&va, 4, 1, f) != 1 || fread(&phase, 4, 1, f) != 1 || fread(regs, 4, 11, f) != 11) {
        fprintf(stderr, "[FCI] replay: cannot read %s\n", path);
        TerminateProcess(GetCurrentProcess(), 95);
    }
    fread(&g_mm0, 8, 1, f);
    fread(&g_mm1, 8, 1, f);
    fread(&g_mm2, 8, 1, f);
    fread(&g_mm3, 8, 1, f);
    fread(&g_mm4, 8, 1, f);
    fread(&g_mm5, 8, 1, f);
    fread(&g_mm6, 8, 1, f);
    fread(&g_mm7, 8, 1, f);
    fread(&g_xmm0, 16, 1, f);
    fread(&g_xmm1, 16, 1, f);
    fread(&g_xmm2, 16, 1, f);
    fread(&g_xmm3, 16, 1, f);
    fread(&g_xmm4, 16, 1, f);
    fread(&g_xmm5, 16, 1, f);
    fread(&g_xmm6, 16, 1, f);
    fread(&g_xmm7, 16, 1, f);
    while (fread(&rva, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && (rva || len)) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)XBOX_PTR(rva), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT
            && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY | PAGE_EXECUTE_READ)))
            fread((void *)XBOX_PTR(rva), 1, len, f);
        else
            fseek(f, (long)len, SEEK_CUR);
    }
    fclose(f);
    g_eax = regs[0];
    g_ecx = regs[1];
    g_edx = regs[2];
    g_ebx = regs[3];
    g_esp = regs[4];
    g_ebp = regs[5];
    g_esi = regs[6];
    g_edi = regs[7];
    g_fs_base = regs[8];
    g_seh_ebp = regs[9];
    g_df = (int)regs[10];
    if (!(fn = recomp_lookup(va))) {
        fprintf(stderr, "[FCI] replay: no lifted function at %08X\n", va);
        TerminateProcess(GetCurrentProcess(), 95);
    }
    fprintf(stderr, "[FCI] replay %08X from %s (esp %08X)\n", va, path, g_esp);
    fn();
    snapshot(va, 1);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 95);
}

/* Call profile (a build with FCI_CALL_PROFILE=1): every direct call counts its target, written to
 * RECOMP_FCI_CALL_PROFILE_OUT at a stop or RECOMP_FCI_CALL_PROFILE_DUMP=<s> seconds after start */
#define PROFILE_SLOTS (1u << 20)
static volatile LONG profile_va[PROFILE_SLOTS], profile_n[PROFILE_SLOTS];

void recomp_call_profile_hit(uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 12, i;
    for (i = 0; i < 64; i++) {
        uint32_t s = (h + i) & (PROFILE_SLOTS - 1);
        LONG cur = profile_va[s];
        if (cur == (LONG)va || (cur == 0 && InterlockedCompareExchange(&profile_va[s], (LONG)va, 0) == 0)
            || profile_va[s] == (LONG)va) {
            InterlockedIncrement(&profile_n[s]);
            return;
        }
    }
}

void fci_call_profile_dump(const char *why)
{
    const char *out = getenv("RECOMP_FCI_CALL_PROFILE_OUT");
    FILE *f;
    uint32_t s, n = 0;
    if (!out || !(f = fopen(out, "w")))
        return;
    fprintf(f, "# %s\n", why);
    for (s = 0; s < PROFILE_SLOTS; s++)
        if (profile_va[s]) {
            fprintf(f, "%08X %ld\n", (uint32_t)profile_va[s], profile_n[s]);
            n++;
        }
    fclose(f);
    fprintf(stderr, "[FCI] call profile: %u functions written to %s (%s)\n", n, out, why);
}

static DWORD WINAPI call_profile_timer(LPVOID p)
{
    Sleep((DWORD)(uintptr_t)p * 1000u);
    fci_call_profile_dump("timer");
    return 0;
}

void fci_call_profile_start(void)
{
    const char *t = getenv("RECOMP_FCI_CALL_PROFILE_DUMP");
    if (t)
        CloseHandle(CreateThread(NULL, 0, call_profile_timer, (LPVOID)(uintptr_t)atoi(t), 0, NULL));
}
