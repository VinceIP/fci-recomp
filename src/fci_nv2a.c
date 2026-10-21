/* NV2A register model and PFIFO pusher: register semantics, interrupts, the pushbuffer and vblank for
 * what the game's D3D library uses. Malformed commands and stalls stop (exit 93). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"
#include "fci_image.h"

#define APERTURE      0xFD000000u
#define APERTURE_SIZE 0x01000000u
#define CONTIG        0x80000000u

#define PMC_INTR_0          0x000100u
#define PMC_INTR_EN_0       0x000140u
#define PMC_PFIFO           (1u << 8)
#define PMC_PGRAPH          (1u << 12)
#define PMC_PCRTC           (1u << 24)
#define PFIFO_INTR_0        0x002100u
#define PFIFO_INTR_EN_0     0x002140u
#define PFIFO_RUNOUT_STATUS 0x002400u
#define PFIFO_CACHE1_STATUS 0x003214u
#define PFB_FLUSH           0x100410u
#define PGRAPH_INTR         0x400100u
#define PGRAPH_NSOURCE      0x400108u
#define PGRAPH_INTR_EN      0x400140u
#define PGRAPH_TRAPPED_ADDR 0x400704u
#define PGRAPH_TRAPPED_DATA 0x400708u
#define PGRAPH_INTR_ERROR   (1u << 20)
#define PGRAPH_SURFACE      0x400710u
#define SURFACE_WRITE_3D    0x00700000u
#define SURFACE_READ_3D     0x07000000u
#define SURFACE_MODULO_3D   0x70000000u
#define PGRAPH_INCREMENT    0x40071Cu
#define PGRAPH_PATT_COLOR0  0x400B10u
#define PCRTC_START         0x600800u
#define PVIDEO_BUFFER       0x008700u
#define PVIDEO_STOP         0x008704u
#define PCRTC_INTR_0        0x600100u
#define PCRTC_INTR_EN_0     0x600140u
#define USER_PUT            0x800040u
#define USER_GET            0x800044u
#define USER_REF            0x800048u

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

static uint32_t *regs;          /* aperture storage, by byte offset / 4 */
static uint8_t *seen;           /* first-access logging, per register */
static uint32_t pmc_pending_sw; /* PMC bits not derived from a block */
static CRITICAL_SECTION lock;
static CONDITION_VARIABLE kick; /* PUT moved or a trap was acknowledged */
static volatile LONG irq_line;
static volatile LONG active;
static uint64_t methods, traps, flips, overlay_frames;
static int pvideo_queued; /* PVIDEO_BUFFER written since the last vblank */
/* RECOMP_FCI_GPU_STATS=1: every 60 flips, the pusher's time drawing, idle and in FLIP_STALL */
static int gpu_stats = -1;
static LONGLONG st_exec, st_idle, st_stall, st_mark;

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void gpu_stats_flip(void)
{
    LARGE_INTEGER f;
    LONGLONG now = qpc(), span;
    if (gpu_stats < 0)
        gpu_stats = getenv("RECOMP_FCI_GPU_STATS") != NULL;
    if (!gpu_stats || flips % 60)
        return;
    QueryPerformanceFrequency(&f);
    span = st_mark ? now - st_mark : 0;
    if (span)
        fprintf(stderr,
                "  [FCI] GPU 60 flips in %.0f ms (%.1f/s): draw %.0f%% idle %.0f%% flip-stall %.0f%%\n",
                1000.0 * span / f.QuadPart, 60.0 * f.QuadPart / span, 100.0 * st_exec / span,
                100.0 * st_idle / span, 100.0 * st_stall / span);
    st_exec = st_idle = st_stall = 0;
    st_mark = now;
}

static uint32_t reg(uint32_t off) { return regs[off / 4]; }

/* Caller holds lock. */
static void update_irq(void)
{
    uint32_t pmc = pmc_pending_sw & ~(PMC_PFIFO | PMC_PGRAPH | PMC_PCRTC);
    if (reg(PFIFO_INTR_0) & reg(PFIFO_INTR_EN_0))
        pmc |= PMC_PFIFO;
    if (reg(PGRAPH_INTR) & reg(PGRAPH_INTR_EN))
        pmc |= PMC_PGRAPH;
    if (reg(PCRTC_INTR_0) & reg(PCRTC_INTR_EN_0))
        pmc |= PMC_PCRTC;
    regs[PMC_INTR_0 / 4] = pmc;
    InterlockedExchange(&irq_line, (pmc && reg(PMC_INTR_EN_0)) ? 1 : 0);
}

