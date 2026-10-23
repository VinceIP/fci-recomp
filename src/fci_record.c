/* Input recording and replay for automated tests (tools/record2script.py, re/notes/FRAME-RATE.md).
 *
 * RECOMP_FCI_RECORD=<file>: every controller report the title reads (when it changes), the direct mouse
 * aim of each game frame and, ten times a second, the game's state: frames, input polls, videos, the
 * game's clock, the player and its position.
 * RECOMP_FCI_REPLAY=<file>: from the moment the player first appears (the sync), the reports and the aim of
 * a recording replace the live input, on the game's clock rather than the host's: the same input reaches
 * the game at the same game time at any frame rate. Before the sync the input is untouched (menus are the
 * test driver's: tools/game_driver.py). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "recomp_types.h"

void fci_status_counts(long *presents, long *polls, long *frames3d, long *movies); /* fci_threads.c */
double fci_game_clock(void);                                                       /* fci_trace.c */
uint32_t fci_trace_player(void);                                                   /* fci_trace.c */
int fci_player_position(float xyz[3]);                                             /* fci_trace.c */

/* the actor's mounted flags (Instincts player+0x400/+0x401, re/notes/INPUT-CAMERA.md) */
#ifndef FCI_ACTOR_MOUNTED
#define FCI_ACTOR_MOUNTED 0x400u
#endif

#define REPORT 20
#define PADS   4

static SRWLOCK lock = SRWLOCK_INIT;
static FILE *rec;
static LARGE_INTEGER freq, start;
static int mode = -1; /* 0 off, 1 record, 2 replay */

/* replay: the recording's gameplay part, times in game seconds from the sync */
typedef struct {
    double t;
    int pad;
    uint8_t report[REPORT];
} PadEntry;
typedef struct {
    double t;
    float yaw, pitch;
} AimEntry;
static PadEntry *pads;
static AimEntry *aims;
static int npads, naims;
static double sync_clock = -1.0, last_aim = 0.0, replay_end = 1e30;

static double host_ms(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static void load_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int cap_p = 0, cap_a = 0;
    if (!f) {
        fprintf(stderr, "[FCI] replay: cannot read %s\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "pad ", 4)) {
            PadEntry e;
            char hex[2 * REPORT + 1];
            int i;
            if (sscanf(line + 4, "%lf %d %40s", &e.t, &e.pad, hex) != 3 || strlen(hex) != 2 * REPORT)
                continue;
            for (i = 0; i < REPORT; i++) {
                unsigned v;
                sscanf(hex + 2 * i, "%2x", &v);
                e.report[i] = (uint8_t)v;
            }
            if (npads == cap_p)
                pads = realloc(pads, sizeof *pads * (cap_p = cap_p ? cap_p * 2 : 1024));
            pads[npads++] = e;
        } else if (!strncmp(line, "aim ", 4)) {
            AimEntry e;
            if (sscanf(line + 4, "%lf %f %f", &e.t, &e.yaw, &e.pitch) != 3)
                continue;
            if (naims == cap_a)
                aims = realloc(aims, sizeof *aims * (cap_a = cap_a ? cap_a * 2 : 1024));
            aims[naims++] = e;
        } else if (!strncmp(line, "end ", 4)) {
            replay_end = atof(line + 4);
        }
    }
    fclose(f);
    fprintf(stderr, "[FCI] replay: %d reports, %d aim frames, %.1f s from %s\n", npads, naims,
            replay_end < 1e29 ? replay_end : 0.0, path);
}

static void setup(void)
{
    const char *r = getenv("RECOMP_FCI_RECORD"), *p = getenv("RECOMP_FCI_REPLAY");
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    mode = 0;
    if (p && *p) {
        load_replay(p);
        mode = 2;
    } else if (r && *r && (rec = fopen(r, "w")) != NULL) {
        fprintf(rec, "# fci-record 1: pad <host_ms> <game_s> <pad> <report>; aim <host_ms> <game_s> <yaw> "
                     "<pitch>; state <host_ms> <game_s> <presents> <polls> <frames3d> <movies> <player> "
                     "<mounted> <x> <y> <z>\n");
        mode = 1;
    }
}

