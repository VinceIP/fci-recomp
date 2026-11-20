/* fci.ini's player settings inside the game's own Controller Options screen (both the profile's Options and
 * the pause menu's). The screen's option list gets the control settings after the game's rows, then a
 * DISPLAY AND GRAPHICS row; A on it fills the same list with the display and graphics settings, under a
 * header row each. All are the game's own option objects (COption), so the game draws the rows, the help
 * line, the A toggle and the value popup. A change applies at once where the
 * runtime can (graphics options, console variables) and is saved to fci.ini. Evidence and addresses:
 * re/notes/MENUS.md (Settings menu). RECOMP_FCI_SETTINGS_MENU=0: off. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "fci_image.h"

const char *fci_config_option_of(const char *key); /* fci_config.c */
const char *fci_config_ini_path(void);
int fci_gfx_set(const char *key_value);  /* xbr/fci_enhance.cpp */
int fci_cvar_set(const char *key_value); /* fci_cvars.c */
int fci_input_evolution_game(void);      /* fci_input.c */

/* ---- the settings ------------------------------------------------------------------------------- */

#define MAX_VALUES 10
struct setting {
    const char *key, *label, *help;
    int restart; /* 1: read at launch, so the change applies after a restart */
    const char *values[MAX_VALUES]; /* "ini value|LABEL" (LABEL defaults to the value), the default first */
};

/* not here: controls-vibration, the master switch behind the game's own CONTROLLER VIBRATION row */
static const struct setting settings[] = {
        {"gfx-res-scale", "RESOLUTION SCALE", "Internal resolution as a multiple of 640x480.", 1,
         {"auto|AUTO", "1", "1.5", "2", "2.5", "3", "4"}},
        {"gfx-aa", "ANTI-ALIASING", "Smooths jagged edges.", 0, {"1|ON", "0|OFF"}},
        {"gfx-anisotropic-filtering", "TEXTURE FILTERING", "Anisotropic filtering: sharper textures at an angle.",
         0, {"16|16X", "8|8X", "4|4X", "2|2X", "0|OFF"}},
        {"gfx-ssao", "AMBIENT OCCLUSION", "Shades creases and corners.", 0, {"1|ON", "0|OFF"}},
        {"gfx-ssao-strength", "OCCLUSION STRENGTH", "How dark the ambient occlusion is.", 0,
         {"1.2", "0.6", "0.9", "1.5", "2"}},
        {"gfx-ssao-radius", "OCCLUSION RADIUS", "How far the ambient occlusion reaches, in metres.", 0,
         {"1.5", "0.75", "1", "2", "3"}},
        {"gfx-shadow-scale", "SHADOW DETAIL", "Sun shadow resolution, a multiple of the original.", 0,
         {"auto|AUTO", "1", "2", "3", "4", "6", "8"}},
        {"gfx-shadow-filter", "SOFT SHADOWS", "Softer shadow edges.", 0, {"1|ON", "0|OFF"}},
        {"gfx-fog", "FOG", "The original fog, or volumetric fog with light shafts and haze.", 0,
         {"0|ORIGINAL", "3|VOLUMETRIC"}},
        {"gfx-object-draw-distance", "DRAW DISTANCE", "How far away objects are still drawn (original 48).", 0,
         {"64", "48", "96", "128"}},
        {"gfx-object-lod-distance", "OBJECT DETAIL", "How far objects keep their detailed models (original 8).", 0,
         {"16", "8", "24", "32"}},
        {"gfx-foliage-lod", "FOLIAGE DETAIL", "How far plants stay 3D before turning flat (original 7.2).", 0,
         {"16", "7.2", "24", "32"}},
        {"display-borderless", "FULLSCREEN", "Borderless fullscreen. Alt+Enter also toggles it.", 1,
         {"0|NO", "1|YES"}},
        {"display-aspect", "ASPECT RATIO", "The 3D view's shape; auto follows the window.", 1,
         {"4:3", "16:9", "16:10", "21:9", "32:9", "auto|AUTO"}},
        {"display-fov", "FIELD OF VIEW", "Vertical field of view in degrees; GAME keeps the original.", 1,
         {"0|GAME", "60", "70", "75", "80", "90", "100", "110"}},
        {"display-frame-rate", "FRAME RATE", "Frames a second; above 30 is experimental.", 1,
         {"30", "60", "120", "144", "0|UNLIMITED"}},
        {"engine-update-cap", "ENGINE AT 30 HZ", "Keeps physics and AI at the original rate above 30 fps.", 1,
         {"1|YES", "0|NO"}},
        {"display-vsync", "VSYNC", "Present in step with the display: no tearing.", 1, {"1|ON", "0|OFF"}},
        {"display-show-fps", "SHOW FPS", "The frame rate in the window's title.", 1, {"0|OFF", "1|ON"}},
        {"skip-intro-movies", "SKIP INTRO MOVIES", "Starts at the title screen.", 1, {"0|NO", "1|YES"}},
        {"controls-mouse-sensitivity", "MOUSE SENSITIVITY", "Mouse look speed.", 1,
         {"1.0|1.0", "0.25", "0.5", "0.75", "1.25", "1.5", "2", "3"}},
        {"controls-mouse-invert-y", "INVERT MOUSE", "Invert vertical mouse look.", 1, {"0|NO", "1|YES"}},
        {"controls-mouse-aim", "DIRECT MOUSE AIM", "On foot, the mouse turns the view by an exact angle.", 1,
         {"1|ON", "0|OFF"}},
        {"controls-mouse-aim-assist", "MOUSE AIM ASSIST", "Aim assist with mouse and keyboard, as with a gamepad.", 1,
         {"0|OFF", "1|ON"}},
        {"controls-mouse-keyboard", "MOUSE AND KEYBOARD", "Off: controllers only.", 1, {"1|ON", "0|OFF"}},
};
#define N_SETTINGS (sizeof settings / sizeof *settings)

