/* Mouse and keyboard as the controller on port 0, merged over a real pad: bindable controls, direct aim
 * on foot (fci_aim.c), the mouse as the right stick elsewhere. The right stick aims directly on foot too,
 * past the game's look. Options: src/fci.ini. */
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* src/input/xinput_xbox.h, the state the runtime fills */
typedef struct {
    uint16_t wButtons;
    uint8_t analog[8]; /* A B X Y Black White LT RT */
    int16_t lx, ly, rx, ry;
} fci_gamepad;
typedef struct {
    uint32_t packet;
    fci_gamepad g;
} fci_input_state;

/* ---- controls and inputs ---- */
enum {
    C_A,
    C_B,
    C_X,
    C_Y,
    C_BLACK,
    C_WHITE,
    C_LT,
    C_RT, /* analog buttons, in order */
    C_DPAD_UP,
    C_DPAD_DOWN,
    C_DPAD_LEFT,
    C_DPAD_RIGHT,
    C_START,
    C_BACK,
    C_LS,
    C_RS, /* wButtons bits */
    C_FORWARD,
    C_BACKWARD,
    C_STRAFE_LEFT,
    C_STRAFE_RIGHT,
    C_LOOK_UP,
    C_LOOK_DOWN,
    C_LOOK_LEFT,
    C_LOOK_RIGHT,
    C_WALK,
    C_COUNT
};
static const char *const control_names[C_COUNT] = {
        "A",         "B",         "X",          "Y",         "BLACK",       "WHITE",        "LT",
        "RT",        "DPAD_UP",   "DPAD_DOWN",  "DPAD_LEFT", "DPAD_RIGHT",  "START",        "BACK",
        "LS",        "RS",        "FORWARD",    "BACKWARD",  "STRAFE_LEFT", "STRAFE_RIGHT", "LOOK_UP",
        "LOOK_DOWN", "LOOK_LEFT", "LOOK_RIGHT", "WALK",
};
/* defaults, a PC shooter's layout over the controller; every one can be rebound */
static const char *const control_defaults[C_COUNT] = {
        "SPACE,ENTER", "C,MOUSE4,BACKSPACE",
        "E,R",         "Q,WHEELUP,WHEELDOWN",
        "F",           "G",
        "MOUSE2",      "MOUSE1",
        "UP,1",        "DOWN,3",
        "LEFT,4",      "RIGHT,2",
        "ESCAPE",      "TAB",
        "LSHIFT",      "MOUSE3",
        "W",           "S",
        "A",           "D",
        "NONE",        "NONE",
        "NONE",        "NONE",
        "LCTRL",
};

/* inputs: virtual keys 0-255, then the mouse */
enum { IN_MOUSE1 = 256, IN_MOUSE2, IN_MOUSE3, IN_MOUSE4, IN_MOUSE5, IN_WHEELUP, IN_WHEELDOWN, IN_COUNT };

static const struct {
    const char *name;
    int code;
} input_names[] = {
        {"SPACE", VK_SPACE},     {"ENTER", VK_RETURN},
        {"ESCAPE", VK_ESCAPE},   {"ESC", VK_ESCAPE},
        {"TAB", VK_TAB},         {"BACKSPACE", VK_BACK},
        {"SHIFT", VK_SHIFT},     {"LSHIFT", VK_LSHIFT},
        {"RSHIFT", VK_RSHIFT},   {"CTRL", VK_CONTROL},
        {"LCTRL", VK_LCONTROL},  {"RCTRL", VK_RCONTROL},
        {"ALT", VK_MENU},        {"LALT", VK_LMENU},
        {"RALT", VK_RMENU},      {"CAPSLOCK", VK_CAPITAL},
        {"UP", VK_UP},           {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT},       {"RIGHT", VK_RIGHT},
        {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE},
        {"HOME", VK_HOME},       {"END", VK_END},
        {"PGUP", VK_PRIOR},      {"PGDN", VK_NEXT},
        {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS},
        {"LBRACKET", VK_OEM_4},  {"RBRACKET", VK_OEM_6},
        {"SEMICOLON", VK_OEM_1}, {"APOSTROPHE", VK_OEM_7},
        {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD},
        {"SLASH", VK_OEM_2},     {"BACKSLASH", VK_OEM_5},
        {"GRAVE", VK_OEM_3},     {"MOUSE1", IN_MOUSE1},
        {"MOUSE2", IN_MOUSE2},   {"MOUSE3", IN_MOUSE3},
        {"MOUSE4", IN_MOUSE4},   {"MOUSE5", IN_MOUSE5},
        {"WHEELUP", IN_WHEELUP}, {"WHEELDOWN", IN_WHEELDOWN},
};

