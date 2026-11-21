/* display-fov and display-aspect: the vertical FOV and the aspect every camera passes to the perspective
 * builder (re/notes/DISPLAY.md, "Field of view"): the 3D view fills the output at its true aspect. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fci_display.h"
#include "fci_image.h"
#include "recomp_types.h"

/* the game's on-foot vertical FOV in degrees (fci_enhance.cpp:450: tan_y = 0.67) */
#define FCI_FOV_DEFAULT_DEG 67.5f

static float fov_target = -1.0f; /* degrees, 0 = the game's own, -1 = not read yet */

static float fov_scale(void)
{
    if (fov_target < 0.0f) {
        const char *e = getenv("RECOMP_FCI_FOV");
        float target = e ? (float)atof(e) : 0.0f;
        if (target > 0.0f) {
            if (target < 40.0f)
                target = 40.0f;
            if (target > 110.0f)
                target = 110.0f;
        }
        fov_target = target;
    }
    return fov_target > 0.0f ? fov_target / FCI_FOV_DEFAULT_DEG : 1.0f;
}

static int fov_evolution(void) { return strstr(FCI_TITLE_NAME, "Evolution") != NULL; }

/* both titles' games scale the FOV at the camera (fci_prehook_002A84E0); the editors' camera builds are not
 * mapped, so there the builder's argument is still scaled */
static int fov_at_camera(void) { return !_stricmp(FCI_IMAGE_FILENAME, "default.xbe"); }

/* prehook on the camera update (Instincts 002A84E0, ebx the camera; Evolution 001F1DA9, edi): FOV in radians
 * at +0x38, projection fovy = +0x38 / ratio +0x384, frustum extents from both. It updates every camera, shadow
 * and cube faces too, so only the calls from Camera_BuildViews (Instincts 000F0500, Evolution 000B8CF5: the
 * render camera) are scaled. A value this hook wrote is not scaled again. */
void fci_prehook_002A84E0(void)
{
    static uint32_t last_written;
    float s = fov_scale(), fov;
    uint32_t bits, from, cam = fov_evolution() ? g_edi : g_ebx;
    if (s == 1.0f || !cam || !fov_at_camera())
        return;
    from = MEM32(g_esp); /* the return address */
    if (fov_evolution() ? from != 0x000B8F70u && from != 0x000B8F75u && from != 0x000B93BAu
                        : from != 0x000F0796u && from != 0x000F079Bu && from != 0x000F0C55u)
        return;
    bits = MEM32(cam + 0x38);
    if (bits == last_written)
        return;
    memcpy(&fov, &bits, 4);
    if (!(fov > 0.0f && fov < 3.1f))
        return;
    fov *= s;
    if (fov > 2.97f)
        fov = 2.97f;
    memcpy(&last_written, &fov, 4);
    MEM32(cam + 0x38) = last_written;
}

/* prehook on the perspective builder (Evolution sub_004048C9 too): its fovy argument is at g_esp + 8 */
void fci_prehook_005514C9(void)
{
    float fovy, s = fov_scale();
    /* a square projection (Instincts 0x00241FC3: aspect 1.0, the FOV from an object) is a light's or
     * shadow's frustum, not a camera: left alone */
    if (MEMF(g_esp + 12) == 1.0f)
        return;
    if (s != 1.0f && !fov_at_camera()) {
        fovy = MEMF(g_esp + 8);
        if (fovy > 0.0f)
            MEMF(g_esp + 8) = fovy * s;
    }
    /* the display aspect: when the guest is told 16:9 (A > 4:3) and A is not 16:9, the aspect argument
     * (g_esp + 12) is scaled by A / (16/9) so the 3D view fills the output at its true aspect */
    float a = fci_host_aspect();
    if (a > 4.0f / 3.0f + 1e-4f && fabsf(a - 16.0f / 9.0f) >= 1e-4f)
        MEMF(g_esp + 12) = MEMF(g_esp + 12) * a / (16.0f / 9.0f);
}
