/* RECOMP_FCI_VEHTRACE=<file>: a line per call of Evolution's vehicle drive functions (re/notes/FRAME-RATE.md):
 * the frame, the game's clock, which drive, the vehicle, its action bits (+0x54) and the stack arguments.
 * Diagnostic for the jeep's frame-rate dependence; off unless set. RECOMP_FCI_VEH_REPLAY=<such a file> drives
 * the car by the trace's bits instead, by game time, to compare the physics at another frame rate. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"

void fci_status_counts(long *presents, long *polls, long *frames3d, long *movies); /* fci_threads.c */
double fci_game_clock(void);                                                       /* fci_trace.c */

static FILE *vt;
static int checked;

static void line(const char *what, uint32_t vehicle)
{
    long presents, polls, frames3d, movies;
    if (!checked) {
        const char *path = getenv("RECOMP_FCI_VEHTRACE");
        checked = 1;
        if (path && *path)
            vt = fopen(path, "w");
    }
    if (!vt || !vehicle)
        return;
    fci_status_counts(&presents, &polls, &frames3d, &movies);
    fprintf(vt, "%ld %.4f %s %08X %02X %08X %.4f %.4f\n", frames3d, fci_game_clock(), what, vehicle,
            MEM32(vehicle + 0x54) & 0xFF, MEM32(g_esp + 4), MEMF(g_esp + 8), MEMF(vehicle + 0x10));
    fflush(vt);
}

/* RECOMP_FCI_VEH_REPLAY=<a RECOMP_FCI_VEHTRACE file> (experiment): the car's action bits from that trace by
 * game time, so physics can be compared with the same input at another frame rate */
static void replay_bits(uint32_t vehicle)
{
    static double *ts;
    static uint8_t *bits;
    static int n = -1, at;
    double now = fci_game_clock();
    if (n < 0) {
        const char *path = getenv("RECOMP_FCI_VEH_REPLAY");
        FILE *f = path && *path ? fopen(path, "r") : NULL;
        char ln[256];
        int cap = 0;
        n = 0;
        while (f && fgets(ln, sizeof ln, f)) {
            long frame;
            double t;
            char what[8];
            unsigned v, b;
            if (sscanf(ln, "%ld %lf %7s %x %x", &frame, &t, what, &v, &b) != 5 || strcmp(what, "D981"))
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 4096;
                ts = realloc(ts, cap * sizeof *ts);
                bits = realloc(bits, cap);
            }
            ts[n] = t;
            bits[n++] = (uint8_t)b;
        }
        if (f)
            fclose(f);
    }
    if (!n || !vehicle)
        return;
    while (at + 1 < n && ts[at + 1] <= now)
        at++;
    if (ts[at] <= now)
        MEM32(vehicle + 0x54) = (MEM32(vehicle + 0x54) & ~0xFFu) | bits[at];
}

void fci_prehook_0012C35F(void) { line("C35F", g_eax); }
void fci_prehook_0012CA9A(void) { line("CA9A", g_ebx); }
void fci_prehook_0012D22B(void) { line("D22B", g_eax); }
void fci_prehook_0012D981(void)
{
    replay_bits(g_ebx);
    line("D981", g_ebx);
}
