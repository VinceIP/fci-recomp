/* fci.ini (template: src/fci.ini), read when the game starts without arguments: each key becomes the
 * RECOMP_* option the code reads. Also the session logs, one per session in log-dir. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fci_image.h"

#define SESSION_END "[FCI] session ended normally"

/* what fci.ini got wrong, printed once the log is open */
static char warnings[2048];

static void warn(const char *what, const char *key)
{
    size_t used = strlen(warnings);
    snprintf(warnings + used, sizeof warnings - used, "[FCI] fci.ini: %s %s\n", what, key);
}

/* ini key -> runtime option, value passed on unchanged unless noted in translate() */
static const struct {
    const char *key, *option;
} keys[] = {
        {"log-input", "RECOMP_INPUT_LOG"},
        {"display-borderless", "RECOMP_FCI_FULLSCREEN"},
        {"display-dimensions", "RECOMP_FCI_WINDOW"},
        {"display-aspect", "RECOMP_FCI_ASPECT"},
        {"skip-intro-movies", "RECOMP_FCI_SKIP_INTROS"},
        {"gfx-res-scale", "RECOMP_FCI_XBR_SCALE"},
        {"gfx-anisotropic-filtering", "RECOMP_FCI_XBR_ANISO"},
        {"gfx-ssao", "RECOMP_FCI_XBR_AO"},
        {"gfx-ssao-strength", "RECOMP_FCI_XBR_AO_STRENGTH"},
        {"gfx-ssao-radius", "RECOMP_FCI_XBR_AO_RADIUS"},
        {"gfx-aa", "RECOMP_FCI_XBR_AA"},
        {"gfx-shadow-scale", "RECOMP_FCI_XBR_SHADOW_SCALE"},
        {"gfx-shadow-filter", "RECOMP_FCI_XBR_SHADOW_FILTER"},
        {"gfx-object-draw-distance", "RECOMP_CVAR_e_obj_view_dist_ratio"},
        {"gfx-object-lod-distance", "RECOMP_CVAR_e_obj_lod_ratio"},
        {"gfx-foliage-lod", "RECOMP_CVAR_e_obj_sprite_switch_dist_ratio"},
        {"controls-mouse-keyboard", "RECOMP_KEYBOARD"},
        {"controls-walk-speed", "RECOMP_WALK_SCALE"},
        {"controls-wheel-hold-ms", "RECOMP_WHEEL_HOLD_MS"},
        {"controls-mouse-sensitivity", "RECOMP_MOUSE_SENSITIVITY"},
        {"controls-mouse-degrees", "RECOMP_MOUSE_DEGREES"},
        {"controls-mouse-aim", "RECOMP_MOUSE_AIM"},
        {"controls-mouse-y-scale", "RECOMP_MOUSE_Y_SCALE"},
        {"controls-mouse-invert-y", "RECOMP_MOUSE_INVERT_Y"},
        {"controls-mouse-capture", "RECOMP_MOUSE_CAPTURE"},
        {"controls-mouse-stick-curve", "RECOMP_MOUSE_CURVE"},
        {"controls-mouse-stick-deadzone", "RECOMP_MOUSE_DEADZONE"},
        {"controls-mouse-stick-window-ms", "RECOMP_MOUSE_WINDOW_MS"},
        {"controls-stick-aim", "RECOMP_STICK_AIM"},
        {"controls-stick-speed", "RECOMP_STICK_SPEED"},
        {"controls-stick-y-scale", "RECOMP_STICK_Y_SCALE"},
        {"controls-stick-deadzone", "RECOMP_STICK_DEADZONE"},
        {"controls-stick-curve", "RECOMP_STICK_CURVE"},
        {"controls-stick-boost", "RECOMP_STICK_BOOST"},
        {"controls-stick-boost-ms", "RECOMP_STICK_BOOST_MS"},
        {"controls-stick-invert-y", "RECOMP_STICK_INVERT_Y"},
};

