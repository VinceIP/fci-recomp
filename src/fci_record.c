/* Input recording and replay for automated tests (tools/record2script.py, re/notes/FRAME-RATE.md).
 *
 * RECOMP_FCI_RECORD=<file>: every controller report the title reads (when it changes), the direct mouse
 * aim of each game frame and, ten times a second, the game's state: frames, input polls, videos, the
 * game's clock, the player and its position.
 * RECOMP_FCI_PLAYBACK=<file>: from the moment the player first appears (the sync), the reports and the aim of
 * a recording replace the live input, on the game's clock rather than the host's: the same input reaches
 * the game at the same game time at any frame rate. A report recorded while the clock stood still (a
 * loading screen's "press A", whose clock depends on the frame rate) waits instead for the clock to stand
 * still and for its recorded host time after the report before it. Before the sync the input is
 * untouched (menus are the test driver's: tools/game_driver.py). With both options the recording logs
 * what the replay did. */
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

uint32_t fci_game_view(void);                       /* fci_threads.c */
void fci_input_pad_stick(int pad, uint8_t *report); /* fci_input.c: the right stick, when it aims directly */

/* the id of the entity the game view controls (Camera_BuildViews, re/notes/INPUT-CAMERA.md) and the player's
 * own entity's id; the same in both games (re/notes/FRAME-RATE.md) */
#ifndef FCI_VIEW_CONTROLLED
#define FCI_VIEW_CONTROLLED 0x30u
#endif
#ifndef FCI_ACTOR_ENTITY
#define FCI_ACTOR_ENTITY 0x4u
#endif
#ifndef FCI_ENTITY_ID
#define FCI_ENTITY_ID 0xA4u
#endif

#define REPORT 20
#define PADS   4

static SRWLOCK lock = SRWLOCK_INIT;
static FILE *rec;
static LARGE_INTEGER freq, start;
static int mode = -1; /* RECORDING | REPLAYING */
#define RECORDING 1
#define REPLAYING 2