/* the controls go in Controller Options; display and graphics on our page, each under its header */
enum { SEC_CONTROLS, SEC_DISPLAY, SEC_GRAPHICS };
static int section_of(const struct setting *s)
{
    if (!strncmp(s->key, "controls-", 9))
        return SEC_CONTROLS;
    return strncmp(s->key, "gfx-", 4) ? SEC_DISPLAY : SEC_GRAPHICS;
}
#define PAGE_TITLE "GRAPHICS"

/* ini keys the runtime reads itself, not through an environment option (fci_config.c) */
static const char *setting_env(const char *key)
{
    return fci_config_option_of(key);
}

/* value i of setting s: the ini text, and the label shown */
static void value_parts(const char *v, char *ini, size_t ini_size, char *label, size_t label_size)
{
    const char *bar = strchr(v, '|');
    size_t n = bar ? (size_t)(bar - v) : strlen(v);
    snprintf(ini, ini_size, "%.*s", (int)n, v);
    snprintf(label, label_size, "%s", bar ? bar + 1 : v);
}

static int n_values(const struct setting *s)
{
    int n = 0;
    while (n < MAX_VALUES && s->values[n])
        n++;
    return n;
}

/* the value in use: the option the runtime got, or fci.ini's line, or the default */
static int current_value(const struct setting *s)
{
    char ini[32], label[32], have[64] = "";
    const char *env = setting_env(s->key), *e = env ? getenv(env) : NULL;
    if (e)
        snprintf(have, sizeof have, "%s", e);
    else {
        FILE *f = fopen(fci_config_ini_path(), "r");
        char line[512];
        size_t kl = strlen(s->key);
        while (f && fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (_strnicmp(p, s->key, kl) || (p[kl] != ' ' && p[kl] != '=' && p[kl] != '\t'))
                continue;
            p = strchr(p, '=');
            if (!p)
                continue;
            for (p++; *p == ' ' || *p == '\t'; p++)
                ;
            snprintf(have, sizeof have, "%s", p);
            have[strcspn(have, " \t\r\n;")] = 0;
        }
        if (f)
            fclose(f);
    }
    if (!have[0])
        return 0;
    for (int i = 0; i < n_values(s); i++) {
        value_parts(s->values[i], ini, sizeof ini, label, sizeof label);
        if (!_stricmp(ini, have) || (atof(ini) == atof(have) && atof(have) != 0))
            return i;
    }
    return 0;
}

