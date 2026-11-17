/* Display options for the kernel's EEPROM and the host renderer (fci_display.c): the display-aspect
 * value, the window size, and the host's effective display aspect A (fci_xbr_glue.cpp, extern "C"). */
#ifndef FCI_DISPLAY_H
#define FCI_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* the guest is told 16:9 widescreen for any host aspect above 4:3, 4:3 otherwise (fci_display.c) */
int fci_aspect_wide(void);
/* RECOMP_FCI_ASPECT as a float: 4:3/16:9/16:10/21:9/32:9, 0 for auto (fci_display.c) */
float fci_display_aspect(void);
/* the display-aspect ini value normalised to one of 4:3/16:9/16:10/21:9/32:9/auto, 1 when it is one
 * of these (the WxH and W/H spellings too), 0 otherwise (fci_display.c) */
int fci_normalize_aspect(const char *value, char *out, size_t size);
/* the HUD remap at 16:10: the 16:9 layout fitted to the width, y scaled about the middle (fci_display.c) */
void fci_hud_fit(float a, float *sx, float *sy, float *ax, float *ay);
/* the host's effective display aspect A: the configured aspect, or for auto the window's client aspect
 * re-read on resize (>= 4:3). Defined in fci_xbr_glue.cpp (extern "C") */
float fci_host_aspect(void);
/* EEPROM XC_VIDEO as the dashboard stores it: video flags in the high word (fci_display.c) */
uint32_t fci_video_flags(void);
/* RECOMP_FCI_WINDOW: 1 with the size, 0 when unset or unreadable (fci_display.c) */
int fci_window_size(uint32_t *w, uint32_t *h);

#endif
