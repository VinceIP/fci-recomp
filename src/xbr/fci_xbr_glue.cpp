// The runtime services Xbox XBE Recomp's D3D8 and GPU code expects (kernel.h, memory.h, app.h, x86_ops.h,
// log.h), implemented on this project's runtime; selected with RECOMP_FCI_XBR=1.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

#include "app/app.h"
#include "fci_gfx.h"
#include "gpu/backend.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xbe/xbe.h"
#include "../fci_image.h"

// ---- this runtime (C) -----------------------------------------------------------------------
extern "C" {
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
ptrdiff_t xbox_GetMemoryOffset(void);
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
int xbox_ContiguousFree(uint32_t addr);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t xbox_va);
void fci_guest_event(uint32_t guest_va, int set);
int fci_guest_host_thread_init(void);
int guest_cpu_interrupt_enter(void);
void guest_cpu_interrupt_leave(int);
void fci_framebuffer_key(int vk, int down);   // keyboard pad, fci_nv2a.c
// mouse and keyboard as the controller (fci_input.c)
void fci_input_key(int code, int down);
void fci_input_release_all(void);
void fci_input_wheel(int notches);
void fci_input_mouse_move(int dx, int dy);
int fci_input_capture_enabled(void);
void fci_input_set_captured(int on);
int fci_input_captured(void);
int fci_guest_set_event_handle(uint32_t token);
int fci_aspect_wide(void);                        // fci_display.c: RECOMP_FCI_ASPECT=16:9
int fci_window_size(uint32_t* w, uint32_t* h);    // fci_display.c: RECOMP_FCI_WINDOW
void fci_log_session_end(void);                   // fci_config.c: a clean exit, for the log
extern const char *fci_xbe_path, *fci_game_dir;   // fci_main.c: from the arguments or fci.ini
}

namespace {
thread_local bool t_host_thread = false;   // StartHostThread: guest calls take the guest CPU
}

// ---- options --------------------------------------------------------------------------------
// RECOMP_* switches (arguments or fci.ini, fci_main.c): "1" is on, any other value off, unset the fallback
static bool OptionFlag(const char* name, bool fallback)
{
    const char* e = std::getenv(name);
    return e && *e ? *e == '1' : fallback;
}

// ---- switch ---------------------------------------------------------------------------------
extern "C" int fci_xbr_enabled(void)
{
    static const int on = OptionFlag("RECOMP_FCI_XBR", false);
    return on;
}

// host DirectSound (xdk/dsound.cpp): with host Direct3D unless RECOMP_FCI_XBR_AUDIO says otherwise; the
// lifted library on the emulated APU otherwise
extern "C" int fci_xbr_audio_enabled(void)
{
    static const int on = OptionFlag("RECOMP_FCI_XBR_AUDIO", fci_xbr_enabled() != 0);
    return on;
}

// The contiguous window reuses freed blocks only with host DirectSound: the lifted DirectSound needs its
// DSP command block at the address its acknowledgement names.
extern "C" int fci_contig_reuse(void) { return fci_xbr_audio_enabled(); }

// ---- log ------------------------------------------------------------------------------------
namespace logx {
void Init(const std::string&, const std::string&, bool) {}
void Write(Level lv, const char* fmt, ...)
{
    if (lv == Level::Debug)
        return;
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "  [XBR] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}
void Flush() { std::fflush(stderr); }
const std::string& Path()
{
    static std::string none;
    return none;
}
}   // namespace logx

// ---- CPU ------------------------------------------------------------------------------------
// the game's code range, used only to pick code addresses out of a stack for a log line (d3d8.cpp)
extern const uint32_t X86CodeBase = 0x00012000u;
extern const uint32_t X86CodeEnd = FCI_CODE_END;

uint8_t* xbr_base() { return reinterpret_cast<uint8_t*>(xbox_GetMemoryOffset()); }

extern "C" void xbr_ctx_load(X86Context& c)
{
    c.eax = g_eax;
    c.ecx = g_ecx;
    c.edx = g_edx;
    c.ebx = g_ebx;
    c.esp = g_esp;
    c.ebp = g_ebp;
    c.esi = g_esi;
    c.edi = g_edi;
}
extern "C" void xbr_ctx_store(const X86Context& c)
{
    g_eax = c.eax;
    g_ecx = c.ecx;
    g_edx = c.edx;
    g_ebx = c.ebx;
    g_esp = c.esp;
    g_ebp = c.ebp;
    g_esi = c.esi;
    g_edi = c.edi;
}

[[noreturn]] void x86_trap(X86Context& ctx, uint32_t va, const char* why)
{
    std::fprintf(stderr, "[FCI] STOP 98 host Direct3D: %s (va=%08X esp=%08X)\n", why, va, ctx.esp);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 98);
    for (;;) {}
}