static void nv2a_stop(const char *why, uint32_t a, uint32_t b)
{
    fprintf(stderr, "[FCI] STOP 93 NV2A: %s (%08X %08X) GET=%08X PUT=%08X\n", why, a, b, reg(USER_GET),
            reg(USER_PUT));
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 93);
}

/* ---- MMIO entry points used by nv2a_mmio_hook.c's decoder ---- */

void *nv2a_get_state(void) { return regs; }

/* referenced only by the decoder's nv2a_hook_init, which is never called here */
void *nv2a_init_standalone(void *vram, uint32_t vram_size, void *ramin, uint32_t ramin_size)
{
    (void)vram;
    (void)vram_size;
    (void)ramin;
    (void)ramin_size;
    nv2a_stop("nv2a_init_standalone called", 0, 0);
    return NULL;
}

uint64_t nv2a_mmio_read(void *opaque, uint64_t addr, unsigned size)
{
    uint32_t off = (uint32_t)addr & ~3u, value;
    (void)opaque;
    if (addr >= APERTURE_SIZE)
        nv2a_stop("read outside aperture", (uint32_t)addr, size);
    EnterCriticalSection(&lock);
    switch (off) {
    case PFIFO_RUNOUT_STATUS:
    case PFIFO_CACHE1_STATUS:
        value = reg(off) | 0x10u; /* LOW_MARK: the cache drains synchronously */
        break;
    default:
        value = reg(off);
    }
    if (!seen[off / 4]) {
        seen[off / 4] = 1;
        fprintf(stderr, "  [NV2A] first read  %06X -> %08X\n", off, value);
    }
    LeaveCriticalSection(&lock);
    value >>= ((uint32_t)addr & 3u) * 8;
    return size == 1 ? (value & 0xFFu) : size == 2 ? (value & 0xFFFFu) : value;
}

void nv2a_mmio_write(void *opaque, uint64_t addr, uint64_t val, unsigned size)
{
    uint32_t off = (uint32_t)addr & ~3u, value = (uint32_t)val;
    (void)opaque;
    if (addr >= APERTURE_SIZE || (size != 1 && size != 2 && size != 4) || ((uint32_t)addr & (size - 1)))
        nv2a_stop("unsupported register write shape", (uint32_t)addr, size);
    EnterCriticalSection(&lock);
    if (size != 4) {
        /* byte and word ports (PRMCIO/PRMVIO) merge into their dword; registers with side effects
         * take whole dwords only */
        uint32_t shift = ((uint32_t)addr & 3u) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        if (off == PMC_INTR_0 || off == PFIFO_INTR_0 || off == PGRAPH_INTR || off == PCRTC_INTR_0
            || off == PFB_FLUSH || off == USER_PUT)
            nv2a_stop("sub-dword write to a register with side effects", (uint32_t)addr, size);
        value = (reg(off) & ~mask) | ((value << shift) & mask);
    }
    if (!seen[off / 4] && (off < 0x700000u || off >= 0x800000u)) { /* not PRAMIN */
        seen[off / 4] = 1;
        fprintf(stderr, "  [NV2A] first write %06X <- %08X (size %u)\n", off, value, size);
    }
    if (off >= 0x8700u
        && off < 0x8C00u) { /* PVIDEO changes except BUFFER, and nonzero STOPs, under RECOMP_FCI_PVIDEO_TRACE */
        static int trace = -1, shown;
        if (trace < 0)
            trace = getenv("RECOMP_FCI_PVIDEO_TRACE") != NULL;
        if (trace && off != PVIDEO_BUFFER && (value != reg(off) || (off == PVIDEO_STOP && value))
            && shown++ < 400)
            fprintf(stderr, "  [NV2A] pvideo %06X <- %08X (overlay frames %llu)\n", off, value,
                    (unsigned long long)overlay_frames);
    }
    switch (off) {
    case PMC_INTR_0:
        pmc_pending_sw &= ~value;
        break;
    case PFIFO_INTR_0:
    case PGRAPH_INTR:
    case PCRTC_INTR_0:
        regs[off / 4] &= ~value; /* write 1 to clear */
        if (off == PGRAPH_INTR && (value & PGRAPH_INTR_ERROR))
            WakeAllConditionVariable(&kick);
        break;
    case PFB_FLUSH:
        regs[off / 4] = value & ~0x00010000u; /* flush completes at once */
        break;
    case PGRAPH_INCREMENT:
        if (value & 2u) { /* READ_3D: the displayed buffer advanced */
            uint32_t s = reg(PGRAPH_SURFACE), modulo = (s & SURFACE_MODULO_3D) >> 28;
            uint32_t read = (s & SURFACE_READ_3D) >> 24;
            read = modulo ? (read + 1) % modulo : 0;
            regs[PGRAPH_SURFACE / 4] = (s & ~SURFACE_READ_3D) | (read << 24);
            WakeAllConditionVariable(&kick);
        }
        regs[off / 4] = value;
        break;
    case USER_PUT:
        regs[off / 4] = value;
        WakeAllConditionVariable(&kick);
        break;
    case PVIDEO_BUFFER:
        regs[off / 4] = value;
        pvideo_queued = 1;
        break;
    default:
        regs[off / 4] = value;
    }
    update_irq();
    LeaveCriticalSection(&lock);
}