/* controls-bind-<name> -> RECOMP_BIND_<CONTROL> (fci_input.c) */
static const struct {
    const char *name, *control;
} binds[] = {
        {"forward", "FORWARD"},
        {"backwards", "BACKWARD"},
        {"strafe-left", "STRAFE_LEFT"},
        {"strafe-right", "STRAFE_RIGHT"},
        {"walk", "WALK"},
        {"look-up", "LOOK_UP"},
        {"look-down", "LOOK_DOWN"},
        {"look-left", "LOOK_LEFT"},
        {"look-right", "LOOK_RIGHT"},
        {"rt", "RT"},
        {"lt", "LT"},
        {"rs", "RS"},
        {"ls", "LS"},
        {"a", "A"},
        {"b", "B"},
        {"x", "X"},
        {"y", "Y"},
        {"black", "BLACK"},
        {"white", "WHITE"},
        {"start", "START"},
        {"back", "BACK"},
        {"dpad-up", "DPAD_UP"},
        {"dpad-right", "DPAD_RIGHT"},
        {"dpad-down", "DPAD_DOWN"},
        {"dpad-left", "DPAD_LEFT"},
};

/* the runtime option for one ini line, in out; 0 when the line sets nothing (auto, an unknown key) */
static int translate(const char *key, const char *value, char *out, size_t size)
{
    size_t i;
    if (!strncmp(key, "RECOMP_", 7)) {
        snprintf(out, size, "%s=%s", key, value);
        return 1;
    }
    if (!strncmp(key, "controls-bind-", 14)) {
        for (i = 0; i < sizeof binds / sizeof binds[0]; i++)
            if (!_stricmp(key + 14, binds[i].name)) {
                snprintf(out, size, "RECOMP_BIND_%s=%s", binds[i].control, value);
                return 1;
            }
        warn("unknown control", key);
        return 0;
    }
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (_stricmp(key, keys[i].key))
            continue;
        if (!_stricmp(value, "auto")
            && (!strcmp(keys[i].key, "display-dimensions") || !strcmp(keys[i].key, "gfx-shadow-scale")))
            return 0; /* the runtime's own default */
        if (!strcmp(keys[i].key, "display-aspect"))
            value = !strcmp(value, "1") || !strcmp(value, "16:9") ? "16:9" : "4:3";
        if (!strcmp(keys[i].key, "gfx-ssao"))
            value = atoi(value) ? "1" : "0"; /* the debug views (2, 3) stay developer options */
        snprintf(out, size, "%s=%s", keys[i].option, value);
        return 1;
    }
    warn("unknown option", key);
    return 0;
}

static void make_dirs(const char *path)
{
    char buf[MAX_PATH];
    char *p;
    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf + 3; *p; p++) /* past the drive, "C:\" */
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = c;
        }
    CreateDirectoryA(buf, NULL);
}

static void expand(const char *in, char *out, size_t size)
{
    size_t n;
    if (!ExpandEnvironmentStringsA(in, out, (DWORD)size))
        snprintf(out, size, "%s", in);
    n = strlen(out);
    while (n > 3 && (out[n - 1] == '\\' || out[n - 1] == '/'))
        out[--n] = 0;
}

/* the last bytes of a log name a clean exit */
static int ended_normally(const char *path)
{
    char tail[256];
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (!fseek(f, -(long)(sizeof tail - 1), SEEK_END) || !fseek(f, 0, SEEK_SET))
        n = fread(tail, 1, sizeof tail - 1, f);
    fclose(f);
    tail[n] = 0;
    return strstr(tail, SESSION_END) != NULL;
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* delete the clean sessions' logs (verbosity 1), then the oldest until a new one fits in limit */
static void prune_logs(const char *dir, int verbosity, int limit)
{
    static char names[512][MAX_PATH];
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i, kept = 0;
    snprintf(pattern, sizeof pattern, "%s\\fci-*.log", dir);
    if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strstr(fd.cFileName, "-kernel.log") && n < 512)
            snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, by_name); /* the names hold the start time: oldest first */
    for (i = n - 1; i >= 0; i--) {
        int drop;
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        drop = verbosity == 1 && ended_normally(path);
        if (!drop && limit > 0 && kept >= limit - 1)
            drop = 1;
        if (!drop) {
            kept++;
            continue;
        }
        DeleteFileA(path);
        snprintf(path, sizeof path, "%s\\%.*s-kernel.log", dir, (int)(strlen(names[i]) - 4), names[i]);
        DeleteFileA(path);
    }
}

