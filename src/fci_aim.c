/* Direct mouse aim: a prehook on Player_ProcessInput (00098AE0) adds the mouse's counts, in degrees, to
 * the game view's turn, so a count is the same angle at any frame rate (re/notes/INPUT-CAMERA.md). */
#include <stdint.h>
#include <windows.h>

#include "re_symbols.h"
#include "recomp_types.h"

/* the view's ViewInput block; another executable's profile gives its own offset (Evolution: 0x8C8) */
#ifndef FCI_GAMEVIEW_INPUT
#define FCI_GAMEVIEW_INPUT RE_GAME_GAMEVIEW_INPUT
#endif

uint32_t fci_game_view(void);                /* fci_threads.c: Camera_BuildViews' view */
int fci_input_aim(float *yaw, float *pitch); /* fci_input.c: degrees since last frame */

void fci_prehook_00098AE0(void)
{
    uint32_t block = MEM32(g_esp + 4), view = fci_game_view();
    float yaw, pitch, now, turn;
    if (!view || block != view + FCI_GAMEVIEW_INPUT)
        return;
    if (fci_input_aim(&yaw, &pitch)) {
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
