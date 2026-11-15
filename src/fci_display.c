/* Display options for the kernel's EEPROM and the host renderer: RECOMP_FCI_ASPECT the console's
 * widescreen setting (re/notes/DISPLAY.md), RECOMP_FCI_WINDOW=<w>x<h> the window size. The host's
 * effective display aspect A (the configured aspect, or for auto the window's client aspect re-read on
 * resize) is fci_host_aspect in fci_xbr_glue.cpp; the guest is told 16:9 widescreen for any A > 4:3. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fci_display.h"

/* the guest is told 16:9 widescreen for any host aspect above 4:3, 4:3 otherwise: A comes from the
 * window (fci_host_aspect, fci_xbr_glue.cpp), so auto follows the window's client aspect */
int fci_aspect_wide(void)
{
    static int wide = -1;
    if (wide < 0)
        wide = fci_host_aspect() > 4.0f / 3.0f;
    return wide;
}

/* RECOMP_FCI_ASPECT as a float: the configured 4:3/16:9/16:10/21:9/32:9, 0 for auto (the window's
 * client aspect, fci_host_aspect); unset is 4:3, the original, as before the wider aspects */
float fci_display_aspect(void)
{
    static float a = -1.0f;
    if (a < 0.0f) {
        const char *e = getenv("RECOMP_FCI_ASPECT");
        a = 4.0f / 3.0f;
        if (e) {
            if (!strcmp(e, "auto"))
                a = 0.0f;
            else if (!strcmp(e, "4:3"))
                a = 4.0f / 3.0f;
            else if (!strcmp(e, "16:9"))
                a = 16.0f / 9.0f;
            else if (!strcmp(e, "16:10"))
                a = 16.0f / 10.0f;
            else if (!strcmp(e, "21:9"))
                a = 21.0f / 9.0f;
            else if (!strcmp(e, "32:9"))
                a = 32.0f / 9.0f;
        }
    }
    return a;
}

/* the display-aspect ini value normalised to one of 4:3/16:9/16:10/21:9/32:9/auto: the WxH and W/H
 * spellings are folded to W:H first; 1 when the value is one of these, 0 otherwise */
int fci_normalize_aspect(const char *value, char *out, size_t size)
{
    static const char *const names[] = {"4:3", "16:9", "16:10", "21:9", "32:9", "auto"};
    char v[16];
    size_t n, i;
    if (!value || !*value || size == 0)
        return 0;
    n = strlen(value);
    if (n >= sizeof v)
        n = sizeof v - 1;
    for (i = 0; i < n; i++) {
        char c = value[i];
        v[i] = (c == 'x' || c == 'X' || c == '/') ? ':' : c;
    }
    v[n] = 0;
    /* the values before the wider aspects: 0 the original 4:3, 1 16:9 */
    if (!strcmp(v, "0") || !strcmp(v, "1")) {
        snprintf(out, size, "%s", v[0] == '1' ? "16:9" : "4:3");
        return 1;
    }
    if ((v[0] | 0x20) == 'a' && (v[1] | 0x20) == 'u' && (v[2] | 0x20) == 't' && (v[3] | 0x20) == 'o' && !v[4]) {
        snprintf(out, size, "auto");
        return 1;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(v, names[i])) {
            snprintf(out, size, "%s", names[i]);
            return 1;
        }
    return 0;
}

/* the 16:10 HUD anchor for a draw whose pixel-box centre is (cx, cy) in the 640x480 guest space: the
 * nearest vertical edge by the centre's x in thirds, the nearest horizontal edge by y in halves, the
 * vertical centre (240) when the box is on the horizontal middle (within 48 px of y 240: the crosshair,
 * a centred menu) */
void fci_hud_anchor(float cx, float cy, float *ax, float *ay)
{
    *ax = cx < 640.0f / 3.0f ? 0.0f : cx < 2.0f * 640.0f / 3.0f ? 320.0f : 640.0f;
    *ay = fabsf(cy - 240.0f) < 48.0f ? 240.0f : (cy < 240.0f ? 0.0f : 480.0f);
}

/* EEPROM XC_VIDEO as the dashboard stores it: video flags in the high word (0x00010000 widescreen,
 * 0x00400000 60 Hz) */
uint32_t fci_video_flags(void) { return 0x00400000u | (fci_aspect_wide() ? 0x00010000u : 0u); }

/* RECOMP_FCI_WINDOW: 1 with the size, 0 when unset or unreadable */
int fci_window_size(uint32_t *w, uint32_t *h)
{
    const char *e = getenv("RECOMP_FCI_WINDOW");
    char *x;
    unsigned long a, b;
    if (!e || !*e)
        return 0;
    a = strtoul(e, &x, 10);
    if (*x != 'x' && *x != 'X')
        return 0;
    b = strtoul(x + 1, NULL, 10);
    if (a < 320 || b < 240 || a > 7680 || b > 4320)
        return 0;
    *w = (uint32_t)a;
    *h = (uint32_t)b;
    return 1;
}
