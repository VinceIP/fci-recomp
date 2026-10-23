/* RECOMP_FCI_TRACE=<file>: one CSV line per 3D frame for comparing frame rates (re/notes/FRAME-RATE.md):
 * the host's time, the engine's clock and frame time, the player's position and the view's angles. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "re_symbols.h"
#include "recomp_types.h"

#ifndef FCI_GAMEVIEW_INPUT
#define FCI_GAMEVIEW_INPUT RE_GAME_GAMEVIEW_INPUT
#endif
/* the engine's timer (Instincts 00644488; a profile gives another image's, 0: none): +0x28 the game's clock,
 * +0x68 the frame time the game steps by, both seconds as doubles */
#ifndef FCI_GAME_TIMER
#define FCI_GAME_TIMER 0x00644488u
#endif
/* the player's actor (Player_ProcessInput's this) points to its entity at +4; the entity's position */
#ifndef FCI_ACTOR_ENTITY
#define FCI_ACTOR_ENTITY 0x4u
#endif
#ifndef FCI_ENTITY_POSITION
#define FCI_ENTITY_POSITION 0x34Cu
#endif

static FILE *trace;
static LARGE_INTEGER freq, start;
static unsigned long frames;
static volatile uint32_t actor; /* Player_ProcessInput's this for the game view (fci_aim.c) */

void fci_trace_actor(uint32_t a)
{
    extern void fci_record_player(uint32_t player); /* fci_record.c */
    if (a != actor) {
        actor = a;
        fci_record_player(a);
    }
}
uint32_t fci_trace_player(void) { return actor; }
double fci_game_clock(void) { return FCI_GAME_TIMER ? MEMD(FCI_GAME_TIMER + 0x28u) : 0.0; }
int fci_player_position(float xyz[3])
{
    uint32_t entity = actor ? MEM32(actor + FCI_ACTOR_ENTITY) : 0;
    if (!entity)
        return 0;
    xyz[0] = MEMF(entity + FCI_ENTITY_POSITION);
    xyz[1] = MEMF(entity + FCI_ENTITY_POSITION + 4);
    xyz[2] = MEMF(entity + FCI_ENTITY_POSITION + 8);
    return 1;
}

/* from Camera_BuildViews' prehook, once per 3D frame */
void fci_trace_frame(uint32_t view)
{
    static int checked;
    LARGE_INTEGER now;
    uint32_t angles = view + FCI_GAMEVIEW_INPUT + 0x18u, entity = actor ? MEM32(actor + FCI_ACTOR_ENTITY) : 0;
    if (!checked) {
        const char *path = getenv("RECOMP_FCI_TRACE");
        checked = 1;
        if (path && *path && (trace = fopen(path, "w")) != NULL) {
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
            fprintf(trace, "frame,host_ms,game_s,frame_s,x,y,z,pitch,roll,yaw\n");
        }
    }
    if (!trace || !view)
        return;
    QueryPerformanceCounter(&now);
    fprintf(trace, "%lu,%.3f,", frames++,
            (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart);
    if (FCI_GAME_TIMER)
        fprintf(trace, "%.6f,%.6f,", MEMD(FCI_GAME_TIMER + 0x28u), MEMD(FCI_GAME_TIMER + 0x68u));
    else
        fprintf(trace, ",,");
    if (entity)
        fprintf(trace, "%.4f,%.4f,%.4f,", MEMF(entity + FCI_ENTITY_POSITION),
                MEMF(entity + FCI_ENTITY_POSITION + 4), MEMF(entity + FCI_ENTITY_POSITION + 8));
    else
        fprintf(trace, ",,,");
    fprintf(trace, "%.3f,%.3f,%.3f\n", MEMF(angles), MEMF(angles + 4), MEMF(angles + 8));
    fflush(trace); /* test runs end the game with a kill */
}