/* ---- pusher ---- */

static uint32_t fetch(uint32_t offset) { return MEM32(CONTIG | (offset & 0x0FFFFFFFu)); }

/* Object lookup in RAMHT (in PRAMIN): PFIFO_RAMHT gives its base (bits 4-8, 4 KiB units) and size
 * (bits 16-17: 4 KiB << n). Channel 0 only. */
#define PRAMIN      0x700000u
#define PFIFO_RAMHT 0x002210u

static int ramht_instance(uint32_t handle, uint32_t *instance)
{
    uint32_t cfg = reg(PFIFO_RAMHT), size = 0x1000u << ((cfg >> 16) & 3u);
    uint32_t base = ((cfg >> 4) & 0x1Fu) << 12, bits = 0, hash = 0, h = handle, i;
    while ((1u << (bits + 1)) < size)
        bits++; /* ctz(size) - 1 */
    while (h) {
        hash ^= h & ((1u << bits) - 1u);
        h >>= bits;
    }
    for (i = 0; i < size / 8; i++, hash = (hash + 1) & (size / 8 - 1)) {
        uint32_t e = PRAMIN + base + hash * 8;
        if (reg(e) == handle && (reg(e + 4) & 0x80000000u)) {
            *instance = (reg(e + 4) & 0xFFFFu) << 4;
            return 1;
        }
        if (!reg(e + 4))
            break;
    }
    return 0;
}

/* Physical base of a DMA object named by handle. */
static uint32_t dma_base(uint32_t handle, uint32_t *limit)
{
    uint32_t inst, flags, frame;
    if (!ramht_instance(handle, &inst))
        nv2a_stop("object handle not in RAMHT", handle, reg(PFIFO_RAMHT));
    flags = reg(PRAMIN + inst);
    if ((flags & 0xFFFu) != 0x02u && (flags & 0xFFFu) != 0x03u && (flags & 0xFFFu) != 0x3Du)
        nv2a_stop("object is not a DMA object", handle, flags);
    *limit = reg(PRAMIN + inst + 4);
    frame = reg(PRAMIN + inst + 8);
    return (frame & 0xFFFFF000u) | (flags >> 20);
}

static uint32_t dma_semaphore, semaphore_offset;
static uint32_t subch_class[8]; /* class of the object bound to each subchannel */

/* Method census for discovery: count and last parameter per (subch, method). */
static struct {
    uint32_t key, count, last;
} census[512];

static void count_method(uint32_t subch, uint32_t mthd, uint32_t param)
{
    uint32_t key = (subch << 16) | mthd, h = (key * 2654435761u) >> 23, i;
    for (i = 0; i < 512; i++, h = (h + 1) & 511) {
        if (census[h].count && census[h].key != key)
            continue;
        census[h].key = key;
        census[h].count++;
        census[h].last = param;
        return;
    }
}