static int parse_input(const char *s)
{
    size_t i;
    if (!s[0])
        return -1;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9')))
        return s[0];
    if ((s[0] == 'F') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    if (!strncmp(s, "NUMPAD", 6) && s[6] >= '0' && s[6] <= '9' && !s[7])
        return VK_NUMPAD0 + (s[6] - '0');
    for (i = 0; i < sizeof input_names / sizeof input_names[0]; i++)
        if (!strcmp(s, input_names[i].name))
            return input_names[i].code;
    return -1;
}

#define MAX_BINDS 4
static int binds[C_COUNT][MAX_BINDS];
static struct {
    double sens, degrees, y_scale, curve, deadzone, walk;
    double stick_speed, stick_y_scale, stick_deadzone, stick_curve, stick_boost;
    int aim, invert_y, window_ms, capture, wheel_ms, log;
    int stick_aim, stick_invert_y, stick_boost_ms;
} opt;

static double opt_num(const char *name, double fallback)
{
    const char *e = getenv(name);
    return e && *e ? atof(e) : fallback;
}

static void bind_control(int c, const char *spec)
{
    char buf[256], *tok, *next = NULL;
    int n = 0;
    for (int k = 0; k < MAX_BINDS; k++)
        binds[c][k] = -1;
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *p = buf; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    for (tok = strtok_s(buf, ", ", &next); tok; tok = strtok_s(NULL, ", ", &next)) {
        int code;
        if (!strcmp(tok, "NONE"))
            continue;
        if ((code = parse_input(tok)) < 0) {
            fprintf(stderr, "[FCI] input: RECOMP_BIND_%s: unknown input %s (ignored)\n", control_names[c],
                    tok);
            continue;
        }
        if (n < MAX_BINDS)
            binds[c][n++] = code;
    }
}

static INIT_ONCE config_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_config(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    for (int c = 0; c < C_COUNT; c++) {
        char name[64];
        const char *e;
        snprintf(name, sizeof name, "RECOMP_BIND_%s", control_names[c]);
        e = getenv(name);
        bind_control(c, e && *e ? e : control_defaults[c]);
    }
    opt.sens = opt_num("RECOMP_MOUSE_SENSITIVITY", 1.0);
    opt.degrees = opt_num("RECOMP_MOUSE_DEGREES", 0.05);
    opt.aim = opt_num("RECOMP_MOUSE_AIM", 1) != 0;
    opt.y_scale = opt_num("RECOMP_MOUSE_Y_SCALE", 1.0);
    opt.invert_y = opt_num("RECOMP_MOUSE_INVERT_Y", 0) != 0;
    opt.curve = opt_num("RECOMP_MOUSE_CURVE", 1.0);
    opt.deadzone = opt_num("RECOMP_MOUSE_DEADZONE", 0.0);
    opt.window_ms = (int)opt_num("RECOMP_MOUSE_WINDOW_MS", 33);
    opt.capture = opt_num("RECOMP_MOUSE_CAPTURE", 1) != 0;
    opt.walk = opt_num("RECOMP_WALK_SCALE", 0.5);
    opt.wheel_ms = (int)opt_num("RECOMP_WHEEL_HOLD_MS", 80);
    opt.log = opt_num("RECOMP_INPUT_LOG", 0) != 0;
    opt.stick_aim = opt_num("RECOMP_STICK_AIM", 0) != 0;
    opt.stick_speed = opt_num("RECOMP_STICK_SPEED", 180.0);
    opt.stick_y_scale = opt_num("RECOMP_STICK_Y_SCALE", 0.75);
    opt.stick_deadzone = opt_num("RECOMP_STICK_DEADZONE", 0.12);
    opt.stick_curve = opt_num("RECOMP_STICK_CURVE", 1.8);
    opt.stick_boost = opt_num("RECOMP_STICK_BOOST", 1.6);
    opt.stick_boost_ms = (int)opt_num("RECOMP_STICK_BOOST_MS", 350);
    opt.stick_invert_y = opt_num("RECOMP_STICK_INVERT_Y", 0) != 0;
    if (opt.stick_curve <= 0)
        opt.stick_curve = 1.0;
    if (opt.stick_deadzone < 0)
        opt.stick_deadzone = 0;
    if (opt.stick_deadzone > 0.9)
        opt.stick_deadzone = 0.9;
    if (opt.stick_boost < 1)
        opt.stick_boost = 1;
    if (opt.stick_boost_ms < 0)
        opt.stick_boost_ms = 0;
    if (opt.curve <= 0)
        opt.curve = 1.0;
    if (opt.window_ms < 4)
        opt.window_ms = 4;
    if (opt.window_ms > 500)
        opt.window_ms = 500;
    fprintf(stderr,
            "[FCI] input: mouse sensitivity %.2f (y %.2f%s), aim %s (%.4f degrees per count), stick "
            "curve %.2f, dead zone %.2f, window %d ms, capture %s\n",
            opt.sens, opt.y_scale, opt.invert_y ? ", inverted" : "", opt.aim ? "direct" : "stick",
            opt.degrees * opt.sens, opt.curve, opt.deadzone, opt.window_ms, opt.capture ? "on" : "off");
    if (opt.stick_aim)
        fprintf(stderr,
                "[FCI] input: stick aim direct, %.0f degrees/s (y %.2f%s), dead zone %.2f, curve %.2f, "
                "boost %.2f over %d ms\n",
                opt.stick_speed, opt.stick_y_scale, opt.stick_invert_y ? ", inverted" : "",
                opt.stick_deadzone, opt.stick_curve, opt.stick_boost, opt.stick_boost_ms);
    else
        fprintf(stderr, "[FCI] input: stick aim through the game's look\n");
    fprintf(stderr, "[FCI] input: bindings");
    for (int c = 0; c < C_COUNT; c++) {
        fprintf(stderr, " %s=", control_names[c]);
        for (int k = 0, any = 0; k < MAX_BINDS; k++) {
            if (binds[c][k] < 0)
                continue;
            fprintf(stderr, "%s%d", any++ ? "+" : "", binds[c][k]);
        }
    }
    fprintf(stderr, "\n");
    return TRUE;
}

static void config(void) { InitOnceExecuteOnce(&config_once, load_config, NULL, NULL); }

/* ---- state from the window (its thread) ---- */
static volatile LONG down[IN_COUNT];
static volatile LONGLONG wheel_until[2]; /* up, down: held until this tick (ms) */
static volatile LONG captured;

#define MOTION_RING 1024
static struct {
    ULONGLONG t;
    int dx, dy;
} motion[MOTION_RING];
static unsigned motion_head;
static SRWLOCK motion_lock = SRWLOCK_INIT;

/* a key (virtual key, left/right resolved by the caller) or mouse button: the generic Shift/Ctrl/Alt follow */
void fci_input_key(int code, int is_down)
{
    if (code < 0 || code >= IN_COUNT)
        return;
    InterlockedExchange(&down[code], is_down ? 1 : 0);
    if (code == VK_LSHIFT || code == VK_RSHIFT)
        InterlockedExchange(&down[VK_SHIFT], down[VK_LSHIFT] | down[VK_RSHIFT]);
    if (code == VK_LCONTROL || code == VK_RCONTROL)
        InterlockedExchange(&down[VK_CONTROL], down[VK_LCONTROL] | down[VK_RCONTROL]);
    if (code == VK_LMENU || code == VK_RMENU)
        InterlockedExchange(&down[VK_MENU], down[VK_LMENU] | down[VK_RMENU]);
}

void fci_input_release_all(void)
{
    for (int i = 0; i < IN_COUNT; i++)
        InterlockedExchange(&down[i], 0);
}

void fci_input_wheel(int notches)
{
    config();
    if (notches)
        wheel_until[notches > 0 ? 0 : 1] = (LONGLONG)GetTickCount64() + opt.wheel_ms;
}

/* direct aim (fci_aim.c): counts since the player's input step last took them */
#define AIM_LIVE_MS 250 /* the step ran this recently: the mouse and the right stick aim directly */
static volatile LONG aim_dx, aim_dy;
static volatile LONGLONG aim_seen;

static int step_live(void) { return (LONGLONG)GetTickCount64() - aim_seen < AIM_LIVE_MS; }
static int aim_live(void) { return opt.aim && step_live(); }
static int stick_live(void) { return opt.stick_aim && step_live(); }

/* the right stick, taken out of the pad reports while it aims directly */
#define STICK_PADS     4
#define STICK_FRESH_MS 100 /* a pad not reported for this long has let go */
static volatile LONG stick_rx[STICK_PADS], stick_ry[STICK_PADS];
static volatile LONGLONG stick_t[STICK_PADS];
static LARGE_INTEGER stick_last; /* the input step that last turned by the stick; 0: resuming */
static double stick_held_ms;     /* how long the stick has been at its rim */

/* a finished pad report (fci_record.c), before the title reads it: while the stick aims directly the game's
 * look never sees it, so its axis snap, turn ramp and target slowdown (re/notes/INPUT-CAMERA.md) are gone */
void fci_input_pad_stick(int pad, uint8_t *report)
{
    config();
    if (pad < 0 || pad >= STICK_PADS || !stick_live())
        return;
    stick_rx[pad] = (int16_t)(report[16] | report[17] << 8);
    stick_ry[pad] = (int16_t)(report[18] | report[19] << 8);
    stick_t[pad] = (LONGLONG)GetTickCount64();
    memset(report + 16, 0, 4);
}

/* this step's turn from the right stick, in degrees, added to *yaw and *pitch: a round dead zone, the curve,
 * and the boost while the stick is held at its rim. Returns whether it turned. */
static int stick_turn(float *yaw, float *pitch)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    LONGLONG ms = (LONGLONG)GetTickCount64();
    double dt, x = 0, y = 0, best = 0, mag, amount, boost = 1;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    dt = stick_last.QuadPart ? (double)(now.QuadPart - stick_last.QuadPart) / (double)freq.QuadPart : 0;
    stick_last = now;
    /* a long frame: no jump */
    if (dt > 0.1)
        dt = 0.1;
    /* the pad pushed furthest */
    for (int p = 0; p < STICK_PADS; p++) {
        double px = stick_rx[p] / 32767.0, py = stick_ry[p] / 32767.0, m = px * px + py * py;
        if (ms - stick_t[p] < STICK_FRESH_MS && m > best) {
            best = m;
            x = px;
            y = py;
        }
    }
    mag = sqrt(best);
    if (mag <= opt.stick_deadzone) {
        stick_held_ms = 0;
        return 0;
    }
    /* zero at the dead zone's edge, full a little short of the rim (worn sticks seldom reach it) */
    amount = (mag - opt.stick_deadzone) / (0.97 - opt.stick_deadzone);
    if (amount >= 1) {
        amount = 1;
        stick_held_ms += dt * 1000.0;
        boost = opt.stick_boost_ms ? 1 + (opt.stick_boost - 1) * fmin(stick_held_ms / opt.stick_boost_ms, 1)
                                   : opt.stick_boost;
    } else {
        stick_held_ms = 0;
    }
    amount = pow(amount, opt.stick_curve) * opt.stick_speed * boost * dt;
    /* along the stick's own direction: no snap to an axis */
    *yaw += (float)(x / mag * amount);
    /* stick up looks up, and the game's pitch grows looking down */
    *pitch += (float)(-y / mag * amount * opt.stick_y_scale * (opt.stick_invert_y ? -1 : 1));
    return 1;
}

