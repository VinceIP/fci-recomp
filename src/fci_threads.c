/* Diagnostics: the guest thread registry, watchdog, profilers, data watchpoints, memory dumps and the
 * status file for test drivers, each switched on by its RECOMP_FCI_* option. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <mmsystem.h>

#include "recomp_types.h"

#define MAX_GUEST_THREADS 32

void fci_nv2a_report(FILE *out);
void nv2a_pb_exec_report(void);
void xbox_IrqlDumpHolders(void);

static struct {
    volatile uint32_t *esp, *eax, *ecx, *edx, *ebx, *esi, *edi, *ebp;
    uint32_t routine, context;
    DWORD host_id;
} threads[MAX_GUEST_THREADS];
static volatile LONG thread_count;

void fci_register_guest_thread(uint32_t routine, uint32_t context)
{
    LONG i = InterlockedIncrement(&thread_count) - 1;
    if (i >= MAX_GUEST_THREADS)
        return;
    threads[i].esp = &g_esp;
    threads[i].eax = &g_eax;
    threads[i].ecx = &g_ecx;
    threads[i].edx = &g_edx;
    threads[i].ebx = &g_ebx;
    threads[i].esi = &g_esi;
    threads[i].edi = &g_edi;
    threads[i].ebp = &g_ebp;
    threads[i].routine = routine;
    threads[i].context = context;
    threads[i].host_id = GetCurrentThreadId();
}

/* A host address's PDB name (generated functions are sub_<guest va>), or its hex value.
 * Returns 1 when the PDB named it; dbghelp is not thread safe, so callers are serialized. */
int fci_host_symbol(DWORD64 pc, char *name, size_t size, DWORD64 *disp, DWORD64 *symbol_size)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 d = 0;
    int ok;
    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    AcquireSRWLockExclusive(&lock);
    ok = SymFromAddr(GetCurrentProcess(), pc, &d, sym) ? 1 : 0;
    ReleaseSRWLockExclusive(&lock);
    if (ok) {
        /* a lifted function sub_<va> the reverse-engineering layer names (re_names.c, re/README.md) */
        extern const char *fci_re_name(uint32_t va);
        const char *re_name = NULL;
        if (!strncmp(sym->Name, "sub_", 4) && strlen(sym->Name) == 12)
            re_name = fci_re_name((uint32_t)strtoul(sym->Name + 4, NULL, 16));
        if (re_name)
            snprintf(name, size, "%s(%s)", sym->Name, re_name);
        else
            snprintf(name, size, "%s", sym->Name);
    } else
        snprintf(name, size, "%llX", (unsigned long long)pc);
    if (disp)
        *disp = ok ? d : 0;
    if (symbol_size)
        *symbol_size = ok ? sym->Size : 0;
    return ok;
}

/* A suspended thread's return addresses, unwound with the x64 unwind data. */
static int unwind_suspended(HANDLE th, DWORD64 *pcs, int max)
{
    CONTEXT ctx;
    int n = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &ctx))
        return 0;
    while (n < max && ctx.Rip) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn;
        pcs[n++] = ctx.Rip;
        fn = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!fn) { /* leaf: return address on the stack */
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void *handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, NULL);
        }
    }
    return n;
}

/* One host thread's call stack (suspended, unwound), named via the PDB. */
static void host_backtrace(DWORD tid)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    DWORD64 pcs[12];
    int f, n;
    if (!th)
        return;
    if (SuspendThread(th) == (DWORD)-1) {
        CloseHandle(th);
        return;
    }
    /* Addresses only while it is suspended: it may hold the CRT or dbghelp lock. */
    n = unwind_suspended(th, pcs, 12);
    ResumeThread(th);
    CloseHandle(th);
    fprintf(stderr, "  host thread %lu:", tid);
    for (f = 0; f < n; f++) {
        char name[256];
        DWORD64 disp;
        if (fci_host_symbol(pcs[f], name, sizeof name, &disp, NULL))
            fprintf(stderr, " %s+%llX", name, (unsigned long long)disp);
        else
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
}

/* RECOMP_FCI_PROFILE=<start s>,<seconds>: sample every thread's RIP each millisecond, then print the
 * hottest functions */