/* Runs one method; returns after any trap it raised has been acknowledged. */
static void method(uint32_t subch, uint32_t mthd, uint32_t param)
{
    methods++;
    count_method(subch, mthd, param);
    if (mthd == 0x1D6C || mthd == 0x1D70 || mthd == 0x0000 || (mthd >= 0x0180 && mthd <= 0x01B0)) {
        static unsigned logged;
        if (logged++ < 64)
            fprintf(stderr, "  [NV2A] subch %u method %04X = %08X\n", subch, mthd, param);
    }
    /* Kelvin semaphore release writes its parameter to memory; drawing is synchronous here, so the
     * release follows all earlier work */
    /* Kelvin flip counters in PGRAPH_SURFACE: FLIP_STALL holds the pusher until the vblank ISR
     * advances READ_3D */
    if (subch == 0 && mthd >= 0x0120 && mthd <= 0x0130) {
        uint32_t s, read, write, modulo;
        DWORD waited = 0;
        EnterCriticalSection(&lock);
        s = reg(PGRAPH_SURFACE);
        read = (s & SURFACE_READ_3D) >> 24;
        write = (s & SURFACE_WRITE_3D) >> 20;
        modulo = (s & SURFACE_MODULO_3D) >> 28;
        switch (mthd) {
        case 0x0120:
            read = param & 7u;
            break;
        case 0x0124:
            write = param & 7u;
            break;
        case 0x0128:
            modulo = param & 7u;
            break;
        case 0x012C:
            if (!modulo)
                nv2a_stop("FLIP_INCREMENT_WRITE with modulo 0", s, param);
            write = (write + 1) % modulo;
            flips++;
            gpu_stats_flip();
            break;
        case 0x0130: {
            LONGLONG t0 = qpc();
            while (((reg(PGRAPH_SURFACE) & SURFACE_READ_3D) >> 24)
                   == ((reg(PGRAPH_SURFACE) & SURFACE_WRITE_3D) >> 20)) {
                SleepConditionVariableCS(&kick, &lock, 100);
                if ((waited += 100) >= 10000)
                    nv2a_stop("FLIP_STALL: read never advanced in 10s", reg(PGRAPH_SURFACE), 0);
            }
            st_stall += qpc() - t0;
            LeaveCriticalSection(&lock);
            return;
        }
        default:
            LeaveCriticalSection(&lock);
            nv2a_stop("unmodeled flip method", mthd, param);
        }
        regs[PGRAPH_SURFACE / 4] = (s & ~(SURFACE_READ_3D | SURFACE_WRITE_3D | SURFACE_MODULO_3D))
                                   | (read << 24) | (write << 20) | (modulo << 28);
        LeaveCriticalSection(&lock);
        if (mthd == 0x012C) /* the executor's frame trace keys on flips */
            nv2a_pb_exec_method(subch, mthd, param);
        return;
    }
    if (mthd == 0x0000) { /* SET_OBJECT: the class comes from the object's RAMHT entry */
        uint32_t inst;
        EnterCriticalSection(&lock);
        subch_class[subch & 7u] = ramht_instance(param, &inst) ? reg(PRAMIN + inst) & 0xFFFu : 0;
        LeaveCriticalSection(&lock);
        {
            static unsigned logged;
            if (logged++ < 16)
                fprintf(stderr, "  [NV2A] subch %u bound to object %08X class %03X\n", subch, param,
                        subch_class[subch & 7u]);
        }
    }
    if (subch_class[subch & 7u] == 0x044 && mthd == 0x0310) {
        /* NV04 pattern color 0: D3D's GPU progress fence, which it polls (0050AB90, 0050AC10) */
        EnterCriticalSection(&lock);
        regs[PGRAPH_PATT_COLOR0 / 4] = param;
        LeaveCriticalSection(&lock);
    }
    if (subch == 0 && mthd == 0x01A4) {
        dma_semaphore = param;
    } else if (subch == 0 && mthd == 0x1D6C) {
        semaphore_offset = param;
    } else if (subch == 0 && mthd == 0x1D70) {
        uint32_t limit, base;
        EnterCriticalSection(&lock);
        base = dma_base(dma_semaphore, &limit);
        LeaveCriticalSection(&lock);
        if (semaphore_offset + 3 > limit)
            nv2a_stop("semaphore offset beyond DMA limit", semaphore_offset, limit);
        MEM32(CONTIG | ((base + semaphore_offset) & 0x0FFFFFFFu)) = param;
        {
            static unsigned logged;
            if (logged++ < 4)
                fprintf(stderr, "  [NV2A] semaphore release %08X -> phys %08X\n", param,
                        base + semaphore_offset);
        }
    }
    if (mthd == 0x0100 && param != 0) { /* NO_OPERATION trap (software method) */
        DWORD waited = 0;
        EnterCriticalSection(&lock);
        if (reg(PGRAPH_INTR) & PGRAPH_INTR_ERROR)
            nv2a_stop("NOP trap while an error interrupt is pending", mthd, param);
        regs[PGRAPH_TRAPPED_ADDR / 4] = (mthd & 0x1FFFu) | ((subch & 7u) << 16);
        regs[PGRAPH_TRAPPED_DATA / 4] = param;
        regs[PGRAPH_NSOURCE / 4] = 1u; /* NOTIFICATION */
        regs[PGRAPH_INTR / 4] |= PGRAPH_INTR_ERROR;
        update_irq();
        if (traps++ < 8)
            fprintf(stderr, "  [NV2A] NOP trap subch %u param %08X (irq %ld)\n", subch, param, irq_line);
        while (reg(PGRAPH_INTR) & PGRAPH_INTR_ERROR) {
            SleepConditionVariableCS(&kick, &lock, 100);
            if ((waited += 100) >= 10000)
                nv2a_stop("NOP trap not acknowledged in 10s", param, reg(PGRAPH_INTR_EN));
        }
        LeaveCriticalSection(&lock);
        return;
    }
    {
        LONGLONG t0 = qpc();
        nv2a_pb_exec_method(subch, mthd, param);
        st_exec += qpc() - t0;
    }
}