/* the player's input step, once a frame: the turn in degrees since the last one */
int fci_input_aim(float *yaw, float *pitch)
{
    LONGLONG now = (LONGLONG)GetTickCount64(), was = aim_seen;
    LONG dx, dy;
    int turned;
    config();
    if (!opt.aim && !opt.stick_aim)
        return 0;
    aim_seen = now;
    dx = InterlockedExchange(&aim_dx, 0);
    dy = InterlockedExchange(&aim_dy, 0);
    if (now - was >= AIM_LIVE_MS) {
        stick_last.QuadPart = 0;
        stick_held_ms = 0;
        return 0; /* resuming: drop what was left over */
    }
    *yaw = (float)(dx * opt.degrees * opt.sens);
    /* screen y grows down, and so does the game's pitch (looking down) */
    *pitch = (float)(dy * opt.degrees * opt.sens * opt.y_scale * (opt.invert_y ? -1 : 1));
    turned = dx || dy;
    if (opt.stick_aim && stick_turn(yaw, pitch))
        turned = 1;
    return turned;
}

static void mouse_motion(int dx, int dy)
{
    if (aim_live()) {
        InterlockedAdd(&aim_dx, dx);
        InterlockedAdd(&aim_dy, dy);
        return;
    }
    AcquireSRWLockExclusive(&motion_lock);
    motion[motion_head % MOTION_RING].t = GetTickCount64();
    motion[motion_head % MOTION_RING].dx = dx;
    motion[motion_head % MOTION_RING].dy = dy;
    motion_head++;
    ReleaseSRWLockExclusive(&motion_lock);
}

