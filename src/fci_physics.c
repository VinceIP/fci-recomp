/* The physics world at the Xbox's 30 steps a second, whatever the frame rate (re/notes/FRAME-RATE.md).
 * The game steps the world once a frame by the frame time (Instincts 003741E0, Evolution 00282B1C, Far Cry's
 * CPhysicalWorld::TimeStep); vehicles, the AI's drives and the player all behave a little differently with
 * shorter steps. The gate adds the frame times up and steps the world only when a whole 1/30 s is due, by the
 * whole steps due: a slow frame takes one longer step, as on the Xbox below 30. Experimental:
 * RECOMP_FCI_PHYS_HZ=30 turns it on (without interpolated drawing, moving things update 30 times a second). */
#include <stdint.h>
#include <stdlib.h>

#include "recomp_types.h"

#define MAX_STEPS 3 /* the timer cuts a frame to 0.1 s; the world refuses a step over 0.2 s */

/* the frame time at the step's argument dt (a float on the guest stack); returns whether to skip the step */
static int fixed_step(uint32_t dt)
{
    static double period = -1.0, due;
    float t = MEMF(dt);
    int n;
    if (period < 0.0) {
        const char *hz = getenv("RECOMP_FCI_PHYS_HZ");
        period = hz && atof(hz) > 0.0 ? 1.0 / atof(hz) : 0.0; /* off by default until drawing interpolates */
    }
    if (!period || !(t > 0.0f)) /* off, or paused: as the game does */
        return 0;
    due += t;
    n = (int)((due + 0.002) / period); /* 2 ms early counts: paced at 29.97 a frame is a step */
    if (n < 1)
        return 1;
    if (n > MAX_STEPS)
        n = MAX_STEPS, due = n * period;
    due -= n * period;
    t = (float)(n * period);
    MEMF(dt) = t;
    return 0;
}

/* Instincts: (world, dt), ret 8 */
int fci_gate_003741E0(void)
{
    if (!fixed_step(g_esp + 8))
        return 0;
    g_esp += 12;
    return 1;
}

/* Evolution: edi = world, (dt), ret 4 */
int fci_gate_00282B1C(void)
{
    if (!fixed_step(g_esp + 4))
        return 0;
    g_esp += 8;
    return 1;
}