static int active(void)
{
    if (mode < 0) {
        AcquireSRWLockExclusive(&lock);
        if (mode < 0)
            setup();
        ReleaseSRWLockExclusive(&lock);
    }
    return mode;
}

static void write_state(double now)
{
    long presents, polls, frames3d, movies;
    uint32_t player = fci_trace_player();
    float xyz[3] = {0, 0, 0};
    unsigned mounted
            = player ? MEM8(player + FCI_ACTOR_MOUNTED) | MEM8(player + FCI_ACTOR_MOUNTED + 1) << 8 : 0;
    fci_status_counts(&presents, &polls, &frames3d, &movies);
    fci_player_position(xyz);
    fprintf(rec, "state %.1f %.4f %ld %ld %ld %ld %08X %04X %.3f %.3f %.3f\n", now, fci_game_clock(),
            presents, polls, frames3d, movies, player, mounted, xyz[0], xyz[1], xyz[2]);
}

/* the player's actor changed (fci_trace.c, once a game frame at most): the first one is the sync */
void fci_record_player(uint32_t player)
{
    int m = active();
    if (player && sync_clock < 0.0) {
        sync_clock = fci_game_clock();
        if (m == 2)
            fprintf(stderr, "[FCI] replay: synced at game time %.3f\n", sync_clock);
    }
    if (m == 1) {
        AcquireSRWLockExclusive(&lock);
        fprintf(rec, "player %.1f %.4f %08X\n", host_ms(), fci_game_clock(), player);
        fflush(rec);
        ReleaseSRWLockExclusive(&lock);
    }
}

/* game seconds since the sync, or -1 before it */
static double since_sync(void) { return sync_clock < 0.0 ? -1.0 : fci_game_clock() - sync_clock; }

/* usb_gamepad_report's finished report (runtime patch 0021) */
void fci_pad_report(int pad, uint8_t *report)
{
    static uint8_t last[PADS][REPORT];
    static double last_state = -1e9;
    int m = active();
    if (m == 1 && pad >= 0 && pad < PADS) {
        double now = host_ms();
        AcquireSRWLockExclusive(&lock);
        if (memcmp(last[pad], report, REPORT)) {
            int i;
            memcpy(last[pad], report, REPORT);
            fprintf(rec, "pad %.1f %.4f %d ", now, fci_game_clock(), pad);
            for (i = 0; i < REPORT; i++)
                fprintf(rec, "%02X", report[i]);
            fputc('\n', rec);
        }
        if (now - last_state >= 100.0) {
            last_state = now;
            write_state(now);
        }
        fflush(rec);
        ReleaseSRWLockExclusive(&lock);
    } else if (m == 2) {
        double t = since_sync();
        int i, found = -1;
        if (t < 0.0)
            return;
        if (t > replay_end) {
            memset(report + 2, 0, REPORT - 2); /* the recording is over: nothing pressed */
            return;
        }
        for (i = 0; i < npads && pads[i].t <= t; i++)
            if (pads[i].pad == pad)
                found = i;
        if (found >= 0)
            memcpy(report, pads[found].report, REPORT);
        else
            memset(report + 2, 0, REPORT - 2);
    }
}

/* the direct aim of this game frame (fci_aim.c): recorded, or replaced by the recording's over the same span
 * of game time. Returns whether *yaw and *pitch were replaced. */
int fci_record_aim(float *yaw, float *pitch)
{
    int m = active();
    if (m == 1) {
        if (*yaw != 0.0f || *pitch != 0.0f) {
            AcquireSRWLockExclusive(&lock);
            fprintf(rec, "aim %.1f %.4f %.5f %.5f\n", host_ms(), fci_game_clock(), *yaw, *pitch);
            fflush(rec);
            ReleaseSRWLockExclusive(&lock);
        }
        return 0;
    }
    if (m == 2) {
        double t = since_sync();
        int i;
        if (t < 0.0)
            return 0;
        *yaw = *pitch = 0.0f;
        for (i = 0; i < naims; i++)
            if (aims[i].t > last_aim && aims[i].t <= t) {
                *yaw += aims[i].yaw;
                *pitch += aims[i].pitch;
            }
        last_aim = t;
        return 1;
    }
    return 0;
}
