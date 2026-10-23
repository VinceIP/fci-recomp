/* The AI's driving at frame rates above 30 (re/notes/FRAME-RATE.md). Between waypoints the AI driver has no
 * movement target for two frames and pulls the handbrake (action 0x22) instead of driving: 67 ms at the
 * Xbox's 30 frames a second, half that at 60. The vehicle then loses less speed and the scripted drives run
 * ahead. From the first frame of such a pulse, the AI's command carries only the handbrake for two 30 fps
 * frames' time; at 30 this changes nothing. RECOMP_FCI_VEH_HANDBRAKE=0 turns it off. */
#include <stdint.h>
#include <stdlib.h>

#include "recomp_types.h"

double fci_game_clock(void); /* fci_trace.c */

#define HANDBRAKE 0x22 /* the actions the movement handlers read (Instincts 0019E5A0, Evolution 0012C298) */
static const int MOVES[] = {1, 2, 3, 4, 0xA, 0xD, 0xE};
#define PULSE (2.0 / 30.0)

static uint32_t ai_vehicle; /* the vehicle the AI driver is commanding, and when */
static double ai_clock = -1.0;

/* a command's actions: a bit array from +4, action n at bit (n - 1) & 31 of word n >> 5 */
static int has(uint32_t cmd, int n) { return MEM32(cmd + 4 + (n >> 5) * 4) >> ((n - 1) & 31) & 1; }
static void set(uint32_t cmd, int n, int on)
{
    uint32_t word = cmd + 4 + (n >> 5) * 4, bit = 1u << ((n - 1) & 31);
    MEM32(word) = on ? MEM32(word) | bit : MEM32(word) & ~bit;
}

static void ai_driver(uint32_t ai)
{
    ai_vehicle = ai ? MEM32(ai + 0x9C) : 0;
    ai_clock = fci_game_clock();
}

static void movement(uint32_t vehicle, uint32_t cmd)
{
    static struct {
        uint32_t vehicle;
        double start; /* the pulse's first frame, -1 when none is held */
        int was_on;
    } held[16];
    static int enabled = -1;
    double now = fci_game_clock();
    int i, on;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_FCI_VEH_HANDBRAKE");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !vehicle || !cmd || vehicle != ai_vehicle || now != ai_clock)
        return;
    for (i = 0; i < 16 && held[i].vehicle && held[i].vehicle != vehicle; i++)
        ;
    if (i == 16)
        return;
    if (!held[i].vehicle) {
        held[i].vehicle = vehicle;
        held[i].start = -1.0;
    }
    if (held[i].start >= 0.0 && now - held[i].start < PULSE - 0.001) {
        for (unsigned k = 0; k < sizeof MOVES / sizeof MOVES[0]; k++)
            set(cmd, MOVES[k], 0);
        set(cmd, HANDBRAKE, 1);
        held[i].was_on = 1;
        return;
    }
    on = has(cmd, HANDBRAKE);
    held[i].start = on && !held[i].was_on ? now : -1.0;
    held[i].was_on = on;
}

/* Instincts: the AI driver (esi = the AI's vehicle controller), the movement handler (ecx = vehicle, eax =
 * command) */
void fci_prehook_000A38F0(void) { ai_driver(g_esi); }
void fci_prehook_0019E5A0(void) { movement(g_ecx, g_eax); }
/* Evolution: the AI driver (eax), the movement handler (eax = vehicle, esi = command) */
void fci_prehook_0007F99B(void) { ai_driver(g_eax); }
void fci_prehook_0012C298(void) { movement(g_eax, g_esi); }