/* ---- fci.ini ---------------------------------------------------------------------------------------- */

/* replace the key's line (commented or not) with "key = value", or add it; the rest stays as it is */
static int ini_save(const char *key, const char *value)
{
    const char *path = fci_config_ini_path();
    char tmp[MAX_PATH], line[1024];
    size_t kl = strlen(key);
    int done = 0;
    FILE *in = fopen(path, "rb"), *out;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(out = fopen(tmp, "wb"))) {
        if (in)
            fclose(in);
        return 0;
    }
    while (in && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == ';')
            p++;
        if (!done && !_strnicmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '=' || p[kl] == '\t')
            && strchr(p, '=')) {
            const char *eol = strchr(line, '\r') ? "\r\n" : "\n";
            fprintf(out, "%s = %s%s", key, value, eol);
            done = 1;
        } else {
            fputs(line, out);
        }
    }
    if (!done)
        fprintf(out, "%s = %s\r\n", key, value);
    if (in)
        fclose(in);
    fclose(out);
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

/* ---- the game's code ---------------------------------------------------------------------------------- */

struct menu_addrs {
    int evo;              /* Evolution's register conventions (noted per field as Instincts / Evolution) */
    uint32_t fill;        /* the Controller Options screen's fill (screen; ret 4) */
    uint32_t list_clear;  /* eax = list */
    uint32_t list_add;    /* edi / eax = list, (option); ret 4 */
    uint32_t list_rows;   /* (list); ret 4: the list widget's rows from the options */
    uint32_t list_help;   /* ecx / edi = list: the selected row's help text */
    uint32_t list_value;  /* (list, option); ret 8 / esi = list, (option); ret 4: the row shows its value */
    uint32_t list_press;  /* eax = event, (list); ret 4 / eax = event, ecx = list: A on the selected row */
    uint32_t alloc;       /* eax = size / cdecl (size, 0): operator new */
    uint32_t vt_dtor, vt_index, vt_load, vt_can_prev, vt_can_next, nop_ret4; /* COption's methods */
    uint32_t empty_alloc; /* the vectors' allocator object */
    uint32_t list_offset; /* the option list inside the screen */
    uint32_t find_child;  /* eax = page / (page), ecx = name: the element */
    uint32_t set_text;    /* ecx = string, eax = characters, (length); ret 4 / ebx = string, (characters) */
    uint32_t title_name;  /* "B_TitleButton" in the image */
};

static const struct menu_addrs instincts = {
        0,          0x00182570, 0x00152430, 0x001521B0, 0x00152510, 0x00152800, 0x00152730, 0x00152AF0,
        0x0033CA60, 0x001817E0, 0x00151AB0, 0x00151450, 0x00151A70, 0x00151AC0, 0x002DF6F0, 0x006F5E3C,
        0x60,       0x000665A0, 0x00068750, 0x00606524,
};

static const struct menu_addrs evolution = {
        1,          0x00119C3D, 0x000FD6DE, 0x000FD4C1, 0x000FD74D, 0x000FD901, 0x000FD886, 0x000FDC7B,
        0x001FEACC, 0x000DCCE1, 0x000FCF84, 0x000FCC85, 0x000FCF5E, 0x000FCF8E, 0x000BF67C, 0x005ADF2D,
        0x60,       0x0004F7FC, 0x00050EC4, 0x004BB44C,
};

static const struct menu_addrs *addrs(void)
{
    if (_stricmp(FCI_IMAGE_FILENAME, "default.xbe"))
        return NULL;
    if (fci_input_evolution_game())
        return &evolution;
    if (!strcmp(FCI_TITLE_NAME, "Far Cry Instincts"))
        return &instincts;
    return NULL;
}

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_FCI_SETTINGS_MENU");
        on = !(e && *e == '0') && addrs() != NULL;
    }
    return on;
}

struct regs {
    uint32_t eax, ecx, ebx, esi, edi;
};