void fci_input_mouse_move(int dx, int dy)
{
    if (captured)
        mouse_motion(dx, dy);
}

/* RECOMP_FCI_CONTROL "mouse <dx> <dy>": motion as if from a captured mouse (test drivers) */
void fci_input_mouse_inject(int dx, int dy)
{
    config();
    mouse_motion(dx, dy);
}

int fci_input_capture_enabled(void)
{
    config();
    return opt.capture;
}
void fci_input_set_captured(int on) { InterlockedExchange(&captured, on ? 1 : 0); }
int fci_input_captured(void) { return captured != 0; }

/* ---- the controller state ---- */
static int held(int code)
{
    if (code == IN_WHEELUP || code == IN_WHEELDOWN)
        return (LONGLONG)GetTickCount64() < wheel_until[code - IN_WHEELUP];
    return down[code] != 0;
}

static int control(int c)
{
    for (int k = 0; k < MAX_BINDS; k++)
        if (binds[c][k] >= 0 && held(binds[c][k]))
            return 1;
    return 0;
}

static int16_t to_axis(double v)
{
    if (v > 1)
        v = 1;
    if (v < -1)
        v = -1;
    return (int16_t)lround(v * 32767.0);
}

/* mouse speed over the window, in counts per second, as a stick deflection */
static void mouse_stick(double *x, double *y)
{
    ULONGLONG now = GetTickCount64();
    long sx = 0, sy = 0;
    AcquireSRWLockShared(&motion_lock);
    for (unsigned i = 0; i < MOTION_RING && i < motion_head; i++) {
        unsigned at = (motion_head - 1 - i) % MOTION_RING;
        if (now - motion[at].t > (ULONGLONG)opt.window_ms)
            break;
        sx += motion[at].dx;
        sy += motion[at].dy;
    }
    ReleaseSRWLockShared(&motion_lock);
    {
        double per_s = 1000.0 / opt.window_ms;
        double vx = sx * per_s * opt.sens / 4000.0;
        double vy = -sy * per_s * opt.sens * opt.y_scale / 4000.0; /* screen y grows down, the stick's up */
        double mag = sqrt(vx * vx + vy * vy);
        if (opt.invert_y)
            vy = -vy;
        if (mag > 0) {
            double shaped = pow(mag > 1 ? 1 : mag, opt.curve);
            shaped = opt.deadzone + (1 - opt.deadzone) * shaped; /* a moving mouse is never inside it */
            vx = vx / mag * shaped;
            vy = vy / mag * shaped;
        }
        *x = vx;
        *y = vy;
    }
}

