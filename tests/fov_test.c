/* Unit test for display-fov (Q71): the camera-update and perspective-builder prehooks in fci_fov.c.
 *
 * Compiles the shipping source (src/fci_fov.c) against a minimal mock of recomp_types.h
 * (tests/fov_mock/recomp_types.h) and a driver that supplies the guest registers and memory.
 *
 * Instincts scales the camera's FOV field (radians at camera+0x38; the game writes pi/2 each frame,
 * projection fovy = field / ratio) in the camera update's prehook (fci_prehook_002A84E0), only when the
 * return address is one of Camera_BuildViews' calls (the render camera; 0x000F0C55 here): shadow and
 * cube-map cameras keep their FOV. The scale is target/67.5, once: the hook remembers the value it wrote,
 * so a second call does not compound, and a new game value (a zoom) is picked up. A non-positive field,
 * or one of 3.1 or more, is left alone; the result is clamped to 2.97 (170 degrees).
 *
 * The perspective builder (fci_prehook_005514C9) does not scale the fovy on Instincts; it scales the
 * display aspect (g_esp+12 by A/(16/9) when A is not 16:9 or 4:3) and leaves a square projection alone.
 *
 * The vectors (one process run each, because the hook caches the target on first use):
 *   cam-unset        RECOMP_FCI_FOV absent: the field stays pi/2
 *   cam-zero         RECOMP_FCI_FOV=0: the field stays pi/2
 *   cam-target       RECOMP_FCI_FOV=60: pi/2*(60/67.5); stable x3; no compound; zoom 1.0 -> 1.0*(60/67.5);
 *                    reset to pi/2 scales back
 *   cam-clamp-low    RECOMP_FCI_FOV=20: the target is clamped up to 40
 *   cam-clamp-high   RECOMP_FCI_FOV=200: the target is clamped down to 110
 *   cam-clamp-result RECOMP_FCI_FOV=110, field 2.0: 3.26 -> 2.97
 *   cam-nonpos       RECOMP_FCI_FOV=60, field 0: left alone
 *   cam-toolarge     RECOMP_FCI_FOV=60, field 3.1: left alone
 *   cam-other        RECOMP_FCI_FOV=60, called from 0x00457223 (a shadow camera): left alone
 *   aspect           RECOMP_FCI_TEST_ASPECT=1.6: the aspect argument is scaled by A/(16/9); the fovy
 *                    is unchanged; a square projection (1.0) is left alone
 *
 *   pytest tests/test_fov_c.py
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"

void fci_prehook_002A84E0(void);
void fci_prehook_005514C9(void);

#define STACK_VA    0x00400000u
#define CAM_VA      0x00410000u
#define FOV_SLOT    8       /* g_esp + 8 = param2 (fovy) of the builder */
#define ASP_SLOT    12      /* g_esp + 12 = param3 (aspect) of the builder */
#define CAM_FOV     0x38    /* camera + 0x38 = the FOV field, radians */
#define FIELD       1.5707964f /* the value the game writes each frame (pi/2) */
#define RENDER_CALL 0x000F0C55u /* Camera_BuildViews' call of the camera update */

static char guest[0x00700000];
ptrdiff_t g_xbox_mem_offset = (ptrdiff_t)guest;
uint32_t g_esp = STACK_VA;
uint32_t g_ebx = CAM_VA;

/* mock of fci_host_aspect (fci_xbr_glue.cpp): 16:9 by default, RECOMP_FCI_TEST_ASPECT to override */
float fci_host_aspect(void)
{
    const char *e = getenv("RECOMP_FCI_TEST_ASPECT");
    return e ? (float)atof(e) : 16.0f / 9.0f;
}

static float cam_field(void) { return *(float *)(guest + CAM_VA + CAM_FOV); }
static void set_cam_field(float v) { *(float *)(guest + CAM_VA + CAM_FOV) = v; }
static float fovy(void) { return *(float *)(guest + STACK_VA + FOV_SLOT); }
static void set_fovy(float v) { *(float *)(guest + STACK_VA + FOV_SLOT) = v; }
static float aspect(void) { return *(float *)(guest + STACK_VA + ASP_SLOT); }
static void set_aspect(float v) { *(float *)(guest + STACK_VA + ASP_SLOT) = v; }

