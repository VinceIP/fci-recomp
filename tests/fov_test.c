/* Unit test for display-fov (Q71): the perspective-builder prehook in fci_fov.c.
 *
 * Compiles the shipping source (src/fci_fov.c) against a minimal mock of recomp_types.h
 * (tests/fov_mock/recomp_types.h) and a driver that supplies the guest stack. The driver
 * sets RECOMP_FCI_FOV (the target, vertical degrees), places the fovy at g_esp + 8
 * (the second cdecl parameter of sub_005514C9), and checks the fovy after the prehook runs.
 *
 * The hook scales the fovy by a fixed ratio (target / 67.5, both vertical degrees; 67.5 is
 * the game's on-foot vfov, fci_enhance.cpp:450). The fovy is a stack parameter, not a
 * persistent field, so there is no compounding: the game re-reads its field each frame
 * and passes the same fovy, which the hook scales again.
 *
 * The vectors (one process run each, because the prehook caches the target on first use):
 *   unset      RECOMP_FCI_FOV absent: the fovy is left at 1.1781 (the game's own)
 *   zero       RECOMP_FCI_FOV=0: the fovy is left at 1.1781 (the game's own)
 *   target     RECOMP_FCI_FOV=60: the fovy is 1.1781*(60/67.5)=1.047; stable across 3
 *              calls; a zoom to 2.0 is scaled: 2.0*(60/67.5)=1.778; reset to 1.1781
 *              scales back to 1.047; an aspect of 1.0 (a light's frustum) is left alone
 *   clamp-low  RECOMP_FCI_FOV=20: the fovy is 1.1781*(40/67.5)=0.698 (clamped to 40)
 *   clamp-high RECOMP_FCI_FOV=200: the fovy is 1.1781*(110/67.5)=1.911 (clamped to 110)
 *
 *   pytest tests/test_fov_c.py
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

void fci_prehook_005514C9(void);

#define STACK_VA    0x00400000u
#define FOV_SLOT    8       /* g_esp + 8 = param2 (fovy) */
#define INITIAL     1.1781f /* the game's on-foot vfov in radians (~67.5 deg) */
#define DEFAULT_DEG 67.5f

static char guest[0x00700000];
ptrdiff_t g_xbox_mem_offset = (ptrdiff_t)guest;
uint32_t g_esp = STACK_VA;

static float fovy(void) { return *(float *)(guest + STACK_VA + FOV_SLOT); }
static void set_fovy(float v) { *(float *)(guest + STACK_VA + FOV_SLOT) = v; }

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "unset";
    const char *expect = getenv("FCI_TEST_EXPECT");

    set_fovy(INITIAL);

    if (strcmp(name, "target") == 0) {
        /* simulate 3 frames: each frame the game re-reads its field (set_fovy) and passes
         * the fovy to the hook, which scales it. The field is unchanged by the hook, so
         * the same fovy is passed each frame and the result is stable. */
        float want = INITIAL * 60.0f / DEFAULT_DEG;
        for (int i = 0; i < 3; i++) {
            set_fovy(INITIAL); /* the game re-reads its field */
            fci_prehook_005514C9();
            float f = fovy();
            if (fabsf(f - want) > 1e-4f) {
                printf("FAIL %s: frame %d fovy=%g (want %g)\n", name, i, f, want);
                return 1;
            }
        }
        /* now the game zooms: the field changes to 2.0 and the hook scales it */
        set_fovy(2.0f);
        fci_prehook_005514C9();
        float want2 = 2.0f * 60.0f / DEFAULT_DEG;
        float f2 = fovy();
        if (fabsf(f2 - want2) > 1e-4f) {
            printf("FAIL %s: zoom fovy=%g (want %g)\n", name, f2, want2);
            return 1;
        }
        /* the game resets the field; the hook scales the original */
        set_fovy(INITIAL);
        fci_prehook_005514C9();
        float f3 = fovy();
        if (fabsf(f3 - want) > 1e-4f) {
            printf("FAIL %s: reset fovy=%g (want %g)\n", name, f3, want);
            return 1;
        }
        /* a square projection (aspect 1.0 at g_esp + 12: a light or shadow frustum) is left alone */
        set_fovy(INITIAL);
        *(float *)(guest + STACK_VA + 12) = 1.0f;
        fci_prehook_005514C9();
        if (fabsf(fovy() - INITIAL) > 1e-6f) {
            printf("FAIL %s: square aspect fovy=%g (want %g)\n", name, fovy(), INITIAL);
            return 1;
        }
        *(float *)(guest + STACK_VA + 12) = 0.0f;
        printf("ok %s: fovy=%g stable x3, %g after zoom, %g after reset\n", name, f2, f3, want);
        return 0;
    }

    fci_prehook_005514C9();
    float f = fovy();
    float want = expect ? (float)atof(expect) : INITIAL;
    int ok = fabsf(f - want) < 1e-4f;
    printf("%s %s: fovy=%g (want %g)\n", ok ? "ok" : "FAIL", name, f, want);
    return ok ? 0 : 1;
}
