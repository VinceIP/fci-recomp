/* The menus' background videos at their own frame rate (re/notes/MOVIES.md, re/notes/FRAME-RATE.md). The
 * update (Instincts 0021FFA0, Evolution 00196AD6) asks BinkWait only for videos under 25 frames a second and
 * otherwise shows the next frame every game frame: at the Xbox's 30 that is the video's rate, at 60 twice
 * it. The gate skips an update whose next frame is not due yet by the rate in the file's header.
 * RECOMP_FCI_BINK_PACE=0 turns it off. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp_types.h"

#define VIDEOS 8

/* eax = the video object: +4 the Bink handle (its +0x14 / +0x18 the frame rate and divider), +0x1C / +0x1D
 * paused, +0x1E pausing follows the argument, +0x21 reopen pending; ret 4 */
static int video_gate(void)
{
    static struct {
        uint32_t video;
        double next; /* when the next frame is due, seconds */
    } due[VIDEOS];
    static int enabled = -1;
    static LARGE_INTEGER freq;
    LARGE_INTEGER count;
    uint32_t video = g_eax, bink = video ? MEM32(video + 4) : 0, rate, div;
    double now, period;
    int i;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_FCI_BINK_PACE");
        enabled = !(e && *e == '0');
        QueryPerformanceFrequency(&freq);
    }
    if (!enabled || !bink || MEM8(video + 0x21)
        || (MEM8(video + 0x1E) && MEM8(video + 0x1D) != MEM8(g_esp + 4)))
        return 0;
    rate = MEM32(bink + 0x14);
    div = MEM32(bink + 0x18);
    if (!div || rate / div < 25) /* the game asks BinkWait itself */
        return 0;
    period = (double)div / rate;
    QueryPerformanceCounter(&count);
    now = (double)count.QuadPart / freq.QuadPart;
    for (i = 0; i < VIDEOS && due[i].video && due[i].video != video; i++)
        ;
    if (i == VIDEOS)
        i = 0;
    if (due[i].video != video || now - due[i].next > 4 * period) { /* new, or the menu was away: start now */
        due[i].video = video;
        due[i].next = now + period;
        return 0;
    }
    if (now < due[i].next - period / 4) { /* not due: the frame on screen stays */
        g_esp += 8;                       /* ret 4 */
        return 1;
    }
    due[i].next += period;
    return 0;
}

/* RECOMP_FCI_BINK_LOG=1: each background video's frames a second (its +0xC, the frame number), every 2 s */
static void video_log(void)
{
    static int on = -1;
    static uint32_t bink, frame0;
    static DWORD t0;
    uint32_t b = g_eax ? MEM32(g_eax + 4) : 0;
    DWORD now = GetTickCount();
    if (on < 0) {
        const char *e = getenv("RECOMP_FCI_BINK_LOG");
        on = e && *e == '1';
    }
    if (!on || !b)
        return;
    if (b != bink || MEM32(b + 0xC) < frame0) {
        bink = b;
        frame0 = MEM32(b + 0xC);
        t0 = now;
    } else if (now - t0 >= 2000) {
        fprintf(stderr, "[FCI] bink %08X: %.2f frames/s (header %.2f)\n", b,
                (MEM32(b + 0xC) - frame0) * 1000.0 / (now - t0), (double)MEM32(b + 0x14) / MEM32(b + 0x18));
        frame0 = MEM32(b + 0xC);
        t0 = now;
    }
}

int fci_gate_0021FFA0(void)
{
    video_log();
    return video_gate();
}
