/* Host entry: maps the XBE, starts the kernel bridge and runs the translated game entry point.
 * usage: <exe> <xbe> <game dir> <save dir> [RECOMP_*=value ...]; exit codes 2, 3 and 8x (fci_manual.c) */
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_types.h"

#include "fci_image.h"

extern void xbe_entry_point(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

static void dump_guest(const char *why)
{
    uint32_t i;
    fprintf(stderr, "[FCI] %s\n", why);
    fprintf(stderr, "  guest eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp, g_ebp);
    fprintf(stderr, "  recent ICALL targets:");
    for (i = 0; i < ICALL_TRACE_SIZE; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n");
    if (g_xbox_mem_offset && g_esp >= 0x1000 && g_esp < 0x7FFF0000u) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0;
        fprintf(stderr, "  guest stack code words (innermost first):");
        for (i = 0; i < 512 && shown < 32; i++) {
            if (sp[i] >= g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, " [%u]%08X", i * 4, sp[i]);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
}

int fci_host_symbol(DWORD64 pc, char *name, size_t size, DWORD64 *disp, DWORD64 *symbol_size);

static void print_host_symbol(DWORD64 address)
{
    char name[256];
    DWORD64 disp = 0;
    if (fci_host_symbol(address, name, sizeof name, &disp, NULL))
        fprintf(stderr, "  host %s+0x%llX\n", name, (unsigned long long)disp);
}

/* NV2A registers (guest FD000000, 16 MiB): trapped by fci_nv2a.c, decoded by nv2a_mmio_hook.c */
extern _Bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write);

/* MCPX APU (guest FE800000, 512 KiB): the runtime's model, active with RECOMP_AC97_READY */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern _Bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write);

/* USB: both OHCI controllers with an Xbox controller fed from XInput or a script (RECOMP_USB=1) */
void xbox_OhciInit(void);
int xbox_OhciOwnsAddress(uint32_t xbox_va);
int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
void xbox_InputInit(void);

static LONG CALLBACK fci_veh(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1)) {
        /* RECOMP_FCI_DATA_WATCH (fci_threads.c): a write to the watched dword */
        extern void fci_data_watch_hit(PCONTEXT ctx);
        fci_data_watch_hit(ep->ContextRecord);
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
        if (fault - (uintptr_t)g_xbox_mem_offset >= 0xFD000000ull
            && fault - (uintptr_t)g_xbox_mem_offset < 0xFE000000ull
            && nv2a_hook_handle_mmio(ep->ContextRecord, fault, va,
                                     ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (g_apu_state && va >= 0xFE800000u && va < 0xFE880000u
            && apu_hook_handle_mmio(ep->ContextRecord, fault, va,
                                    ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (xbox_OhciOwnsAddress(va) && xbox_OhciHandleMmio(ep->ContextRecord, va))
            return EXCEPTION_CONTINUE_EXECUTION; /* USB host controllers (RECOMP_USB) */
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION
        || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW
        || code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INVALID_HANDLE
        || code == STATUS_HEAP_CORRUPTION || code == STATUS_STACK_BUFFER_OVERRUN) {
        uintptr_t fault
                = code == EXCEPTION_ACCESS_VIOLATION ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        fprintf(stderr, "[FCI] host exception %08lX rip=%llX fault=%llX guestva=%08X %s\n", code,
                (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)fault,
                (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset),
                code == EXCEPTION_ACCESS_VIOLATION
                        ? (ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read")
                        : "");
        print_host_symbol(ep->ContextRecord->Rip);
        dump_guest("stopped on host exception");
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int load_file(const char *path, void **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || !(*data = malloc((size_t)n)) || fread(*data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    *size = (size_t)n;
    return 1;
}

/* The game (004AD2E4) reads the kernel's PE header at 80010000 and expects a last section named INIT:
 * fci-kinit.dll is a real image laid out that way, and its loaded header is copied there unchanged. */
void fci_cpu_init(void);
void fci_disk_init(const char *save_dir);

static int install_kernel_header(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash;
    HMODULE dll;
    const uint8_t *image;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *last;
    unsigned (*kinit)(volatile unsigned *);
    volatile unsigned ran = 0;
    uintptr_t init;
    uint32_t headers;
    uint8_t *page = (uint8_t *)((uintptr_t)0x80010000u + g_xbox_mem_offset);

    if (!n || n >= MAX_PATH || !(slash = wcsrchr(path, L'\\')))
        return 0;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"fci-kinit.dll");
    if (!(dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR))) {
        fprintf(stderr, "[FCI] cannot load fci-kinit.dll (%lu)\n", GetLastError());
        return 0;
    }
    image = (const uint8_t *)dll;
    dos = (const IMAGE_DOS_HEADER *)image;
    nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
    last = IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections - 1;
    kinit = (unsigned (*)(volatile unsigned *))GetProcAddress(dll, "fci_kinit");
    init = (uintptr_t)kinit - (uintptr_t)image;
    headers = nt->OptionalHeader.SizeOfHeaders;
    if (!kinit || dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE
        || memcmp(last->Name, "INIT\0\0\0\0", 8) || init < last->VirtualAddress
        || init >= (uintptr_t)last->VirtualAddress + last->Misc.VirtualSize
        || !(last->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) || headers > 0x1000u
        || nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        fprintf(stderr, "[FCI] fci-kinit.dll does not satisfy the final-INIT kernel header contract\n");
        return 0;
    }
    if (kinit(&ran) != 0x54494E49u || ran != 0x54494E49u)
        return 0;
    memset(page, 0, 0x1000u);
    memcpy(page, image, headers);
    fprintf(stderr,
            "[FCI] kernel header: fci-kinit.dll loaded header, pe=%X sections=%u optsize=%X "
            "INIT rva=%X initializer rva=%llX executed\n",
            (unsigned)dos->e_lfanew, nt->FileHeader.NumberOfSections, nt->FileHeader.SizeOfOptionalHeader,
            last->VirtualAddress, (unsigned long long)init);
    fci_cpu_init();
    return 1;
}

/* KeTickCount is a data export the game reads directly; the runtime only advanced it from the NV2A
 * thread fci_nv2a.c replaces, so it is kept in milliseconds here (the splash waits on it, 00200EE0). */
static DWORD WINAPI kernel_clock(LPVOID unused)
{
    (void)unused;
    for (;;) {
        MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();
        Sleep(1);
    }
}

void guest_cpu_join(void);
void fci_replay(void);
void fci_watchdog_start(void);
void fci_profiler_start(void);
void fci_register_guest_thread(uint32_t routine, uint32_t context);
void guest_cpu_part(void);

/* The XBE and game folder this run uses, from the arguments or fci.ini (host DirectSound reads both) */
const char *fci_xbe_path, *fci_game_dir, *fci_save_dir;
int fci_launch_argc;
char **fci_launch_argv;
void fci_config_load(int *argc, char ***argv); /* fci_config.c: fci.ini when started without arguments */

int main(int argc, char **argv)
{
    void *xbe = NULL;
    size_t xbe_size = 0;

    { /* a relaunched instance (game <-> map editor) keeps the first one's log */
        const char *log = getenv("RECOMP_FCI_RELAUNCH_LOG");
        if (log && *log) {
            freopen(log, "a", stderr);
            freopen(log, "a", stdout);
        }
    }
    fci_config_load(&argc, &argv);
    if (argc >= 4) {
        fci_xbe_path = argv[1];
        fci_game_dir = argv[2];
        fci_save_dir = argv[3];
        fci_launch_argc = argc;
        fci_launch_argv = argv;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s <default.xbe> <game dir> <save dir> [RECOMP_*=value ...]\n", argv[0]);
        return 2;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    /* The console has one CPU: one guest thread runs guest code at a time, released across kernel
     * calls. The runtime switches this build depends on are set here, not taken from the environment. */
    _putenv("RECOMP_GUEST_LOCK=1");
    /* events the XDK builds in its own memory (D3D waits on one) are real dispatcher objects */
    _putenv("RECOMP_TITLE_KEVENTS=1");
    /* DirectSound drives the APU directly; the runtime models it only with RECOMP_AC97_READY */
    _putenv("RECOMP_AC97_READY=1");
    _putenv("RECOMP_KERNEL_LOG_BUDGET=3000"); /* kernel trace lines */
    /* NtFreeVirtualMemory returns memory to the guest heap; without it every free failed and Story
     * Mode's streamer ran out of memory. It also lets freed contiguous blocks be reused. */
    _putenv("RECOMP_HEAP_RECLAIM=1");
    /* 1 ms timer resolution: the game's short sleeps and timed waits would otherwise stretch to the
     * 15.6 ms default tick, which slowed loading to a crawl */
    timeBeginPeriod(1);
    /* options from the command line or fci.ini: RECOMP_* switches only */
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "RECOMP_", 7) || !strchr(argv[i], '=')) {
            fprintf(stderr, "[FCI] rejected option %s\n", argv[i]);
            return 2;
        }
        _putenv(argv[i]);
        fprintf(stderr, "[FCI] option %s\n", argv[i]);
    }
    if (!strcmp(FCI_IMAGE_FILENAME, "ige.xbe")) {
        const char *script = getenv("RECOMP_FCI_EDITOR_PAD_SCRIPT");
        if (script)
            _putenv_s("RECOMP_PAD_SCRIPT", script);
        {
            const char *live = getenv("RECOMP_FCI_EDITOR_PAD_LIVE");
            if (live)
                _putenv_s("RECOMP_PAD_LIVE", live);
        }
        {
            const char *capture = getenv("RECOMP_FCI_EDITOR_CAPTURE");
            if (capture)
                _putenv_s("RECOMP_FCI_XBR_CAPTURE", capture);
        }
    }
    AddVectoredExceptionHandler(1, fci_veh);

    if (!load_file(argv[1], &xbe, &xbe_size) || xbe_size != FCI_XBE_SIZE) {
        fprintf(stderr, "[FCI] cannot load the original XBE (%s)\n", argv[1]);
        return 2;
    }
    if (memcmp(xbe, "XBEH", 4)
        || (*(const uint32_t *)((const char *)xbe + 0x128) ^ 0xA8FC57ABu) != FCI_ENTRY_VA) {
        fprintf(stderr, "[FCI] XBE does not match compiled image %s entry %08X\n", FCI_IMAGE_FILENAME,
                FCI_ENTRY_VA);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "[FCI] memory layout init failed\n");
        return 2;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    /* main RAM; the APU model maps contiguous physical addresses to the 80000000 window itself */
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    fprintf(stderr, "[FCI] APU model %s\n", g_apu_state ? "up" : "FAILED");
    xbox_InputInit();
    /* the console's front sockets: four root ports on the first controller, root port 2 being controller 1
     * (then 3, 0, 1 for controllers 2 to 4); with the toolkit's two ports the pad was controller 3 */
    if (!getenv("RECOMP_USB_NDP"))
        _putenv_s("RECOMP_USB_NDP", "4");
    if (!getenv("RECOMP_USB_PORT"))
        _putenv_s("RECOMP_USB_PORT", "2");
    xbox_OhciInit(); /* inert unless RECOMP_USB=1 */
    fprintf(stderr, "[FCI] guest memory offset %llX code [%08X,%08X)\n",
            (unsigned long long)g_xbox_mem_offset, g_xbox_code_lo, g_xbox_code_hi);
    if (!install_kernel_header())
        return 2;
    xbox_kernel_init();
    /* every PsCreateSystemThreadEx creates a real host thread; the entry's own thread returns */
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    xbox_path_init(argv[2], argv[3]);
    fci_disk_init(argv[3]);
    xbox_kernel_bridge_init();
    { /* XeImageFileName names the image compiled into this process */
        const char name[] = "\\Device\\CdRom0\\" FCI_IMAGE_FILENAME;
        uint32_t str = XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
        uint32_t buf = XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_BUF;
        for (size_t i = 0; i < sizeof(name); i++)
            MEM8(buf + i) = (uint8_t)name[i];
        MEM16(str) = sizeof(name) - 1;
        MEM16(str + 2) = sizeof(name);
        MEM32(str + 4) = buf;
        fprintf(stderr, "[FCI] compiled image %s entry %08X\n", FCI_IMAGE_FILENAME, FCI_ENTRY_VA);
        {
            uint32_t page = MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);
            if (page) {
                extern uint32_t fci_launch_page_hash(uint32_t page);
                fprintf(stderr, "[FCI] received launch page type=%u title=%08X hash=%08X\n", MEM32(page),
                        MEM32(page + 4), fci_launch_page_hash(page));
            }
        }
    }
    if (!CreateThread(NULL, 0, kernel_clock, NULL, 0, NULL)) {
        fprintf(stderr, "[FCI] cannot start the kernel clock\n");
        return 2;
    }
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[FCI] flat dispatch unavailable; binary-search lookup in use\n");
    fci_replay(); /* diagnostic: RECOMP_FCI_REPLAY runs one lifted call and exits (fci_probe.c) */
    {             /* host Direct3D 12 (src/xbr): the D3D8 entry points switch to it with RECOMP_FCI_XBR=1 */
        extern void fci_xbr_init(void);
        fci_xbr_init();
    }
    fprintf(stderr, "[FCI] entering original entry %08X esp=%08X\n", FCI_ENTRY_VA, g_esp);
    xbox_WatchdogStart(); /* inert unless RECOMP_WATCHDOG_SECS is given */
    fci_watchdog_start(); /* inert unless RECOMP_FCI_WATCHDOG is given */
    fci_profiler_start(); /* inert unless RECOMP_FCI_PROFILE is given */
    {
        extern void fci_call_profile_start(void);
        fci_call_profile_start(); /* inert unless RECOMP_FCI_CALL_PROFILE_DUMP is given */
    }
    {
        extern void fci_data_watch_start(void);
        fci_data_watch_start(); /* inert unless RECOMP_FCI_DATA_WATCH is given */
    }
    {
        extern void fci_memdump_start(void);
        extern void fci_status_start(void);
        fci_memdump_start(); /* inert unless RECOMP_FCI_MEMDUMP is given */
        fci_status_start();  /* inert unless RECOMP_FCI_STATUS is given */
    }
    fci_register_guest_thread(FCI_ENTRY_VA, 0);
    guest_cpu_join(); /* the entry is a guest thread too */
    xbe_entry_point();
    guest_cpu_part();
    fprintf(stderr, "[FCI] original entry returned eax=%08X\n", g_eax);
    {
        HANDLE game = (HANDLE)xbox_thread_debug_handle();
        DWORD code = 0;
        if (!game) {
            fprintf(stderr, "[FCI] no game thread was created\n");
            return 0;
        }
        /* the game's system thread runs the game from here */
        WaitForSingleObject(game, INFINITE);
        GetExitCodeThread(game, &code);
        fprintf(stderr, "[FCI] game thread exited %08lX\n", code);
    }
    return 0;
}
