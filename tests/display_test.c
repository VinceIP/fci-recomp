// the display options (fci_display.c): the aspect normalisation and the HUD anchor, tested in-process
// (they take the value as an argument, no cache); the float aspect and the widescreen flag are cached per
// process, so the wrapper runs one process per vector. fci_host_aspect is mocked (the glue's, extern "C").
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fci_display.h"

// mock of fci_host_aspect (fci_xbr_glue.cpp): 16:9 by default, RECOMP_FCI_TEST_ASPECT to override
float fci_host_aspect(void)
{
    const char *e = getenv("RECOMP_FCI_TEST_ASPECT");
    return e ? (float)atof(e) : 16.0f / 9.0f;
}

static int fails;
static void expect_eq(float got, float want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %.6f want %.6f\n", what, got, want);
        fails++;
    }
}
static void expect_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want)) {
        fprintf(stderr, "FAIL %s: got %s want %s\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    char out[16];
    // fci_normalize_aspect: the W:H, WxH, W/H spellings and auto are recognised, the rest are not
    struct {
        const char *in, *out;
    } cases[] = {
            {"4:3", "4:3"},   {"4x3", "4:3"},   {"4/3", "4:3"},
            {"16:9", "16:9"}, {"16x9", "16:9"}, {"16/9", "16:9"},
            {"16:10", "16:10"}, {"16x10", "16:10"}, {"16/10", "16:10"},
            {"21:9", "21:9"},  {"21x9", "21:9"},  {"21/9", "21:9"},
            {"32:9", "32:9"},  {"32x9", "32:9"},  {"32/9", "32:9"},
            {"auto", "auto"}, {"AUTO", "auto"}, {"0", "4:3"}, {"1", "16:9"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (!fci_normalize_aspect(cases[i].in, out, sizeof out)) {
            fprintf(stderr, "FAIL normalise %s: not recognised\n", cases[i].in);
            fails++;
        } else {
            expect_str(out, cases[i].out, cases[i].in);
        }
    }
    const char *unknown[] = {"", "1:1", "25:9", "2", "wide", "16:8"};
    for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++)
        if (fci_normalize_aspect(unknown[i], out, sizeof out)) {
            fprintf(stderr, "FAIL normalise %s: should not be recognised\n", unknown[i]);
            fails++;
        }

    // fci_hud_anchor: the box centre's nearest edge (x in thirds, y in halves), the centre when around the
    // middle (within 64 px of x 320 and 48 px of y 240)
    struct {
        float cx, cy, ax, ay;
    } anchors[] = {
            {320, 240, 320, 240},  // the middle: centred (the crosshair)
            {300, 220, 320, 240},  // around the middle: centred
            {320, 240 - 49, 320, 0},   // just above the middle band: top
            {320, 240 + 49, 320, 480}, // just below the middle band: bottom
            {320, 50, 320, 0},         // top centre: top
            {320, 430, 320, 480},      // bottom centre: bottom
            {50, 50, 0, 0},            // top-left corner: top-left
            {590, 50, 640, 0},         // top-right corner: top-right
            {50, 430, 0, 480},         // bottom-left corner: bottom-left
            {590, 430, 640, 480},      // bottom-right corner: bottom-right
            {100, 240, 0, 240},        // left third, middle band: left edge, vertical centre
            {540, 240, 640, 240},      // right third, middle band: right edge, vertical centre
            {320, 100, 320, 0},        // middle x, top half: top edge, horizontal centre
            {320, 380, 320, 480},      // middle x, bottom half: bottom edge, horizontal centre
    };
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; i++) {
        float ax, ay;
        fci_hud_anchor(anchors[i].cx, anchors[i].cy, &ax, &ay);
        expect_eq(ax, anchors[i].ax, "anchor x");
        expect_eq(ay, anchors[i].ay, "anchor y");
    }

    // fci_display_aspect and fci_aspect_wide are cached per process: the wrapper runs one process per
    // vector (RECOMP_FCI_ASPECT the ini value, RECOMP_FCI_TEST_ASPECT the host aspect for the wide flag)
    const char *a = getenv("RECOMP_FCI_ASPECT");
    float want_a = 4.0f / 3.0f;  // unset: the original 4:3
    if (a) {
        if (!strcmp(a, "auto"))
            want_a = 0.0f;
        else if (!strcmp(a, "4:3"))
            want_a = 4.0f / 3.0f;
        else if (!strcmp(a, "16:9"))
            want_a = 16.0f / 9.0f;
        else if (!strcmp(a, "16:10"))
            want_a = 16.0f / 10.0f;
        else if (!strcmp(a, "21:9"))
            want_a = 21.0f / 9.0f;
        else if (!strcmp(a, "32:9"))
            want_a = 32.0f / 9.0f;
    }
    expect_eq(fci_display_aspect(), want_a, "fci_display_aspect");

    const char *ha = getenv("RECOMP_FCI_TEST_ASPECT");
    float host_a = ha ? (float)atof(ha) : 16.0f / 9.0f;
    int want_wide = host_a > 4.0f / 3.0f ? 1 : 0;
    if (fci_aspect_wide() != want_wide) {
        fprintf(stderr, "FAIL fci_aspect_wide: got %d want %d\n", fci_aspect_wide(), want_wide);
        fails++;
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