static int fail(const char *name, const char *what, float got, float want)
{
    printf("FAIL %s: %s=%g (want %g)\n", name, what, got, want);
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "cam-unset";
    const char *expect = getenv("FCI_TEST_EXPECT");
    const char *seed_e = getenv("FCI_TEST_SEED");
    float seed = seed_e ? (float)atof(seed_e) : FIELD;

    MEM32(g_esp) = strcmp(name, "cam-other") ? RENDER_CALL : 0x00457223u; /* the return address */

    if (strcmp(name, "cam-target") == 0) {
        /* the full scenario: stable x3, two calls in a frame do not compound, a zoom is picked up,
         * a reset scales back. RECOMP_FCI_FOV=60, so the scale is 60/67.5. */
        float s = 60.0f / 67.5f;
        float want = FIELD * s;
        for (int i = 0; i < 3; i++) {
            set_cam_field(FIELD); /* the game re-reads its field each frame */
            fci_prehook_002A84E0();
            if (fabsf(cam_field() - want) > 1e-4f)
                return fail(name, "frame", cam_field(), want);
        }
        /* two calls in a frame do not compound: the field is already scaled (=last_written), so the
         * second call leaves it alone */
        fci_prehook_002A84E0();
        if (fabsf(cam_field() - want) > 1e-4f)
            return fail(name, "two-calls", cam_field(), want);
        /* a zoom: the game writes a new field (1.0), the hook scales it */
        set_cam_field(1.0f);
        fci_prehook_002A84E0();
        if (fabsf(cam_field() - 1.0f * s) > 1e-4f)
            return fail(name, "zoom", cam_field(), 1.0f * s);
        /* a reset: the game writes the on-foot field again, the hook scales it back */
        set_cam_field(FIELD);
        fci_prehook_002A84E0();
        if (fabsf(cam_field() - want) > 1e-4f)
            return fail(name, "reset", cam_field(), want);
        printf("ok %s: field=%g stable x3, no compound, %g after zoom, %g after reset\n",
               name, want, 1.0f * s, want);
        return 0;
    }

    if (strcmp(name, "aspect") == 0) {
        /* the display aspect: the aspect argument (g_esp+12) is scaled by A/(16/9) when A is not 16:9
         * and the projection is not square; the fovy is unchanged (Instincts); a square projection
         * (1.0) is left alone */
        float A = (float)atof(getenv("RECOMP_FCI_TEST_ASPECT") ? getenv("RECOMP_FCI_TEST_ASPECT") : "1");
        set_fovy(1.178f);
        set_aspect(16.0f / 9.0f);
        fci_prehook_005514C9();
        float want_a = (16.0f / 9.0f) * A / (16.0f / 9.0f);
        if (fabsf(aspect() - want_a) > 1e-4f)
            return fail(name, "aspect", aspect(), want_a);
        if (fabsf(fovy() - 1.178f) > 1e-6f)
            return fail(name, "fovy", fovy(), 1.178f);
        set_aspect(1.0f);
        fci_prehook_005514C9();
        if (fabsf(aspect() - 1.0f) > 1e-6f)
            return fail(name, "square", aspect(), 1.0f);
        printf("ok %s: aspect=%g (A=%g), fovy unchanged, square left alone\n", name, want_a, A);
        return 0;
    }

    /* the simple camera-field vectors: seed the field, run the hook once, check the result */
    set_cam_field(seed);
    fci_prehook_002A84E0();
    float got = cam_field();
    float want = expect ? (float)atof(expect) : seed;
    if (fabsf(got - want) > 1e-4f)
        return fail(name, "field", got, want);
    printf("ok %s: field=%g (seed %g)\n", name, got, seed);
    return 0;
}