/* the session's log (and the kernel's trace with verbosity 2), when there is no console to print to */
static void open_session_log(const char *dir, int verbosity, int limit, const char *fixed)
{
    char path[MAX_PATH], kernel[MAX_PATH];
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) {
        if (verbosity == 0)
            _putenv("RECOMP_KERNEL_LOG_FILE=NUL");
        return; /* a console: the log stays there */
    }
    if (verbosity == 0) {
        freopen("NUL", "w", stderr);
        freopen("NUL", "w", stdout);
        _putenv("RECOMP_KERNEL_LOG_FILE=NUL");
        return;
    }
    if (fixed) { /* the older log=<file> */
        snprintf(path, sizeof path, "%s", fixed);
        snprintf(kernel, sizeof kernel, "NUL");
    } else {
        time_t now = time(NULL);
        struct tm t;
        char stem[64];
        localtime_s(&t, &now);
        strftime(stem, sizeof stem, "fci-%Y%m%d-%H%M%S", &t);
        make_dirs(dir);
        prune_logs(dir, verbosity, limit);
        snprintf(path, sizeof path, "%s\\%s.log", dir, stem);
        if (verbosity >= 2)
            snprintf(kernel, sizeof kernel, "%s\\%s-kernel.log", dir, stem);
        else
            snprintf(kernel, sizeof kernel, "NUL");
    }
    _putenv_s("RECOMP_KERNEL_LOG_FILE", kernel);
    SetEnvironmentVariableA("RECOMP_FCI_RELAUNCH_LOG", path); /* relaunched instances append */
    if (verbosity == 1)
        _putenv("RECOMP_FCI_LOG_SESSION=1"); /* fci_log_session_end marks a clean exit */
    freopen(path, "w", stderr);
    freopen(path, "a", stdout);
}

/* the window was closed: with log-verbosity 1 the next start deletes this session's log */
void fci_log_session_end(void)
{
    const char *on = getenv("RECOMP_FCI_LOG_SESSION");
    if (on && *on == '1') {
        fprintf(stderr, "%s\n", SESSION_END);
        fflush(stderr);
    }
}

int fci_default_launch;

/* Started without arguments: the arguments come from fci.ini. Defaults: the game files in "game" beside
 * the executable, saves and logs under the title's %APPDATA% folder (FCI_APPDATA), host Direct3D and
 * DirectSound. */