void fci_kbm_state(fci_input_state *s)
{
    static uint32_t packet;
    static ULONGLONG logged;
    double mx = 0, my = 0, lx, ly, len, scale;
    config();
    memset(s, 0, sizeof *s);
    for (int c = C_A; c <= C_RT; c++)
        s->g.analog[c - C_A] = control(c) ? 255 : 0;
    for (int c = C_DPAD_UP; c <= C_RS; c++)
        if (control(c))
            s->g.wButtons |= (uint16_t)(1u << (c - C_DPAD_UP));

    lx = (double)(control(C_STRAFE_RIGHT) - control(C_STRAFE_LEFT));
    ly = (double)(control(C_FORWARD) - control(C_BACKWARD));
    len = sqrt(lx * lx + ly * ly);
    scale = control(C_WALK) ? opt.walk : 1.0;
    if (len > 0) { /* diagonals as far as a stick reaches, not further */
        lx = lx / len * scale;
        ly = ly / len * scale;
    }
    s->g.lx = to_axis(lx);
    s->g.ly = to_axis(ly);

    if (control(C_LOOK_LEFT) || control(C_LOOK_RIGHT) || control(C_LOOK_UP) || control(C_LOOK_DOWN)) {
        mx = (double)(control(C_LOOK_RIGHT) - control(C_LOOK_LEFT));
        my = (double)(control(C_LOOK_UP) - control(C_LOOK_DOWN));
    } else if (!aim_live()) {
        mouse_stick(&mx, &my);
    }
    s->g.rx = to_axis(mx);
    s->g.ry = to_axis(my);
    s->packet = ++packet; /* the title's input layer looks for changes by packet number */

    if (opt.log && GetTickCount64() - logged >= 250) {
        logged = GetTickCount64();
        fprintf(stderr,
                "[FCI] input: buttons %04X analog %02X%02X%02X%02X%02X%02X%02X%02X L %d,%d R %d,%d%s\n",
                s->g.wButtons, s->g.analog[0], s->g.analog[1], s->g.analog[2], s->g.analog[3], s->g.analog[4],
                s->g.analog[5], s->g.analog[6], s->g.analog[7], s->g.lx, s->g.ly, s->g.rx, s->g.ry,
                captured ? " (mouse captured)" : "");
    }
}