#define PROF_MAX 2000000
static DWORD64 *prof_pc;
static LONG prof_n;

static int prof_cmp(const void *a, const void *b)
{
    DWORD64 x = *(const DWORD64 *)a, y = *(const DWORD64 *)b;
    return x < y ? -1 : x > y;
}

static DWORD WINAPI profiler(LPVOID param)
{
    const char *spec = (const char *)param;
    unsigned start = (unsigned)atoi(spec), secs = 10, i;
    const char *comma = strchr(spec, ',');
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    ULONGLONG until;
    if (comma)
        secs = (unsigned)atoi(comma + 1);
    prof_pc = (DWORD64 *)malloc(sizeof(DWORD64) * PROF_MAX);
    if (!prof_pc)
        return 0;
    Sleep(start * 1000u);
    timeBeginPeriod(1);
    until = GetTickCount64() + secs * 1000ull;
    while (GetTickCount64() < until && prof_n < PROF_MAX - 64) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        if (snap == INVALID_HANDLE_VALUE) {
            Sleep(1);
            continue;
        }
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && prof_n < PROF_MAX - 1; ok = Thread32Next(snap, &te)) {
            HANDLE th;
            CONTEXT ctx;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            if (!(th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)))
                continue;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(th) != (DWORD)-1) {
                if (GetThreadContext(th, &ctx))
                    prof_pc[prof_n++] = ctx.Rip;
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        Sleep(1);
    }
    timeEndPeriod(1);
    { /* aggregate by symbol start: sort PCs, name runs, keep the top 40 */
        struct {
            char name[64];
            LONG n;
        } top[512];
        int ntop = 0, t;
        qsort(prof_pc, (size_t)prof_n, sizeof(DWORD64), prof_cmp);
        for (i = 0; i < (unsigned)prof_n;) {
            char name[64];
            DWORD64 disp = 0, symbol_size = 0, lo, hi;
            unsigned j = i;
            fci_host_symbol(prof_pc[i], name, sizeof name, &disp, &symbol_size);
            lo = prof_pc[i] - disp;
            hi = lo + (symbol_size ? symbol_size : 1);
            while (j < (unsigned)prof_n && prof_pc[j] >= lo && prof_pc[j] < hi)
                j++;
            if (j == i)
                j = i + 1;
            for (t = 0; t < ntop && strcmp(top[t].name, name); t++)
                ;
            if (t == ntop && ntop < 512) {
                snprintf(top[ntop].name, sizeof top[ntop].name, "%s", name);
                top[ntop++].n = 0;
            }
            if (t < 512)
                top[t].n += (LONG)(j - i);
            i = j;
        }
        fprintf(stderr, "[FCI] PROFILE %us from %us: %ld samples\n", secs, start, prof_n);
        for (i = 0; i < 40; i++) {
            int best = -1;
            for (t = 0; t < ntop; t++)
                if (top[t].n > 0 && (best < 0 || top[t].n > top[best].n))
                    best = t;
            if (best < 0)
                break;
            fprintf(stderr, "  %6.2f%% %s\n", 100.0 * top[best].n / prof_n, top[best].name);
            top[best].n = -1;
        }
        fflush(stderr);
    }
    return 0;
}

/* RECOMP_FCI_STACKPROF=<start s>,<seconds>,<guest thread>: sample one thread's call stack every 2 ms and
 * print each function's inclusive and leaf share, waits included */
#define SP_DEPTH 16
#define SP_MAX   20000