static DWORD WINAPI pusher(LPVOID unused)
{
    uint32_t ret = 0;
    int in_call = 0;
    (void)unused;
    for (;;) {
        uint32_t get, put, word;
        LONGLONG t0 = qpc();
        EnterCriticalSection(&lock);
        while ((get = reg(USER_GET)) == (put = reg(USER_PUT)))
            SleepConditionVariableCS(&kick, &lock, INFINITE);
        LeaveCriticalSection(&lock);
        st_idle += qpc() - t0;

        word = fetch(get);
        get += 4;
        if ((word & 0xE0000003u) == 0x20000000u) { /* old jump */
            get = word & 0x1FFFFFFCu;
        } else if ((word & 3u) == 1u) { /* new jump */
            get = word & 0xFFFFFFFCu;
        } else if ((word & 3u) == 2u) { /* call */
            if (in_call)
                nv2a_stop("nested pushbuffer call", word, get);
            ret = get;
            in_call = 1;
            get = word & 0xFFFFFFFCu;
        } else if (word == 0x00020000u) { /* return */
            if (!in_call)
                nv2a_stop("pushbuffer return without call", word, get);
            get = ret;
            in_call = 0;
        } else if ((word & 0xE0030003u) == 0 || (word & 0xE0030003u) == 0x40000000u) {
            uint32_t count = (word >> 18) & 0x7FFu, subch = (word >> 13) & 7u;
            uint32_t mthd = word & 0x1FFCu, i;
            int noninc = (word & 0xE0000000u) == 0x40000000u;
            for (i = 0; i < count; i++) {
                uint32_t param = fetch(get);
                get += 4;
                method(subch, noninc ? mthd : mthd + i * 4, param);
            }
        } else {
            nv2a_stop("malformed pushbuffer command", word, get - 4);
        }
        EnterCriticalSection(&lock);
        regs[USER_GET / 4] = get;
        LeaveCriticalSection(&lock);
    }
}

/* ---- lifecycle and interrupt delivery (timer thread) ---- */

