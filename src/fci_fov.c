/* display-fov: scales the vertical FOV every camera passes to the perspective builder (re/notes/DISPLAY.md,
 * "Field of view"). */
#include <stdlib.h>

#include "recomp_types.h"

/* the game's on-foot vertical FOV in degrees (fci_enhance.cpp:450: tan_y = 0.67) */
#define FCI_FOV_DEFAULT_DEG 67.5f

static float fov_target; /* degrees, 0 = the game's own */

/* prehook on the perspective builder (Evolution sub_004048C9 too): its fovy argument is at g_esp + 8 */
void fci_prehook_005514C9(void)
{
    static int ready = 0;
    float target, ratio, fovy;
    if (!ready) {
        const char *e = getenv("RECOMP_FCI_FOV");
        target = e ? (float)atof(e) : 0.0f;
        if (target > 0.0f) {
            if (target < 40.0f)
                target = 40.0f;
            if (target > 110.0f)
                target = 110.0f;
        }
        fov_target = target;
        ready = 1;
    }
    target = fov_target;
    if (target <= 0.0f)
        return;
    fovy = MEMF(g_esp + 8);
    /* a square projection (Instincts 0x00241FC3: aspect 1.0, the FOV from an object) is a light's or
     * shadow's frustum, not a camera: left alone */
    if (fovy <= 0.0f || MEMF(g_esp + 12) == 1.0f)
        return;
    ratio = target / FCI_FOV_DEFAULT_DEG;
    MEMF(g_esp + 8) = fovy * ratio;
}