/* call a lifted function as the game would: registers in, arguments pushed, its result in eax */
static uint32_t gcall(uint32_t fn, struct regs r, int n, const uint32_t *args)
{
    const uint32_t sp = g_esp, a = g_eax, c = g_ecx, d = g_edx, b = g_ebx, si = g_esi, di = g_edi;
    recomp_func_t f = recomp_lookup(fn);
    if (!f) {
        fprintf(stderr, "[FCI] settings menu: no translation at %08X\n", fn);
        return 0;
    }
    for (int i = n; i-- > 0;) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0xFE0F0000u; /* a return address no code has: the lifted ret pops it */
    g_eax = r.eax, g_ecx = r.ecx, g_ebx = r.ebx, g_esi = r.esi, g_edi = r.edi;
    f();
    const uint32_t result = g_eax;
    g_esp = sp;
    g_eax = a, g_ecx = c, g_edx = d, g_ebx = b, g_esi = si, g_edi = di;
    return result;
}

static uint32_t galloc(uint32_t bytes)
{
    const struct menu_addrs *m = addrs();
    const uint32_t args[2] = {bytes, 0};
    uint32_t p = m->evo ? gcall(m->alloc, (struct regs){0}, 2, args) : gcall(m->alloc, (struct regs){.eax = bytes}, 0, NULL);
    if (p)
        memset((void *)XBOX_PTR(p), 0, bytes);
    return p;
}

/* the game's std::wstring (28 bytes: allocator, 8-character buffer or pointer, size, capacity) at p */
static void wstr_set(uint32_t p, const char *text)
{
    const uint32_t len = (uint32_t)strlen(text);
    uint32_t buf = p + 4;
    if (len >= 8) {
        buf = galloc((len + 1) * 2);
        MEM32(p + 4) = buf;
    }
    for (uint32_t i = 0; i <= len; i++)
        MEM16(buf + i * 2) = (uint8_t)text[i];
    MEM32(p + 0x14) = len;
    MEM32(p + 0x18) = len >= 8 ? len : 7;
}

/* ---- the option objects ---------------------------------------------------------------------------- */

static uint32_t s_vtable;              /* our COption vtable: the base's methods, a no-op visit */
static uint32_t s_list;                /* the list our objects are in */
static uint32_t s_entry;               /* the DISPLAY AND GRAPHICS row */
static uint32_t s_heads[2];            /* the page's DISPLAY and GRAPHICS header rows */
static uint32_t s_opts[N_SETTINGS];    /* our rows, while the list holds them */
static int s_index[N_SETTINGS];        /* the value each showed */
static int s_chosen[N_SETTINGS];       /* 1 + the value picked this session (saved, maybe not yet in use) */

/* a COption (0x80 bytes): +04 help, +20 name, +3C class name, +58 value index, +5C/+6C values, +7C locked */
static uint32_t make_option(const char *name, const char *help, int n, const char *const *labels, int index)
{
    const struct menu_addrs *m = addrs();
    if (!s_vtable) {
        const uint32_t vt[6] = {m->vt_dtor, m->nop_ret4, m->vt_index, m->vt_load, m->vt_can_prev, m->vt_can_next};
        if (!(s_vtable = galloc(sizeof vt)))
            return 0;
        memcpy((void *)XBOX_PTR(s_vtable), vt, sizeof vt);
    }
    uint32_t o = galloc(0x80);
    if (!o)
        return 0;
    MEM32(o) = s_vtable;
    wstr_set(o + 0x04, help);
    wstr_set(o + 0x20, name);
    MEM32(o + 0x54) = 15; /* the class name: an empty std::string */
    MEM32(o + 0x58) = (uint32_t)index;
    /* two vectors of the value texts: +5C the values, +6C what the row and the popup show */
    for (uint32_t v = 0x5C; v <= 0x6C; v += 0x10) {
        const uint32_t values = n ? galloc((uint32_t)n * 28) : 0;
        if (n && !values)
            return 0;
        for (int i = 0; i < n; i++)
            wstr_set(values + (uint32_t)i * 28, labels[i]);
        MEM32(o + v) = m->empty_alloc;
        MEM32(o + v + 4) = values;
        MEM32(o + v + 8) = values + (uint32_t)n * 28;
        MEM32(o + v + 12) = values + (uint32_t)n * 28;
    }
    return o;
}