namespace kernel {
uint8_t* g_base = nullptr;
const xbe::Image* g_image = nullptr;   // the title's XBE, parsed from the runtime's first argument

std::string ReadCString(uint32_t va, size_t max)
{
    std::string s;
    for (size_t i = 0; va && i < max; i++) {
        char c = char(R8(va + uint32_t(i)));
        if (!c)
            break;
        s += c;
    }
    return s;
}

// D:\name or name: under the game folder (the runtime's second argument)
bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out)
{
    if (!fci_game_dir)
        return false;
    std::string rel = xbox_path;
    if (rel.size() > 2 && rel[1] == ':')
        rel = rel.substr(2);
    while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
        rel = rel.substr(1);
    std::ifstream f(std::string(fci_game_dir) + "\\" + rel, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count)
{
    recomp_func_t f = recomp_lookup(fn);
    if (!f)
        f = recomp_lookup_manual(fn);
    if (!f)
        x86_trap(ctx, fn, "guest call to an address with no translation");
    uint32_t esp = ctx.esp;
    xbr_ctx_store(ctx);
    for (size_t i = count; i-- > 0;) {
        g_esp -= 4;
        W32(g_esp, args[i]);
    }
    g_esp -= 4;
    W32(g_esp, 0xFE0F0000u);   // a return address no code has: the lifted ret pops it
    int cpu = t_host_thread ? guest_cpu_interrupt_enter() : 0;
    f();
    if (t_host_thread)
        guest_cpu_interrupt_leave(cpu);
    xbr_ctx_load(ctx);
    ctx.esp = esp;
    return ctx.eax;
}

ObjectRef FromPointer(uint32_t guest)
{
    auto o = std::make_shared<Object>();
    o->guest = guest;
    return o;
}
ObjectRef FromHandle(uint32_t handle)
{
    if (!handle)
        return nullptr;
    auto o = std::make_shared<Object>();
    o->handle = handle;
    return o;
}
int32_t SetEvent(Object& o)
{
    if (o.handle)
        fci_guest_set_event_handle(o.handle);
    else
        fci_guest_event(o.guest, 1);
    return 0;
}
int32_t ResetEvent(Object& o)
{
    fci_guest_event(o.guest, 0);
    return 0;
}

void StartHostThread(const char* name, std::function<void(X86Context&)> fn)
{
    std::thread([name, fn] {
        if (!fci_guest_host_thread_init()) {
            LOGE("host thread %s: no worker stack or TIB", name);
            return;
        }
        t_host_thread = true;
        X86Context ctx{};
        xbr_ctx_load(ctx);
        fn(ctx);
    }).detach();
}

void SetSavedDataAddress(uint32_t) {}
void SavePersistedDisplay(uint32_t, uint32_t, uint32_t, const void*, uint32_t)
{ LOG_FIRST(1, "PersistDisplay: the display is not carried across a reboot"); }
}   // namespace kernel