/* called by the runtime's memory layout instead of its NV2A thread (patches/runtime) */
void fci_nv2a_start(void *aperture)
{
    DWORD old;
    HANDLE h;
    regs = (uint32_t *)VirtualAlloc(NULL, APERTURE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    seen = (uint8_t *)VirtualAlloc(NULL, APERTURE_SIZE / 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!regs || !seen || !aperture || !VirtualProtect(aperture, APERTURE_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "[FCI] NV2A model setup failed (%lu)\n", GetLastError());
        TerminateProcess(GetCurrentProcess(), 93);
    }
    InitializeCriticalSection(&lock);
    InitializeConditionVariable(&kick);
    h = CreateThread(NULL, 0, pusher, NULL, 0, NULL);
    if (!h)
        TerminateProcess(GetCurrentProcess(), 93);
    CloseHandle(h);
    InterlockedExchange(&active, 1);
    fprintf(stderr, "[FCI] NV2A model active: aperture %08X trapped, pusher started\n", APERTURE);
}

/* PVIDEO overlay (movies): the newest queued buffer is latched at each vblank and composed as YUV 4:2:2,
 * point-sampled, color-keyed with FORMAT DISPLAY */
#define PVIDEO_BASE(i)      (0x008900u + (i) * 4)
#define PVIDEO_OFFSET(i)    (0x008920u + (i) * 4)
#define PVIDEO_SIZE_IN(i)   (0x008928u + (i) * 4)
#define PVIDEO_POINT_OUT(i) (0x008948u + (i) * 4)
#define PVIDEO_SIZE_OUT(i)  (0x008950u + (i) * 4)
#define PVIDEO_FORMAT(i)    (0x008958u + (i) * 4)
#define PVIDEO_COLOR_KEY    0x008B00u

static struct {
    int on;
    uint32_t src, pitch, in_w, in_h, out_x, out_y, out_w, out_h, format, key;
} overlay;

/* Caller holds lock. */
static void latch_overlay(uint32_t buffer_bits)
{
    int i = (buffer_bits & 0x10u) ? 1 : 0;
    overlay.on = 1;
    overlay.src = reg(PVIDEO_BASE(i)) + reg(PVIDEO_OFFSET(i));
    overlay.format = reg(PVIDEO_FORMAT(i));
    overlay.pitch = overlay.format & 0x1FC0u;
    overlay.in_w = reg(PVIDEO_SIZE_IN(i)) & 0x7FFu;
    overlay.in_h = (reg(PVIDEO_SIZE_IN(i)) >> 16) & 0x7FFu;
    overlay.out_x = reg(PVIDEO_POINT_OUT(i)) & 0xFFFu;
    overlay.out_y = (reg(PVIDEO_POINT_OUT(i)) >> 16) & 0xFFFu;
    overlay.out_w = reg(PVIDEO_SIZE_OUT(i)) & 0xFFFu;
    overlay.out_h = (reg(PVIDEO_SIZE_OUT(i)) >> 16) & 0xFFFu;
    overlay.key = reg(PVIDEO_COLOR_KEY);
}

static uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void compose_overlay(uint32_t *row, uint32_t y, uint32_t width)
{
    uint32_t x;
    if (!overlay.on || !overlay.in_w || !overlay.in_h || !overlay.out_w || !overlay.out_h || !overlay.pitch
        || y < overlay.out_y || y >= overlay.out_y + overlay.out_h)
        return;
    {
        uint32_t sy = (y - overlay.out_y) * overlay.in_h / overlay.out_h;
        uint32_t line = overlay.src + sy * overlay.pitch;
        int yuy2 = (overlay.format >> 16) & 1;
        for (x = overlay.out_x; x < width && x < overlay.out_x + overlay.out_w; x++) {
            uint32_t sx = (x - overlay.out_x) * overlay.in_w / overlay.out_w;
            uint32_t pair = MEM32(CONTIG | ((line + (sx & ~1u) * 2) & 0x0FFFFFFFu));
            int Y, U, V, c, d, e;
            if ((overlay.format >> 20) & 1 && (row[x] & 0x00FFFFFFu) != (overlay.key & 0x00FFFFFFu))
                continue;
            if (yuy2) { /* Y0 U Y1 V */
                Y = (sx & 1) ? (pair >> 16) & 0xFF : pair & 0xFF;
                U = (pair >> 8) & 0xFF;
                V = (pair >> 24) & 0xFF;
            } else { /* U Y0 V Y1 */
                Y = (sx & 1) ? (pair >> 24) & 0xFF : (pair >> 8) & 0xFF;
                U = pair & 0xFF;
                V = (pair >> 16) & 0xFF;
            }
            c = Y - 16;
            d = U - 128;
            e = V - 128;
            row[x] = 0xFF000000u | ((uint32_t)clamp255((298 * c + 409 * e + 128) >> 8) << 16)
                     | ((uint32_t)clamp255((298 * c - 100 * d - 208 * e + 128) >> 8) << 8)
                     | clamp255((298 * c + 516 * d + 128) >> 8);
        }
    }
}

/* the host renderer (src/xbr) composes the latched overlay into the frame it presents the same way
 * (d3d8.cpp D3DDevice_Swap); frame: 32bpp rows */
int fci_nv2a_overlay_on(void) { return active && overlay.on; }

void fci_nv2a_overlay_compose(uint32_t *frame, uint32_t height, uint32_t pitch_px)
{
    uint32_t y;
    EnterCriticalSection(&lock);
    for (y = 0; y < height; y++)
        compose_overlay(frame + (size_t)y * pitch_px, y, pitch_px < 640 ? pitch_px : 640);
    LeaveCriticalSection(&lock);
}

/* RECOMP_FCI_FRAMES=<path prefix>: the scanned-out buffer as <prefix>NNNN.bmp every 30th vblank
 * (RECOMP_FCI_FRAMES_EVERY), at most 200 files */
static void capture_frame(uint32_t vblanks)
{
    static const char *prefix;
    static int checked;
    static unsigned written, every = 30;
    static int latest;
    uint32_t start = reg(PCRTC_START), y, sum = 0, nonzero = 0;
    char path[MAX_PATH];
    FILE *f;
    if (!checked) {
        const char *e = getenv("RECOMP_FCI_FRAMES_EVERY"); /* vblanks between captures */
        prefix = getenv("RECOMP_FCI_FRAMES");
        latest = getenv("RECOMP_FCI_FRAMES_LATEST") != NULL; /* one file, overwritten, no cap */
        if (e && atoi(e) > 0)
            every = (unsigned)atoi(e);
        checked = 1;
    }
    if (!prefix || (!latest && written >= 200) || vblanks % every || !start)
        return;
    if (latest)
        snprintf(path, sizeof path, "%slatest.bmp", prefix);
    else
        snprintf(path, sizeof path, "%s%04u.bmp", prefix, written);
    if (!(f = fopen(path, "wb")))
        return;
    {
        uint8_t h[54] = {'B', 'M'};
        uint32_t size = 54 + 640 * 480 * 4;
        int32_t height = -480;
        memcpy(h + 2, &size, 4);
        h[10] = 54;
        h[14] = 40;
        *(int32_t *)(h + 18) = 640;
        memcpy(h + 22, &height, 4);
        h[26] = 1;
        h[28] = 32;
        fwrite(h, 1, 54, f);
    }
    for (y = 0; y < 480; y++) {
        uint32_t row[640], x;
        for (x = 0; x < 640; x++) {
            row[x] = MEM32(CONTIG | ((start + y * 2560 + x * 4) & 0x0FFFFFFFu));
            sum = sum * 31u + row[x];
            if (row[x] & 0x00FFFFFFu)
                nonzero++;
        }
        compose_overlay(row, y, 640);
        fwrite(row, 4, 640, f);
    }
    fclose(f);
    fprintf(stderr,
            "  [FCI] frame %s: scanout %08X sum %08X nonzero %u/307200 methods %llu flips %llu overlay "
            "%llu\n",
            path, start, sum, nonzero, (unsigned long long)methods, (unsigned long long)flips,
            (unsigned long long)overlay_frames);
    written++;
}

/* RECOMP_FCI_WINDOW=1: the software scanout with the overlay in a plain GDI window, 2x */
static uint32_t window_frame[640 * 480];
static HWND window;

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        BITMAPINFO bi;
        RECT rc;
        HDC dc = BeginPaint(w, &ps);
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 640;
        bi.bmiHeader.biHeight = -480;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        GetClientRect(w, &rc);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, 640, 480, window_frame, &bi, DIB_RGB_COLORS,
                      SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP) { /* keyboard pad (RECOMP_KEYBOARD=1) */
        extern void fci_framebuffer_key(int vk, int down);
        fci_framebuffer_key((int)wp, msg == WM_KEYDOWN);
        return 0;
    }
    if (msg == WM_KILLFOCUS) { /* a key held while focus leaves stays held otherwise */
        extern void fci_framebuffer_key(int vk, int down);
        int vk;
        for (vk = 0; vk < 256; vk++)
            fci_framebuffer_key(vk, 0);
    }
    if (msg == WM_CLOSE) {
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID unused)
{
    WNDCLASSA wc;
    RECT r = {0, 0, 1280, 960};
    MSG m;
    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "FciWhole";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowA("FciWhole", FCI_TITLE_NAME " (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                           wc.hInstance, NULL);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

static void window_present(void)
{
    static int state = -1;
    uint32_t start, y, x;
    if (state < 0) {
        const char *e = getenv("RECOMP_FCI_WINDOW");
        state = e && *e == '1';
        if (state)
            CloseHandle(CreateThread(NULL, 0, window_thread, NULL, 0, NULL));
    }
    if (!state || !window)
        return;
    start = reg(PCRTC_START);
    for (y = 0; y < 480; y++) {
        uint32_t *row = window_frame + y * 640;
        for (x = 0; x < 640; x++)
            row[x] = start ? MEM32(CONTIG | ((start + y * 2560 + x * 4) & 0x0FFFFFFFu)) : 0;
        compose_overlay(row, y, 640);
    }
    InvalidateRect(window, NULL, FALSE);
}

/* Timer thread, every tick: raise vblank at 60 Hz on the performance counter and report whether the
 * interrupt line is asserted. More than 4 periods behind (a debugger stop), it resyncs. */
int fci_nv2a_tick(void)
{
    static LONGLONG next_vblank, period;
    LONGLONG now;
    if (!active)
        return 0;
    if (!period) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        period = f.QuadPart / 60;
    }
    now = qpc();
    if (now >= next_vblank) {
        static uint32_t vblanks;
        next_vblank = now - next_vblank > 4 * period ? now + period : next_vblank + period;
        EnterCriticalSection(&lock);
        regs[PCRTC_INTR_0 / 4] |= 1u;
        /* STOP bit 0 ends the overlay and clears the USE bits, which D3D's overlay disable waits for
         * (005131E0); otherwise a queued buffer is latched */
        if (reg(PVIDEO_STOP) & 1u) {
            regs[PVIDEO_STOP / 4] = 0;
            regs[PVIDEO_BUFFER / 4] = 0;
            overlay.on = 0;
        } else if (pvideo_queued && reg(PVIDEO_BUFFER)) {
            overlay_frames++;
            latch_overlay(reg(PVIDEO_BUFFER));
        }
        pvideo_queued = 0;
        update_irq();
        LeaveCriticalSection(&lock);
        capture_frame(++vblanks);
        window_present();
    }
    return irq_line != 0;
}

void fci_nv2a_report(FILE *out)
{
    if (!active)
        return;
    EnterCriticalSection(&lock);
    fprintf(out,
            "  NV2A model: GET=%08X PUT=%08X methods=%llu traps=%llu PMC_INTR=%08X EN=%08X "
            "PGRAPH_INTR=%08X EN=%08X PCRTC_INTR=%08X EN=%08X line=%ld\n",
            reg(USER_GET), reg(USER_PUT), (unsigned long long)methods, (unsigned long long)traps,
            reg(PMC_INTR_0), reg(PMC_INTR_EN_0), reg(PGRAPH_INTR), reg(PGRAPH_INTR_EN), reg(PCRTC_INTR_0),
            reg(PCRTC_INTR_EN_0), irq_line);
    LeaveCriticalSection(&lock);
    {
        int i;
        fprintf(out, "  NV2A methods (subch:method xcount last):");
        for (i = 0; i < 512; i++)
            if (census[i].count)
                fprintf(out, " %u:%04X x%u %08X;", census[i].key >> 16, census[i].key & 0xFFFFu,
                        census[i].count, census[i].last);
        fprintf(out, "\n");
    }
}