static uint32_t make_setting(int i)
{
    const struct setting *s = &settings[i];
    char ini[32], labels[MAX_VALUES][32], help[200];
    const char *ptrs[MAX_VALUES];
    int n = n_values(s);
    for (int v = 0; v < n; v++) {
        value_parts(s->values[v], ini, sizeof ini, labels[v], sizeof labels[v]);
        ptrs[v] = labels[v];
    }
    snprintf(help, sizeof help, "%s%s", s->help, s->restart ? " Applies after a restart." : "");
    s_index[i] = s_chosen[i] ? s_chosen[i] - 1 : current_value(s);
    return make_option(s->label, help, n, ptrs, s_index[i]);
}

static void list_add(uint32_t list, uint32_t option)
{
    const uint32_t arg = option;
    const struct regs r = addrs()->evo ? (struct regs){.eax = list} : (struct regs){.edi = list};
    gcall(addrs()->list_add, r, 1, &arg);
}

static void row_value(uint32_t list, uint32_t option)
{
    const uint32_t args[2] = {list, option};
    if (addrs()->evo)
        gcall(addrs()->list_value, (struct regs){.esi = list}, 1, &args[1]);
    else
        gcall(addrs()->list_value, (struct regs){0}, 2, args);
}

/* the rows from the options, each with its value (the game sets values through a listener ours lack) */
static void list_show(uint32_t list)
{
    const uint32_t arg = list;
    gcall(addrs()->list_rows, (struct regs){0}, 1, &arg);
    if (s_entry)
        row_value(list, s_entry);
    for (int k = 0; k < 2; k++)
        if (s_heads[k])
            row_value(list, s_heads[k]);
    for (int i = 0; i < (int)N_SETTINGS; i++)
        if (s_opts[i])
            row_value(list, s_opts[i]);
    gcall(addrs()->list_help, addrs()->evo ? (struct regs){.edi = list} : (struct regs){.ecx = list}, 0, NULL);
}

/* the guest text (UTF-16, len characters) reads as s, ignoring case */
static int text_is(uint32_t chars, uint32_t len, const char *s)
{
    if (len != strlen(s) || len > 64)
        return 0;
    for (uint32_t i = 0; i < len; i++)
        if ((MEM16(chars + i * 2) | 0x20) != ((uint8_t)s[i] | 0x20))
            return 0;
    return 1;
}

/* the screen's title, only where it still reads as expected (the element's text field at +3C) */
static void set_title(uint32_t screen, const char *expect, const char *title)
{
    const struct menu_addrs *m = addrs();
    const uint32_t pages = MEM32(screen + 0x4C);
    for (uint32_t p = 0; pages && p < 2; p++) {
        const uint32_t page = MEM32(pages + p * 4);
        const uint32_t elem = m->evo ? gcall(m->find_child, (struct regs){.ecx = m->title_name}, 1, &page)
                                     : gcall(m->find_child, (struct regs){.eax = page, .ecx = m->title_name}, 0, NULL);
        const uint32_t widget = elem ? MEM32(elem + 8) : 0, field = widget + 0x3C;
        if (!widget)
            continue;
        const uint32_t len = MEM32(field + 0x14), cap = MEM32(field + 0x18);
        const uint32_t chars = cap >= 8 ? MEM32(field + 4) : field + 4;
        if (text_is(chars, len, title))
            return;
        if (!text_is(chars, len, expect))
            continue;
        const uint32_t n = (uint32_t)strlen(title), text = galloc((n + 1) * 2);
        if (!text)
            return;
        for (uint32_t i = 0; i <= n; i++)
            MEM16(text + i * 2) = (uint8_t)title[i];
        if (m->evo)
            gcall(m->set_text, (struct regs){.ebx = field}, 1, &text);
        else
            gcall(m->set_text, (struct regs){.eax = text, .ecx = field}, 1, &n);
        return;
    }
    fprintf(stderr, "[FCI] settings menu: title not found\n");
}