static DWORD WINAPI stack_profiler(LPVOID param)
{
    const char *spec = (const char *)param, *p;
    unsigned start = (unsigned)atoi(spec), secs = 10, index = 1, n = 0, i, f;
    static DWORD64 pcs[SP_MAX][SP_DEPTH];
    static unsigned char depth[SP_MAX];
    static struct {
        char name[64];
        LONG incl, leaf;
    } tab[2048];
    int ntab = 0, t;
    HANDLE th;
    ULONGLONG until;
    if ((p = strchr(spec, ',')) != NULL) {
        secs = (unsigned)atoi(p + 1);
        if ((p = strchr(p + 1, ',')) != NULL)
            index = (unsigned)atoi(p + 1);
    }
    Sleep(start * 1000u);
    if (index >= (unsigned)thread_count || index >= MAX_GUEST_THREADS
        || !(th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                             threads[index].host_id))) {
        fprintf(stderr, "[FCI] STACKPROF: no guest thread %u\n", index);
        return 0;
    }
    timeBeginPeriod(1);
    until = GetTickCount64() + secs * 1000ull;
    while (GetTickCount64() < until && n < SP_MAX) {
        int got;
        if (SuspendThread(th) == (DWORD)-1)
            break;
        got = unwind_suspended(th, pcs[n], SP_DEPTH);
        ResumeThread(th);
        if (got)
            depth[n++] = (unsigned char)got;
        Sleep(2);
    }
    timeEndPeriod(1);
    CloseHandle(th);
    for (i = 0; i < n; i++) {
        int seen[SP_DEPTH], ns = 0, k;
        for (f = 0; f < depth[i]; f++) {
            char name[64];
            fci_host_symbol(pcs[i][f], name, sizeof name, NULL, NULL);
            for (t = 0; t < ntab && strcmp(tab[t].name, name); t++)
                ;
            if (t == ntab) {
                if (ntab == 2048)
                    continue;
                snprintf(tab[ntab].name, sizeof tab[ntab].name, "%s", name);
                tab[ntab].incl = tab[ntab].leaf = 0;
                ntab++;
            }
            if (f == 0)
                tab[t].leaf++;
            for (k = 0; k < ns && seen[k] != t; k++)
                ;
            if (k == ns) {
                seen[ns++] = t;
                tab[t].incl++;
            }
        }
    }
    fprintf(stderr, "[FCI] STACKPROF guest thread %u (routine %08X) %us from %us: %u samples\n", index,
            threads[index].routine, secs, start, n);
    for (i = 0; i < 60; i++) {
        int best = -1;
        for (t = 0; t < ntab; t++)
            if (tab[t].incl > 0 && (best < 0 || tab[t].incl > tab[best].incl))
                best = t;
        if (best < 0)
            break;
        fprintf(stderr, "  %6.2f%% incl %6.2f%% leaf %s\n", 100.0 * tab[best].incl / n,
                100.0 * tab[best].leaf / n, tab[best].name);
        tab[best].incl = -1;
    }
    fflush(stderr);
    return 0;
}

void fci_profiler_start(void)
{
    const char *spec = getenv("RECOMP_FCI_PROFILE");
    if (spec)
        CloseHandle(CreateThread(NULL, 0, profiler, (LPVOID)spec, 0, NULL));
    if ((spec = getenv("RECOMP_FCI_STACKPROF")) != NULL)
        CloseHandle(CreateThread(NULL, 0, stack_profiler, (LPVOID)spec, 0, NULL));
}