/* replay: the recording's gameplay part, times in game seconds from the sync */
typedef struct {
    double t, wait_ms; /* wait_ms >= 0: recorded while the clock stood still; host time after the previous */
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
static int cursor[PADS] = {-1, -1, -1, -1}; /* each pad's report now */
static double cursor_ms[PADS], clock_ms;    /* host time of each pad's last change and the clock's */
#define STILL_MS 250.0                      /* the clock stands still: no game frame for this long */

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
            e.wait_ms = -1.0;
            if (sscanf(line + 4, "%lf %d %40s %lf", &e.t, &e.pad, hex, &e.wait_ms) < 3
                || strlen(hex) != 2 * REPORT || e.pad < 0 || e.pad >= PADS)
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
    const char *r = getenv("RECOMP_FCI_RECORD"), *p = getenv("RECOMP_FCI_PLAYBACK");
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    mode = 0;
    if (p && *p) {
        load_replay(p);
        mode = REPLAYING;
    }
    if (r && *r && (rec = fopen(r, "w")) != NULL) { /* with a replay too: what the replay did */
        fprintf(rec, "# fci-record 1: pad <host_ms> <game_s> <pad> <report>; aim <host_ms> <game_s> <yaw> "
                     "<pitch>; state <host_ms> <game_s> <presents> <polls> <frames3d> <movies> <player> "
                     "<player id> <controlled id> <x> <y> <z>\n");
        mode |= RECORDING;
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
    uint32_t view = fci_game_view(), entity = player ? MEM32(player + FCI_ACTOR_ENTITY) : 0;
    uint32_t controlled = view ? MEM32(view + FCI_VIEW_CONTROLLED) : 0;
    entity = entity ? MEM32(entity + FCI_ENTITY_ID) : 0;
    fci_status_counts(&presents, &polls, &frames3d, &movies);
    fci_player_position(xyz);
    fprintf(rec, "state %.1f %.4f %ld %ld %ld %ld %08X %08X %08X %.3f %.3f %.3f\n", now, fci_game_clock(),
            presents, polls, frames3d, movies, player, entity, controlled, xyz[0], xyz[1], xyz[2]);
}

/* the player's actor changed (fci_trace.c, once a game frame at most): the first one is the sync */
void fci_record_player(uint32_t player)
{
    int m = active();
    if (player && sync_clock < 0.0) {
        int i;
        sync_clock = fci_game_clock();
        for (i = 0; i < PADS; i++)
            cursor_ms[i] = host_ms();
        if (m & REPLAYING)
            fprintf(stderr, "[FCI] replay: synced at game time %.3f\n", sync_clock);
    }
    if (m & RECORDING) {
        AcquireSRWLockExclusive(&lock);
        fprintf(rec, "player %.1f %.4f %08X\n", host_ms(), fci_game_clock(), player);
        fflush(rec);
        ReleaseSRWLockExclusive(&lock);
    }
}

/* game seconds since the sync, or -1 before it */
static double since_sync(void) { return sync_clock < 0.0 ? -1.0 : fci_game_clock() - sync_clock; }

/* for the status file (fci_threads.c): game seconds of the replay still to come, -1 without one */
double fci_replay_left(void)
{
    double t;
    if (!(active() & REPLAYING))
        return -1.0;
    t = since_sync();
    return t < 0.0 ? replay_end : t >= replay_end ? 0.0 : replay_end - t;
}

/* the recording's report for this pad now */
static void replay_report(int pad, uint8_t *report)
{
    static double clock = -1.0;
    double t = since_sync(), now = host_ms();
    int next;
    if (fci_game_clock() != clock) {
        clock = fci_game_clock();
        clock_ms = now;
    }
    if (t < 0.0)
        return;
    if (t > replay_end) {
        memset(report + 2, 0, REPORT - 2); /* the recording is over: nothing pressed */
        return;
    }
    for (;;) { /* advance this pad's cursor past every report that is due */
        for (next = cursor[pad] + 1; next < npads && pads[next].pad != pad; next++)
            ;
        if (next >= npads)
            break;
        if (pads[next].wait_ms < 0.0 ? pads[next].t > t
                                     : now - clock_ms < STILL_MS || now - cursor_ms[pad] < pads[next].wait_ms)
            break;
        cursor[pad] = next;
        cursor_ms[pad] = now;
    }
    if (cursor[pad] >= 0)
        memcpy(report, pads[cursor[pad]].report, REPORT);
    else
        memset(report + 2, 0, REPORT - 2);
}

/* usb_gamepad_report's finished report (runtime patch 0021) */
void fci_pad_report(int pad, uint8_t *report)
{
    static uint8_t last[PADS][REPORT];
    static double last_state = -1e9;
    int m = active();
    if (pad < 0 || pad >= PADS)
        return;
    if (m & REPLAYING)
        replay_report(pad, report);
    if (m & RECORDING) {
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
    }
    /* after the recording, so a replay drives the stick aim the same way */
    fci_input_pad_stick(pad, report);
}

/* the direct aim of this game frame (fci_aim.c): recorded, or replaced by the recording's over the same span
 * of game time. Returns whether *yaw and *pitch were replaced. */
int fci_record_aim(float *yaw, float *pitch)
{
    int m = active(), replaced = 0;
    double t = since_sync();
    if ((m & REPLAYING) && t >= 0.0) {
        int i;
        *yaw = *pitch = 0.0f;
        for (i = 0; i < naims; i++)
            if (aims[i].t > last_aim && aims[i].t <= t) {
                *yaw += aims[i].yaw;
                *pitch += aims[i].pitch;
            }
        last_aim = t;
        replaced = 1;
    }
    if ((m & RECORDING) && (*yaw != 0.0f || *pitch != 0.0f)) {
        AcquireSRWLockExclusive(&lock);
        fprintf(rec, "aim %.1f %.4f %.5f %.5f\n", host_ms(), fci_game_clock(), *yaw, *pitch);
        fflush(rec);
        ReleaseSRWLockExclusive(&lock);
    }
    return replaced;
}