/* a value picked: apply it where the runtime reads it live, and save it */
static void changed(int i, int index)
{
    const struct setting *s = &settings[i];
    char ini[32], label[32], kv[96];
    if (index < 0 || index >= n_values(s))
        return;
    s_chosen[i] = index + 1;
    value_parts(s->values[index], ini, sizeof ini, label, sizeof label);
    const char *env = setting_env(s->key);
    int live = 0;
    if (env && !s->restart) {
        snprintf(kv, sizeof kv, "%s=%s", env, _stricmp(ini, "auto") ? ini : "");
        live = fci_gfx_set(kv) || fci_cvar_set(kv);
    }
    int saved = ini_save(s->key, ini);
    fprintf(stderr, "[FCI] settings menu: %s = %s (%s, %s)\n", s->key, ini, live ? "applied" : "at restart",
            saved ? "saved" : "NOT SAVED");
    fflush(stderr);
}

/* the option is still in the list (the screen may have been closed and its objects freed since) */
static int in_list(uint32_t list, uint32_t option)
{
    const uint32_t begin = MEM32(list + 0x18), end = MEM32(list + 0x1C);
    if (!begin || end < begin || end - begin > 0x400)
        return 0;
    for (uint32_t p = begin; p < end; p += 4)
        if (MEM32(p) == option)
            return MEM32(option) == s_vtable;
    return 0;
}

/* the section's settings onto the list, after a header row (one empty value) when it has a name */
static int add_section(uint32_t list, int section, const char *head)
{
    static const char *const blank[] = {""};
    int n = 0;
    if (head && (s_heads[section - SEC_DISPLAY] = make_option(head, "", 1, blank, 0)) != 0)
        list_add(list, s_heads[section - SEC_DISPLAY]);
    for (int i = 0; i < (int)N_SETTINGS; i++)
        if (section_of(&settings[i]) == section && (s_opts[i] = make_setting(i)) != 0) {
            list_add(list, s_opts[i]);
            n++;
        }
    return n;
}

static void forget_rows(uint32_t list)
{
    memset(s_opts, 0, sizeof s_opts);
    memset(s_heads, 0, sizeof s_heads);
    s_entry = 0;
    s_list = list;
}

/* our page is in the list (its header rows exist only there) */
static int page_shown(void) { return s_list && s_heads[0] && in_list(s_list, s_heads[0]); }

/* the list now shows the display and graphics settings instead of the controller's */
static void fill_settings(uint32_t list)
{
    set_title(list - addrs()->list_offset, "CONTROLLER OPTIONS", PAGE_TITLE);
    gcall(addrs()->list_clear, (struct regs){.eax = list}, 0, NULL);
    forget_rows(list);
    int n = add_section(list, SEC_DISPLAY, "DISPLAY");
    n += add_section(list, SEC_GRAPHICS, "GRAPHICS");
    list_show(list);
    fprintf(stderr, "[FCI] settings menu: %d display and graphics settings shown\n", n);
}

static void check_changes(uint32_t list)
{
    if (!list || list != s_list)
        return;
    for (int i = 0; i < (int)N_SETTINGS; i++) {
        if (!s_opts[i] || !in_list(list, s_opts[i]))
            continue;
        int index = (int)MEM32(s_opts[i] + 0x58);
        if (index != s_index[i]) {
            s_index[i] = index;
            changed(i, index);
            row_value(list, s_opts[i]);
        }
    }
}

/* the lifted bodies, present in the images whose profile gates them */
__attribute__((weak)) void sub_00182570_fci_body(void);
__attribute__((weak)) void sub_00152AF0_fci_body(void);
__attribute__((weak)) void sub_00152510_fci_body(void);
__attribute__((weak)) void sub_00151AB0_fci_body(void);
__attribute__((weak)) void sub_00151FD0_fci_body(void);
__attribute__((weak)) void sub_00119C3D_fci_body(void);
__attribute__((weak)) void sub_000FDC7B_fci_body(void);
__attribute__((weak)) void sub_000FD74D_fci_body(void);
__attribute__((weak)) void sub_000FCF84_fci_body(void);
__attribute__((weak)) void sub_000FD37D_fci_body(void);