static void all_host_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
            host_backtrace(te.th32ThreadID);
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID param)
{
    unsigned secs = (unsigned)(uintptr_t)param;
    LONG n, i;
    Sleep(secs * 1000u);
    n = thread_count < MAX_GUEST_THREADS ? thread_count : MAX_GUEST_THREADS;
    fprintf(stderr, "[FCI] WATCHDOG after %us: %ld guest threads, %llu icalls\n", secs, n,
            (unsigned long long)g_icall_count);
    for (i = 0; i < n; i++) {
        uint32_t esp, k;
        int shown = 0;
        { /* an exited thread's TLS (the register pointers below) is gone */
            HANDLE th = OpenThread(SYNCHRONIZE, FALSE, threads[i].host_id);
            DWORD alive = th ? WaitForSingleObject(th, 0) : WAIT_OBJECT_0;
            if (th)
                CloseHandle(th);
            if (alive != WAIT_TIMEOUT) {
                fprintf(stderr, "  thread %ld host %lu routine %08X: exited\n", i, threads[i].host_id,
                        threads[i].routine);
                continue;
            }
        }
        esp = *threads[i].esp;
        fprintf(stderr,
                "  thread %ld host %lu routine %08X ctx %08X\n"
                "    eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                i, threads[i].host_id, threads[i].routine, threads[i].context, *threads[i].eax,
                *threads[i].ecx, *threads[i].edx, *threads[i].ebx, *threads[i].esi, *threads[i].edi,
                *threads[i].ebp, esp);
        { /* where the host thread is: 4 samples of RIP, named via the PDB */
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threads[i].host_id);
            int s;
            fprintf(stderr, "    host:");
            for (s = 0; th && s < 4; s++) {
                CONTEXT ctx;
                char name[256];
                DWORD64 disp = 0;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(th) == (DWORD)-1)
                    break;
                GetThreadContext(th, &ctx);
                ResumeThread(th);
                if (fci_host_symbol(ctx.Rip, name, sizeof name, &disp, NULL))
                    fprintf(stderr, " %s+%llX", name, (unsigned long long)disp);
                else
                    fprintf(stderr, " %s", name);
                Sleep(7);
            }
            if (th)
                CloseHandle(th);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "    stack code words:");
        for (k = 0; esp >= 0x1000 && esp < 0x04000000u && k < 1024 && shown < 40; k++) {
            uint32_t v = MEM32(esp + k * 4);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " [%u]%08X", k * 4, v);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  recent icalls:");
    for (i = 0; i < ICALL_TRACE_SIZE; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n");
    fci_nv2a_report(stderr); /* the aperture itself is trapped */
    all_host_threads();      /* device/timer threads too: who is stuck where */
    if (getenv("RECOMP_PB_EXEC"))
        nv2a_pb_exec_report();
    xbox_IrqlDumpHolders();
    { /* RECOMP_FCI_PEEK=va[,va...]: 16 dwords at each va and at the va it holds */
        const char *p = getenv("RECOMP_FCI_PEEK");
        while (p && *p) {
            uint32_t va = (uint32_t)strtoul(p, (char **)&p, 16), k, target;
            for (int pass = 0;
                 pass < 2 && ((va >= 0x1000 && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u));
                 pass++) {
                fprintf(stderr, "  peek %08X:", va);
                for (k = 0; k < 16; k++)
                    fprintf(stderr, " %08X", MEM32(va + k * 4));
                fprintf(stderr, "\n");
                target = MEM32(va);
                va = target;
            }
            while (*p == ',' || *p == ' ')
                p++;
        }
    }
    { /* RECOMP_FCI_FIND=dword[,dword...]: guest RAM and the contiguous window addresses holding each */
        const char *s = getenv("RECOMP_FCI_FIND");
        static const uint32_t ranges[2][2] = {{0x00010000u, 0x04000000u}, {0x80000000u, 0x84000000u}};
        while (s && *s) {
            char *next;
            uint32_t want = (uint32_t)strtoul(s, &next, 16), shown = 0, r, va;
            s = *next == ',' ? next + 1 : NULL;
            fprintf(stderr, "  find %08X:", want);
            for (r = 0; r < 2; r++)
                for (va = ranges[r][0]; va < ranges[r][1] && shown < 64; va += 0x1000) {
                    MEMORY_BASIC_INFORMATION mbi;
                    uint32_t k;
                    if (!VirtualQuery((void *)XBOX_PTR(va), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT
                        || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                        continue;
                    for (k = 0; k < 0x1000 && shown < 64; k += 4)
                        if (MEM32(va + k) == want) {
                            fprintf(stderr, " %08X", va + k);
                            shown++;
                        }
                }
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 92);
    return 0;
}

void fci_watchdog_start(void)
{
    const char *s = getenv("RECOMP_FCI_WATCHDOG");
    unsigned secs = s ? (unsigned)atoi(s) : 0;
    HANDLE h;
    if (!secs)
        return;
    h = CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)secs, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* RECOMP_FCI_DATA_WATCH: a hardware watchpoint (DR0) on a guest dword in every thread; each hit prints
 * the lifted functions on the call chain */
static uintptr_t watch_host;
static int watch_rw; /* RECOMP_FCI_DATA_WATCH_RW=1: reads stop too */
static volatile LONG watch_hits;

void fci_data_watch_hit(PCONTEXT ctx)
{
    /* each distinct call chain once (up to 64); in read/write mode the value is not read here, which
     * would hit the watch again */
    static uint64_t seen[64];
    static volatile LONG nseen;
    void *frames[24];
    USHORT n, k;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)watch_host; /* a chain per watched address */
    (void)ctx;
    if (!watch_rw) { /* RECOMP_FCI_DATA_WATCH_VALUE=<hex>: only writes that store it */
        static const char *v;
        static int init;
        if (!init) {
            v = getenv("RECOMP_FCI_DATA_WATCH_VALUE");
            init = 1;
        }
        if (v && *(volatile uint32_t *)watch_host != (uint32_t)strtoul(v, NULL, 16))
            return;
    }
    if (InterlockedIncrement(&watch_hits) > 100000)
        return;
    n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (k = 0; k < n; k++)
        h = (h ^ (uint64_t)(uintptr_t)frames[k]) * 1099511628211ull;
    for (k = 0; k < (USHORT)nseen && k < 64; k++)
        if (seen[k] == h)
            return;
    if (nseen >= 64)
        return;
    seen[InterlockedIncrement(&nseen) - 1] = h;
    if (watch_rw)
        fprintf(stderr,
                "[FCI] data watch %08X (access #%ld):", (uint32_t)(watch_host - (uintptr_t)g_xbox_mem_offset),
                watch_hits);
    else
        fprintf(stderr,
                "[FCI] data watch %08X now %08X:", (uint32_t)(watch_host - (uintptr_t)g_xbox_mem_offset),
                *(volatile uint32_t *)watch_host);
    for (k = 3; k < n; k++) { /* past the handler and the exception dispatch */
        char name[256];
        if (fci_host_symbol((DWORD64)(uintptr_t)frames[k], name, sizeof name, NULL, NULL)
            && strncmp(name, "fci_", 4) && strncmp(name, "Rtl", 3) && strncmp(name, "KiUser", 6))
            fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static DWORD WINAPI data_watch_thread(LPVOID unused)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const char *after = getenv("RECOMP_FCI_DATA_WATCH_AFTER"); /* ms before arming: skip early writes */
    (void)unused;
    if (after)
        Sleep((DWORD)atoi(after));
    { /* *<global>+<offset>: resolved now, since heap objects move between runs but globals do not */
        const char *spec = getenv("RECOMP_FCI_DATA_WATCH");
        if (spec && *spec == '*') {
            char *plus;
            uint32_t global = (uint32_t)strtoul(spec + 1, &plus, 16);
            uint32_t offset = *plus == '+' ? (uint32_t)strtoul(plus + 1, NULL, 16) : 0;
            uint32_t target = MEM32(global) + offset;
            watch_host = (uintptr_t)g_xbox_mem_offset + target;
            fprintf(stderr, "[FCI] data watch: [%08X]+%X = %08X\n", global, offset, target);
        }
    }
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        if (snap == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            HANDLE th;
            CONTEXT c;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            if (!(th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                  te.th32ThreadID)))
                continue;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (SuspendThread(th) != (DWORD)-1) {
                if (GetThreadContext(th, &c) && c.Dr0 != watch_host) {
                    c.Dr0 = watch_host;
                    /* L0, 4 bytes; write, or read and write with RECOMP_FCI_DATA_WATCH_RW=1 */
                    c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (watch_rw ? 3ull << 16 : 1ull << 16)
                            | (3ull << 18);
                    SetThreadContext(th, &c);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
        Sleep(50);
    }
    return 0;
}

/* RECOMP_FCI_MEMDUMP=<ms>[,<ms>...]: guest RAM [00010000, 04000000) to memdump-<ms>.bin at those times
 * after start, while the game runs on */
static DWORD WINAPI memdump_thread(LPVOID param)
{
    const char *p = (const char *)param;
    ULONGLONG start = GetTickCount64();
    while (p && *p) {
        char *next, name[64];
        unsigned ms = (unsigned)strtoul(p, &next, 10);
        FILE *f;
        ULONGLONG now = GetTickCount64() - start;
        if (now < ms)
            Sleep((DWORD)(ms - now));
        snprintf(name, sizeof name, "memdump-%u.bin", ms);
        if ((f = fopen(name, "wb")) != NULL) {
            fwrite((const void *)XBOX_PTR(0x00010000u), 1, 0x04000000u - 0x00010000u, f);
            fclose(f);
            fprintf(stderr, "[FCI] memdump %s at %llu ms\n", name,
                    (unsigned long long)(GetTickCount64() - start));
        }
        p = *next == ',' ? next + 1 : next;
    }
    return 0;
}

/* RECOMP_FCI_STATUS=<file>: the game's state as one JSON line, ten times a second, for test drivers;
 * RECOMP_FCI_CONTROL=<file>: commands appended while the game runs (tools/game_driver.py) */
static volatile LONG status_presents, status_polls, status_frames3d, status_movies;
static char status_movie[160];

void fci_status_present(void) { InterlockedIncrement(&status_presents); }
void fci_status_counts(long *presents, long *polls, long *frames3d, long *movies)
{
    *presents = status_presents;
    *polls = status_polls;
    *frames3d = status_frames3d;
    *movies = status_movies;
}
void fci_prehook_001F6C00(void) { InterlockedIncrement(&status_polls); }
static volatile uint32_t
        status_view; /* Camera_BuildViews' argument: the game view (re/notes/INPUT-CAMERA.md) */
void fci_prehook_000F0500(void)
{
    extern void fci_trace_frame(uint32_t view); /* RECOMP_FCI_TRACE (fci_trace.c) */
    status_view = MEM32(g_esp + 4);
    InterlockedIncrement(&status_frames3d);
    fci_trace_frame(status_view);
}
uint32_t fci_game_view(void) { return status_view; }
void fci_status_movie(const char *path)
{
    snprintf(status_movie, sizeof status_movie, "%s", path);
    InterlockedIncrement(&status_movies);
}

static uint32_t status_resolve(const char *expr)
{
    if (*expr == '*') {
        char *plus;
        uint32_t global = (uint32_t)strtoul(expr + 1, &plus, 16);
        uint32_t base = MEM32(global);
        return base ? base + (*plus == '+' ? (uint32_t)strtoul(plus + 1, NULL, 16) : 0) : 0;
    }
    return (uint32_t)strtoul(expr, NULL, 16);
}

static void memdump_now(const char *tag)
{
    char name[96];
    FILE *f;
    snprintf(name, sizeof name, "memdump-%s.bin", tag);
    if ((f = fopen(name, "wb")) != NULL) {
        fwrite((const void *)XBOX_PTR(0x00010000u), 1, 0x04000000u - 0x00010000u, f);
        fclose(f);
        fprintf(stderr, "[FCI] memdump %s\n", name);
    }
}

static DWORD WINAPI status_thread(LPVOID param)
{
    const char *path = (const char *)param, *watch = getenv("RECOMP_FCI_STATUS_WATCH");
    const char *control = getenv("RECOMP_FCI_CONTROL");
    ULONGLONG start = GetTickCount64();
    long control_at = 0;
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    for (;;) {
        FILE *f = fopen(tmp, "w");
        if (f) {
            char movie[200];
            size_t j = 0;
            for (const char *m = status_movie; *m && j < sizeof movie - 2; m++) { /* JSON string */
                if (*m == '\\' || *m == '"')
                    movie[j++] = '\\';
                movie[j++] = *m;
            }
            movie[j] = 0;
            fprintf(f,
                    "{\"ms\": %llu, \"presents\": %ld, \"polls\": %ld, \"frames3d\": %ld, \"movies\": %ld, "
                    "\"movie\": \"%s\", \"view\": \"%08X\"",
                    (unsigned long long)(GetTickCount64() - start), status_presents, status_polls,
                    status_frames3d, status_movies, movie, status_view);
            {
                extern double fci_game_clock(void), fci_replay_left(void); /* fci_trace.c, fci_record.c */
                fprintf(f, ", \"game_s\": %.3f, \"replay_left\": %.3f", fci_game_clock(), fci_replay_left());
            }
            for (const char *w = watch; w && *w;) { /* name=expr pairs */
                const char *eq = strchr(w, '='), *end = strchr(w, ',');
                if (!eq || (end && eq > end))
                    break;
                {
                    char expr[64];
                    size_t len = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
                    uint32_t va;
                    snprintf(expr, sizeof expr, "%.*s", (int)(len < 63 ? len : 63), eq + 1);
                    va = status_resolve(expr);
                    fprintf(f, ", \"%.*s\": \"%08X\"", (int)(eq - w), w,
                            va >= 0x10000u && va < 0x04000000u ? MEM32(va) : 0u);
                }
                w = end ? end + 1 : NULL;
            }
            fprintf(f, "}\n");
            fclose(f);
            MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
        }
        if (control) { /* new command lines since last time */
            FILE *c = fopen(control, "r");
            if (c) {
                char line[128];
                if (fseek(c, control_at, SEEK_SET) == 0)
                    while (fgets(line, sizeof line, c)) {
                        char *nl = strchr(line, '\n');
                        if (!nl)
                            break; /* a line still being written */
                        *nl = 0;
                        control_at = ftell(c);
                        if (!strncmp(line, "dump ", 5))
                            memdump_now(line + 5);
                        else if (!strncmp(line, "mouse ", 6)) {
                            extern void fci_input_mouse_inject(int dx, int dy);
                            int dx = 0, dy = 0;
                            if (sscanf(line + 6, "%d %d", &dx, &dy) == 2)
                                fci_input_mouse_inject(dx, dy);
                        } else if (!strncmp(line, "set ", 4)) {
                            extern int fci_gfx_set(const char *key_value);  /* xbr/fci_enhance.cpp */
                            extern int fci_cvar_set(const char *key_value); /* fci_cvars.c */
                            if (!fci_cvar_set(line + 4) && !fci_gfx_set(line + 4))
                                fprintf(stderr, "[FCI] control: refused %s\n", line);
                        } else if (!strncmp(line, "watch ", 6) || !strncmp(line, "watchrw ", 8)) {
                            extern void fci_data_watch_set(uint32_t va, int rw);
                            const char *arg = line + (line[5] == 'r' ? 8 : 6);
                            fci_data_watch_set(status_resolve(arg), line[5] == 'r');
                        }
                    }
                fclose(c);
            }
        }
        Sleep(100);
    }
    return 0;
}

void fci_status_start(void)
{
    const char *s = getenv("RECOMP_FCI_STATUS");
    if (s && *s)
        CloseHandle(CreateThread(NULL, 0, status_thread, (LPVOID)s, 0, NULL));
}

void fci_memdump_start(void)
{
    const char *s = getenv("RECOMP_FCI_MEMDUMP");
    if (s && *s)
        CloseHandle(CreateThread(NULL, 0, memdump_thread, (LPVOID)s, 0, NULL));
}

/* RECOMP_FCI_CONTROL "watch <va>" / "watchrw <va>": the arming thread moves every DR0 within 50 ms */
static volatile LONG watch_thread_started;

void fci_data_watch_set(uint32_t va, int rw)
{
    watch_rw = rw;
    watch_host = (uintptr_t)g_xbox_mem_offset + va;
    fprintf(stderr, "[FCI] data watch now %08X%s\n", va, rw ? " (reads too)" : "");
    if (!InterlockedExchange(&watch_thread_started, 1))
        CloseHandle(CreateThread(NULL, 0, data_watch_thread, NULL, 0, NULL));
}

void fci_data_watch_start(void)
{
    const char *s = getenv("RECOMP_FCI_DATA_WATCH");
    if (!s)
        return;
    watch_host = *s == '*' ? 0 : (uintptr_t)g_xbox_mem_offset + (uint32_t)strtoul(s, NULL, 16);
    watch_rw = getenv("RECOMP_FCI_DATA_WATCH_RW") && *getenv("RECOMP_FCI_DATA_WATCH_RW") == '1';
    watch_thread_started = 1;
    CloseHandle(CreateThread(NULL, 0, data_watch_thread, NULL, 0, NULL));
}
