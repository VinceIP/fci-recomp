/* Direct mouse aim: a prehook on Player_ProcessInput (00098AE0) adds the mouse's counts, in degrees, to
 * the game view's turn, so a count is the same angle at any frame rate (re/notes/INPUT-CAMERA.md). */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#include "re_symbols.h"
#include "recomp_types.h"

/* the view's ViewInput block; another executable's profile gives its own offset (Evolution: 0x8C8) */
#ifndef FCI_GAMEVIEW_INPUT
#define FCI_GAMEVIEW_INPUT RE_GAME_GAMEVIEW_INPUT
#endif

/* cl_aimingHelper: the console settings object's global and the cvar's slot in it; the cvar's int is at
 * MEM32(cvar + 0x10) (Look_ComputeTurn 0003D450 reads it each frame) */
#ifndef FCI_AIM_SETTINGS
#define FCI_AIM_SETTINGS 0x006B3984u
#endif
#ifndef FCI_AIM_HELPER_SLOT
#define FCI_AIM_HELPER_SLOT 0x9Cu
#endif

uint32_t fci_game_view(void);                 /* fci_threads.c: Camera_BuildViews' view */
int fci_input_on_kbm(void);                   /* fci_input.c: the player uses mouse and keyboard */
int fci_input_mp_assist_kept(void);           /* fci_input.c: RECOMP_MOUSE_AIM_ASSIST=1 */
int fci_net_in_session(void);                 /* fci_net_worker.c: in a System Link match */
int fci_input_aim(float *yaw, float *pitch);  /* fci_input.c: degrees since last frame */
void fci_trace_actor(uint32_t actor);         /* fci_trace.c */
int fci_record_aim(float *yaw, float *pitch); /* fci_record.c */

/* In multiplayer the game's aim assist is off while the player uses mouse and keyboard, and the player's
 * own value is back when they pick up the pad. Each player's game applies its own assist. */
static void aim_assist(void)
{
    static int forced;
    static uint32_t saved;
    uint32_t settings, cvar, value;
    int off;
    if (!forced && !fci_net_in_session())
        return; /* single-player: untouched */
    settings = MEM32(FCI_AIM_SETTINGS);
    cvar = settings ? MEM32(settings + FCI_AIM_HELPER_SLOT) : 0;
    value = cvar ? MEM32(cvar + 0x10) : 0;
    if (!value)
        return;
    off = fci_net_in_session() && !fci_input_mp_assist_kept() && fci_input_on_kbm();
    if (off && MEM32(value)) { /* forcing it now, or the game set it again (a level load) */
        if (!forced)
            fprintf(stderr, "[FCI] input: aim assist off (mouse and keyboard in multiplayer)\n");
        saved = MEM32(value);
        MEM32(value) = 0;
        forced = 1;
    } else if (!off && forced) {
        MEM32(value) = saved;
        forced = 0;
        fprintf(stderr, "[FCI] input: aim assist back to %u (gamepad)\n", saved);
    }
}

void fci_prehook_00098AE0(void)
{
    uint32_t block = MEM32(g_esp + 4), view = fci_game_view();
    float yaw, pitch, now, turn;
    if (!view || block != view + FCI_GAMEVIEW_INPUT)
        return;
    fci_trace_actor(g_ecx); /* RECOMP_FCI_TRACE: the player's actor */
    aim_assist();
    if (!fci_input_aim(&yaw, &pitch))
        yaw = pitch = 0.0f;
    /* recorded, or replaced by a replay (fci_record.c) */
    if (fci_record_aim(&yaw, &pitch) || yaw != 0.0f || pitch != 0.0f) {
        MEMF(block + RE_GAME_VIEWINPUT_TURNYAW) += yaw;
        /* the game wraps the new pitch to +-180 before clamping it (85 degrees on foot): keep a fast
         * flick from wrapping past straight up or down and coming back as the opposite limit */
        now = MEMF(block + RE_GAME_VIEWINPUT_ANGLES);
        turn = MEMF(block + RE_GAME_VIEWINPUT_TURNPITCH) + pitch;
        if (now + turn > 90.0f)
            turn = 90.0f - now;
        if (now + turn < -90.0f)
            turn = -90.0f - now;
        MEMF(block + RE_GAME_VIEWINPUT_TURNPITCH) = turn;
    }
}