void fci_config_load(int *argc, char ***argv)
{
    static char exe_dir[MAX_PATH], xbe[MAX_PATH], game[MAX_PATH], save[MAX_PATH], log_dir[MAX_PATH];
    static char fixed_log[MAX_PATH], shader_cache[MAX_PATH], shader_cache_arg[MAX_PATH + 32];
    static char lines[96][512], rate_arg[64];
    static char *args[120];
    int n = 0, opts = 0, i, have_ini = 0, verbosity = 1, limit = 20, cap = 1;
    double rate = 30;
    char ini[MAX_PATH], *slash;
    FILE *f;

    if (*argc >= 4)
        return;
    fci_default_launch = 1;
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    if ((slash = strrchr(exe_dir, '\\')) != NULL)
        *slash = 0;
    snprintf(game, sizeof game, "%s\\game", exe_dir);
    expand("%APPDATA%\\" FCI_APPDATA "\\profile", save, sizeof save);
    expand("%APPDATA%\\" FCI_APPDATA "\\logs", log_dir, sizeof log_dir);
    expand("%APPDATA%\\" FCI_APPDATA "\\shader-cache", shader_cache, sizeof shader_cache);
    xbe[0] = fixed_log[0] = 0;
    snprintf(ini, sizeof ini, "%s\\fci.ini", exe_dir);
    if ((f = fopen(ini, "r")) != NULL) {
        char line[512];
        have_ini = 1;
        while (fgets(line, sizeof line, f)) {
            char *key = line, *v, *e;
            while (*key == ' ' || *key == '\t')
                key++;
            if (!*key || *key == '#' || *key == ';' || !(v = strchr(key, '=')))
                continue;
            for (e = v; e > key && (e[-1] == ' ' || e[-1] == '\t'); e--)
                ;
            *e = 0;
            v++;
            while (*v == ' ' || *v == '\t')
                v++;
            if ((e = strchr(v, ';')) != NULL) /* a note after the value: "MOUSE1 ; fire weapon" */
                *e = 0;
            for (e = v + strlen(v);
                 e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
                *--e = 0;
            if (!_stricmp(key, "save-dir") || !_stricmp(key, "save"))
                expand(v, save, sizeof save);
            else if (!_stricmp(key, "log-dir"))
                expand(v, log_dir, sizeof log_dir);
            else if (!_stricmp(key, "shader-cache"))
                expand(v, shader_cache, sizeof shader_cache);
            else if (!_stricmp(key, "log-verbosity"))
                verbosity = atoi(v);
            else if (!_stricmp(key, "log-limit"))
                limit = atoi(v);
            else if (!_stricmp(key, "log"))
                expand(v, fixed_log, sizeof fixed_log);
            else if (!_stricmp(key, "game-dir") || !_stricmp(key, "game"))
                expand(v, game, sizeof game);
            else if (!_stricmp(key, "xbe"))
                expand(v, xbe, sizeof xbe);
            else if (!_stricmp(key, "display-frame-rate"))
                rate = !strcmp(v, "0") || !_stricmp(v, "unlimited") ? 0 : atof(v) > 0 ? atof(v) : 30;
            else if (!_stricmp(key, "engine-update-cap"))
                cap = atoi(v) != 0;
            else if (opts < 96 && translate(key, v, lines[opts], sizeof lines[0]))
                opts++;
        }
        fclose(f);
    }
    if (verbosity < 0)
        verbosity = 0;
    if (verbosity > 2)
        verbosity = 2;
    if (!xbe[0])
        snprintf(xbe, sizeof xbe, "%s\\%s", game, FCI_IMAGE_FILENAME);
    make_dirs(save);
    open_session_log(log_dir, fixed_log[0] ? 2 : verbosity, limit, fixed_log[0] ? fixed_log : NULL);
    args[n++] = (*argv)[0];
    args[n++] = xbe;
    args[n++] = game;
    args[n++] = save;
    args[n++] = "RECOMP_FCI_XBR=1";
    args[n++] = "RECOMP_USB=1";
    args[n++] = "RECOMP_KEYBOARD=1";
    /* the players' defaults, as fci.ini shows them; tests and tools keep the original game's look */
    args[n++] = "RECOMP_FCI_XBR_SCALE=auto";
    args[n++] = "RECOMP_FCI_XBR_ANISO=16";
    args[n++] = "RECOMP_FCI_XBR_AO=1";
    args[n++] = "RECOMP_FCI_XBR_AA=1";
    args[n++] = "RECOMP_FCI_XBR_SHADOW_FILTER=1";
    args[n++] = "RECOMP_CVAR_e_obj_view_dist_ratio=64";
    args[n++] = "RECOMP_CVAR_e_obj_lod_ratio=16";
    args[n++] = "RECOMP_CVAR_e_obj_sprite_switch_dist_ratio=16";
    if (shader_cache[0]) {
        snprintf(shader_cache_arg, sizeof shader_cache_arg, "RECOMP_FCI_XBR_SHADER_CACHE=%s", shader_cache);
        args[n++] = shader_cache_arg;
    }
    /* experimental frame rates: capped, the game keeps its 30 updates and the
       frames in between are blended from its last two; uncapped, the game itself runs at the rate */
    if (rate == 0 || rate > 30) {
        if (cap)
            snprintf(rate_arg, sizeof rate_arg, "RECOMP_FCI_XBR_REPLAY=%g", rate ? rate : 1000);
        else if (rate)
            snprintf(rate_arg, sizeof rate_arg, "RECOMP_FCI_XBR_FPS=%g", rate);
        else
            snprintf(rate_arg, sizeof rate_arg, "RECOMP_FCI_XBR_UNLOCK=1");
        args[n++] = rate_arg;
    }
    for (i = 0; i < opts; i++)
        args[n++] = lines[i];
    args[n] = NULL;
    *argc = n;
    *argv = args;
    fprintf(stderr, "[FCI] started without arguments: %s (game %s, save %s, log verbosity %d)\n%s",
            have_ini ? ini : "no fci.ini, defaults", game, save, verbosity, warnings);
}