// ---- memory ---------------------------------------------------------------------------------
namespace mem {
uint32_t AllocContiguous(uint32_t size, uint32_t, uint32_t, uint32_t align)
{ return xbox_ContiguousAlloc(size, align < kPage ? kPage : align); }
// gives the block back to the contiguous window (reused once the renderer is past it)
void FreeContiguous(uint32_t va)
{
    if (va)
        xbox_ContiguousFree(va);
}
uint32_t AllocSystem(uint32_t size, uint32_t align) { return xbox_HeapAlloc(size, align); }
void FreeSystem(uint32_t va) { xbox_HeapFree(va); }
uint32_t PhysicalFromVirtual(uint32_t va)
{
    if (va >= 0x80000000u)
        return va & 0x0FFFFFFFu;               // contiguous window
    return 0x08000000u | (va & 0x03FFFFFFu);   // main RAM, tagged as MmGetPhysicalAddress does
}
uint32_t VirtualFromPhysical(uint32_t pa)
{
    if ((pa & 0x0C000000u) == 0x08000000u)
        return pa & 0x03FFFFFFu;
    return kContiguousBase | (pa & 0x0FFFFFFFu);
}
}   // namespace mem

// ---- app: window, settings ------------------------------------------------------------------
namespace app {
namespace {
// The window survives the game's reboots, which start a new process: the first instance makes the frame
// window, and each instance draws into a child render window of its own (RECOMP_FCI_FRAME, inherited).
HWND g_window = nullptr;   // this instance's render window
HWND g_frame = nullptr;    // the frame: this process's own or the first instance's
bool g_own_frame = false;
DWORD g_window_thread = 0;
bool g_alt_enter = true;                  // RECOMP_FCI_ALT_ENTER=0: no fullscreen toggle
constexpr UINT kMsgToggle = WM_APP + 1;   // render window to frame: Alt+Enter
constexpr UINT kMsgLeave = WM_APP + 2;    // fci_window_handoff: the render window goes
constexpr int kClientW = 1280, kClientH = 960;

bool Option(const char* name, bool fallback) { return OptionFlag(name, fallback); }
std::atomic<bool> g_leaving{false};   // the render window went to the next instance (fci_window_handoff)

// borderless fullscreen on the frame's monitor; the present keeps the picture's aspect inside it (d3d12.cpp letterboxes)
struct {
    bool on = false;
    WINDOWPLACEMENT place{sizeof(WINDOWPLACEMENT)};
    LONG style = 0;
} g_full;

void ToggleFullscreen(HWND f)
{
    if (!g_full.on) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(f, &g_full.place)
            || !GetMonitorInfoW(MonitorFromWindow(f, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        g_full.style = GetWindowLongW(f, GWL_STYLE);
        SetWindowLongW(f, GWL_STYLE, (g_full.style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(f, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_full.on = true;
    } else {
        SetWindowLongW(f, GWL_STYLE, g_full.style);
        SetWindowPlacement(f, &g_full.place);
        SetWindowPos(f, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full.on = false;
    }
}

BOOL CALLBACK FitChild(HWND c, LPARAM lp)
{
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    // asynchronous: the render window may belong to another instance
    SetWindowPos(c, nullptr, 0, 0, r->right, r->bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    return TRUE;
}

BOOL CALLBACK FocusChild(HWND c, LPARAM)
{
    SetFocus(c);
    return FALSE;
}

// the frame: window management only; it runs on while its instance is frozen and never touches the guest
LRESULT CALLBACK FrameProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT r;
        GetClientRect(w, &r);
        EnumChildWindows(w, FitChild, reinterpret_cast<LPARAM>(&r));
        return 0;
    }
    case WM_SETFOCUS:
        EnumChildWindows(w, FocusChild, 0);
        return 0;
    case kMsgToggle:
        ToggleFullscreen(w);
        return 0;
    case WM_CLOSE:   // later instances are in jobs that die with this process
        fci_log_session_end();
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// ---- mouse and keyboard (fci_input.c) ----
constexpr int kInMouse1 = 256;   // fci_input.c: inputs past the virtual keys
constexpr UINT_PTR kCaptureTimer = 1;

// the left/right key a Shift, Ctrl or Alt message stands for
int SpecificKey(WPARAM vk, LPARAM lp)
{
    UINT scan = (UINT(lp) >> 16) & 0xFF;
    bool extended = (lp >> 24) & 1;
    switch (vk) {
    case VK_SHIFT:
        return int(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
    case VK_CONTROL:
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return extended ? VK_RMENU : VK_LMENU;
    }
    return int(vk);
}

// the mouse held in the window, cursor hidden: only while the game is in front
void Capture(HWND w, bool on)
{
    if (on && !fci_input_capture_enabled())
        return;
    fci_input_set_captured(on);
    if (on) {
        RECT r;
        GetClientRect(w, &r);
        MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
    }
}

bool InFront(HWND w) { return GetForegroundWindow() == GetAncestor(w, GA_ROOT); }

LRESULT CALLBACK WindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
        fci_framebuffer_key(int(wp), msg == WM_KEYDOWN);
        fci_input_key(SpecificKey(wp, lp), msg == WM_KEYDOWN);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (wp == VK_RETURN && (lp & (1 << 29))) {   // Alt+Enter
            if (msg == WM_SYSKEYDOWN && g_alt_enter && !(lp & (1 << 30)))
                PostMessageW(g_frame, kMsgToggle, 0, 0);
            return 0;
        }
        fci_framebuffer_key(int(wp), msg == WM_SYSKEYDOWN);
        fci_input_key(SpecificKey(wp, lp), msg == WM_SYSKEYDOWN);
        if (wp == VK_F10 || wp == VK_MENU)
            return 0;   // no menu mode: Alt and F10 can be bound
        break;          // Alt+F4 and the rest stay with the system
    case WM_SYSCHAR:
        if (wp == '\r')
            return 0;   // no beep for Alt+Enter
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (GetFocus() != w)
            SetFocus(w);   // a focus change releases every held key
        if (!fci_input_captured() && fci_input_capture_enabled()) {
            Capture(w, true);   // the click that captures is not a press
            return 0;
        }
        [[fallthrough]];
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        bool press = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN
                     || msg == WM_XBUTTONDOWN;
        int button = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP   ? 0
                     : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? 1
                     : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ? 2
                     : GET_XBUTTON_WPARAM(wp) == XBUTTON1           ? 3
                                                                    : 4;
        fci_input_key(kInMouse1 + button, press);
        if (press)
            SetCapture(w);
        else if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
            ReleaseCapture();
        return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_MOUSEWHEEL:
        fci_input_wheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (fci_input_captured() && InFront(w)
            && GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER))
                       != UINT(-1)
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            fci_input_mouse_move(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
        break;   // DefWindowProc cleans up the raw input
    }
    case WM_SETCURSOR:
        if (fci_input_captured() && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wp == kCaptureTimer && fci_input_captured())
            Capture(w, InFront(w));   // follows moves and resizes
        return 0;
    case WM_KILLFOCUS:
        for (int vk = 0; vk < 256; vk++)
            fci_framebuffer_key(vk, 0);
        fci_input_release_all();
        if (fci_input_captured())
            Capture(w, false);
        break;
    case kMsgLeave:
        if (fci_input_captured())
            Capture(w, false);   // the next instance captures on its own first click
        DestroyWindow(w);
        g_window = nullptr;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
}   // namespace

const GameInfo kGame{nullptr};

// Graphics options (fci_gfx.h) are read again when they change. RECOMP_FCI_XBR_NO_PRESENT_WAIT=1 (tests)
// drops a frame the compositor cannot take now: with the display asleep it would wait hundreds of ms.
Settings& Get()
{
    static Settings s;
    static std::atomic<uint32_t> gen{0};
    uint32_t now = fci_gfx::Generation();
    if (gen.load(std::memory_order_relaxed) != now) {
        int n = int(fci_gfx::Number("RECOMP_FCI_XBR_ANISO", 0));
        s.aniso = n >= 2 ? uint32_t(std::min(n, 16)) : 0;
        s.present_never_waits = Option("RECOMP_FCI_XBR_NO_PRESENT_WAIT", false);
        gen.store(now, std::memory_order_relaxed);
    }
    return s;
}

// the title asked for a widescreen display (CreateDevice: D3DPRESENTFLAG_WIDESCREEN, which FCI sets when
// RECOMP_FCI_ASPECT=16:9 reaches it through XGetVideoFlags, fci_display.c): its 4:3 buffer is shown at 16:9
std::atomic<bool> g_widescreen{false};
void SetWidescreen(bool on) { g_widescreen = on; }
bool Widescreen() { return g_widescreen; }

// The render scale per axis: RECOMP_FCI_XBR_SCALE=auto follows the shown picture's size, <n> (0.25 to 4)
// is n times the console's lines with the columns the aspect needs; above the window size it supersamples.
void RenderScale(uint32_t guest_w, uint32_t guest_h, float& sx, float& sy)
{
    static float fixed = 1.0f;   // 0: auto
    static uint32_t gen = 0;
    if (gen != fci_gfx::Generation()) {
        gen = fci_gfx::Generation();
        std::string e = fci_gfx::Value("RECOMP_FCI_XBR_SCALE");
        float v = e.empty() ? 1.0f : float(std::atof(e.c_str()));
        fixed = _stricmp(e.c_str(), "auto") == 0 ? 0.0f : v > 0.25f && v <= 4.0f ? v : 1.0f;
    }
    if (!guest_w || !guest_h) {
        sx = sy = 1;
        return;
    }
    float aspect = Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (fixed == 0) {
        static float last_w = 0;
        RECT c;
        if (g_window && GetClientRect(g_window, &c) && c.right > 0 && c.bottom > 0)
            last_w = std::min(float(c.right), float(c.bottom) * aspect);
        if (last_w > 0) {
            float pw = std::round(last_w), ph = std::round(last_w / aspect);
            sx = std::clamp(pw / float(guest_w), 0.25f, 8.0f);
            sy = std::clamp(ph / float(guest_h), 0.25f, 8.0f);
            return;
        }
    }
    float n = fixed > 0 ? fixed : 1.0f;
    sy = n;
    sx = n * aspect / (float(guest_w) / float(guest_h));
}
std::filesystem::path ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}
// the option holds the folder in the process's code page (fci_config.c expands %APPDATA% with an "A" call);
// a name it cannot hold comes out unusable, and then there is no cache, never an exception
std::filesystem::path ShaderCacheDir()
{
    std::string v = fci_gfx::Value("RECOMP_FCI_XBR_SHADER_CACHE");
    wchar_t w[MAX_PATH];
    int n = v.empty() ? 0 : MultiByteToWideChar(CP_ACP, 0, v.c_str(), -1, w, MAX_PATH);
    return n > 0 ? std::filesystem::path(w) : std::filesystem::path();
}

// the render window, in the frame (1280x960 client), on a thread of its own that pumps its messages
void* Window()
{
    static std::once_flag once;
    std::call_once(once, [] {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([ready] {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSW fc{};
            fc.lpfnWndProc = FrameProc;
            fc.hInstance = inst;
            fc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
            fc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            fc.lpszClassName = L"FCIWholeFrame";
            RegisterClassW(&fc);
            WNDCLASSW wc{};
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = inst;
            wc.hCursor = fc.hCursor;
            wc.lpszClassName = L"FCIWholeD3D12";
            RegisterClassW(&wc);
            g_alt_enter = Option("RECOMP_FCI_ALT_ENTER", true);
            // RECOMP_FCI_XBR_BACKGROUND=1 (test runs): shown without taking the focus from
            // whatever window the user is typing into
            bool background = Option("RECOMP_FCI_XBR_BACKGROUND", false);
            bool editor = fci_xbe_path && std::strstr(fci_xbe_path, "ige.xbe");
            const wchar_t* title = editor ? L"" FCI_TITLE_NAME " Map Editor (recompiled)"
                                          : L"" FCI_TITLE_NAME " (recompiled)";
            const char* fe = std::getenv("RECOMP_FCI_FRAME");
            HWND frame = fe ? reinterpret_cast<HWND>(uintptr_t(std::strtoull(fe, nullptr, 16))) : nullptr;
            if (frame && IsWindow(frame)) {
                SetWindowTextW(frame, title);
            } else {
                // RECOMP_FCI_WINDOW (fci.ini display-dimensions), else the largest window of the aspect
                // that fits the screen's work area; never larger than the work area
                RECT work{0, 0, 1920, 1080};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                uint32_t cw = 0, ch = 0;
                if (!fci_window_size(&cw, &ch)) {
                    ch = uint32_t(work.bottom - work.top);
                    cw = fci_aspect_wide() ? ch * 16 / 9 : ch * 4 / 3;
                }
                float fit = std::min({1.0f, float(work.right - work.left - 32) / cw,
                                      float(work.bottom - work.top - 64) / ch});
                RECT r{0, 0, LONG(cw * fit), LONG(ch * fit)};
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                frame = CreateWindowExW(background ? WS_EX_NOACTIVATE : 0, fc.lpszClassName, title,
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
                g_own_frame = true;
                char hex[32];
                std::snprintf(hex, sizeof(hex), "%llX", static_cast<unsigned long long>(uintptr_t(frame)));
                SetEnvironmentVariableA("RECOMP_FCI_FRAME", hex);   // later instances draw in it
                ShowWindow(frame, background ? SW_SHOWNOACTIVATE : SW_SHOW);
                if (Option("RECOMP_FCI_FULLSCREEN", false))
                    ToggleFullscreen(frame);
            }
            g_frame = frame;
            RECT c;
            GetClientRect(frame, &c);
            g_window = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0,
                                       0, c.right, c.bottom, frame, nullptr, inst, nullptr);
            if (!background)
                SetFocus(g_window);
            {   // raw mouse motion, delivered while the game is in front (checked per message)
                RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_INPUTSINK, g_window};
                RegisterRawInputDevices(&rid, 1, sizeof(rid));
                SetTimer(g_window, kCaptureTimer, 100, nullptr);
            }
            g_window_thread = GetCurrentThreadId();
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }).detach();
        WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
    });
    return g_window;
}

void SplashFirstFrame() {}
bool HandingOver() { return false; }
bool Leaving() { return g_leaving.load(); }
void FinishHandover() {}
std::recursive_mutex& UiLock()
{
    static std::recursive_mutex m;
    return m;
}
// The game asks for every second vertical blank (30 frames/s; its Bink background advances per swap):
// Pace waits out the period after a present, without the guest CPU. RECOMP_FCI_XBR_FPS=<n> paces at n
// frames a second instead (re/notes/FRAME-RATE.md); RECOMP_FCI_XBR_UNLOCK=1 does not pace at all.
static uint32_t g_interval = 1;
void SetPresentInterval(uint32_t interval) { g_interval = interval; }
void Pace()
{
    static int unlock = -1;
    static double fps;
    static LARGE_INTEGER freq, next;
    if (unlock < 0) {
        unlock = OptionFlag("RECOMP_FCI_XBR_UNLOCK", false);
        const char* f = std::getenv("RECOMP_FCI_XBR_FPS");
        fps = f ? std::atof(f) : 0.0;
        QueryPerformanceFrequency(&freq);
    }
    uint32_t blanks = g_interval & 0xF;
    if (unlock || (fps <= 0 && blanks < 2))
        return;   // 0 and 1 blanks: the display's own refresh paces it
    LONGLONG period = fps > 0 ? LONGLONG(double(freq.QuadPart) / fps)
                              : freq.QuadPart * blanks * 1001 / 60000;   // NTSC blanks
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart > next.QuadPart + period) {
        next.QuadPart = now.QuadPart + period;   // late or first: shown at once, the period counts from here
        return;
    }
    if (now.QuadPart < next.QuadPart) {
        kernel::GuestCpuFree free;
        for (;;) {
            QueryPerformanceCounter(&now);
            LONGLONG left = next.QuadPart - now.QuadPart;
            if (left <= 0)
                break;
            DWORD ms = DWORD(left * 1000 / freq.QuadPart);
            if (ms > 1)
                Sleep(ms - 1);
            else
                SwitchToThread();
        }
    }
    next.QuadPart += period;
}
}   // namespace app

// Before this instance freezes for a relaunch: its render window goes, so the frame shows black until the
// next instance draws. Returns the thread to keep running, the frame and whether this instance made it.
extern "C" unsigned long fci_window_handoff(void** frame, int* own)
{
    {   // the presenter holds this lock for a whole present (d3d12.cpp): none is half done after it
        std::lock_guard lock(app::UiLock());
        app::g_leaving = true;
    }
    if (app::g_window)
        SendMessageW(app::g_window, app::kMsgLeave, 0, 0);
    if (frame)
        *frame = app::g_frame;
    if (own)
        *own = app::g_own_frame;
    return app::g_window_thread;
}

// The frame window and the thread that runs it, for a notice shown over the game (fci_manual.c)
extern "C" void* fci_frame_window(unsigned long* thread)
{
    if (thread)
        *thread = app::g_window_thread;
    return app::g_frame;
}

// ---- start-up -------------------------------------------------------------------------------
namespace dispatch {
// whether the address has a lifted function (the DirectSound export audit asks)
X86Func* Find(uint32_t guest)
{
    static X86Func* const lifted = [](X86Context&, uint8_t*) {};
    return recomp_lookup(guest) ? lifted : nullptr;
}
}   // namespace dispatch

extern "C" void fci_xbr_init(void)
{
    kernel::g_base = xbr_base();
    if (fci_xbr_audio_enabled() && fci_xbe_path) {
        static xbe::Image image;
        std::ifstream f(fci_xbe_path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string error;
        if (image.Parse(std::move(data), error))
            kernel::g_image = &image;
        else
            LOGE("xbe: %s cannot be read (%s); host DirectSound has no image sections", fci_xbe_path,
                 error.c_str());
        std::fprintf(stderr, "[FCI] host DirectSound (Xbox XBE Recomp) on\n");
    }
    if (!fci_xbr_enabled())
        return;
    std::fprintf(stderr, "[FCI] host Direct3D 12 (Xbox XBE Recomp D3D8 layer) on: RECOMP_FCI_XBR=1\n");
    // RECOMP_FCI_XBR_CAPTURE=<s>[,<s>...]: capture the frame presented at each of those seconds
    // after start (every draw, each target and the final frame, under XBE_CAPTURE_DIR)
    if (const char* spec = std::getenv("RECOMP_FCI_XBR_CAPTURE")) {
        std::string times = spec;
        std::thread([times] {
            auto start = std::chrono::steady_clock::now();
            size_t pos = 0;
            while (pos < times.size()) {
                int s = std::atoi(times.c_str() + pos);
                std::this_thread::sleep_until(start + std::chrono::seconds(s));
                gpu::g_capture = true;
                pos = times.find(',', pos);
                pos = pos == std::string::npos ? times.size() : pos + 1;
            }
        }).detach();
    }
    // with XBE_CAPTURE_DIR set, F3 captures the next frame (the host window has no key handler for it)
    if (std::getenv("XBE_CAPTURE_DIR")) {
        std::thread([] {
            for (bool was = false;;) {
                bool down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
                if (down && !was)
                    gpu::g_capture = true;
                was = down;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }
    // capture requests appended to a file while the game runs, like RECOMP_PAD_LIVE
    if (const char* spec = std::getenv("RECOMP_FCI_XBR_CAPTURE_LIVE")) {
        std::string path = spec;
        std::thread([path] {
            std::streamoff consumed = 0;
            for (;;) {
                std::ifstream file(path, std::ios::binary);
                if (file) {
                    file.seekg(consumed);
                    std::string line;
                    bool capture = false;
                    while (std::getline(file, line)) {
                        consumed += std::streamoff(line.size() + 1);
                        capture |= !line.empty();
                    }
                    if (capture)
                        gpu::g_capture = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    }
}