/* ---- hooks, the same for both titles ---------------------------------------------------------------- */

/* the Controller Options screen's fill (screen; ret 4): the game's rows, then our entry */
static int on_fill(void (*body)(void))
{
    const struct menu_addrs *m = addrs();
    if (!enabled() || !m || !body)
        return 0;
    const uint32_t screen = MEM32(g_esp + 4), list = screen + m->list_offset;
    body();
    static const char *const more[] = {">"};
    forget_rows(list);
    const int n = add_section(list, SEC_CONTROLS, NULL);
    if ((s_entry = make_option("DISPLAY AND GRAPHICS", "Window, frame rate, field of view and graphics quality.",
                               1, more, 0)) != 0)
        list_add(list, s_entry);
    list_show(list);
    fprintf(stderr, "[FCI] settings menu: %d control settings added\n", n);
    return 1;
}

/* A on an option list's row (eax = event): our entry row opens our settings; after the game's toggle, a
 * changed value is applied and saved. pop is what the handler's ret takes off the stack. */
static int on_press(void (*body)(void), uint32_t list, uint32_t pop)
{
    if (!enabled() || !addrs() || !body)
        return 0;
    const uint32_t event = g_eax;
    if (list && list == s_list && s_entry && MEM32(event) == 0x2F) {
        const uint32_t widget = MEM32(list + 4), options = MEM32(list + 0x18);
        const int16_t row = (int16_t)MEM16(widget + 0xC4);
        if (row >= 0 && options && MEM32(options + (uint32_t)row * 4) == s_entry) {
            fill_settings(list);
            g_esp += pop;
            return 1;
        }
    }
    body();
    check_changes(list);
    return 1;
}

/* an option list's rows rebuilt ((list); ret 4): after a value picked in the popup */
static int on_rows(void (*body)(void))
{
    if (!enabled() || !body)
        return 0;
    const uint32_t list = MEM32(g_esp + 4);
    body();
    check_changes(list);
    return 1;
}

/* COption's value index set (ecx = option, index; ret 4), as the value popup sets it: one of our rows
 * applies, saves and shows the new value */
static int on_set_index(void (*body)(void))
{
    if (!enabled() || !body || !s_vtable || MEM32(g_ecx) != s_vtable)
        return 0;
    const uint32_t option = g_ecx;
    body();
    if (s_list && in_list(s_list, option))
        check_changes(s_list);
    return 1;
}

/* the value popup's input (ecx = handler, event, ...; ret 8): closing it shows the screen's own title
 * again, so ours is put back */
static int on_popup(void (*body)(void))
{
    if (!enabled() || !body || !page_shown())
        return 0;
    body();
    set_title(s_list - addrs()->list_offset, "CONTROLLER OPTIONS", PAGE_TITLE);
    return 1;
}

/* ---- the gates: Instincts, then Evolution ---------------------------------------------------------- */

int fci_gate_00182570(void) { return on_fill(sub_00182570_fci_body); }
int fci_gate_00152AF0(void) { return on_press(sub_00152AF0_fci_body, MEM32(g_esp + 4), 8); } /* list on the stack */
int fci_gate_00152510(void) { return on_rows(sub_00152510_fci_body); }
int fci_gate_00151AB0(void) { return on_set_index(sub_00151AB0_fci_body); }
int fci_gate_00151FD0(void) { return on_popup(sub_00151FD0_fci_body); }

int fci_gate_00119C3D(void) { return on_fill(sub_00119C3D_fci_body); }
int fci_gate_000FDC7B(void) { return on_press(sub_000FDC7B_fci_body, g_ecx, 4); } /* list in ecx */
int fci_gate_000FD74D(void) { return on_rows(sub_000FD74D_fci_body); }
int fci_gate_000FCF84(void) { return on_set_index(sub_000FCF84_fci_body); }
int fci_gate_000FD37D(void) { return on_popup(sub_000FD37D_fci_body); }
