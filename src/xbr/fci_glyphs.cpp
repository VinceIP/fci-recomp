// Button prompts: the font's controller glyphs (the {A} ... {TRIGD} tokens the menus and hints use) drawn
// again as a modern pad's buttons, or as the keys and mouse buttons bound to those controls while mouse and
// keyboard are in use. The font page is recognised by its bytes as it is uploaded; the DXT3 blocks under each
// glyph are decoded, drawn over and encoded again. Guest memory keeps the original page. Evidence: re/notes/PROMPTS.md.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "fci_glyphs.h"

extern "C" {
int fci_input_on_kbm(void);
int fci_vkb_typing(void);
int fci_input_binding(const char* control);
}

namespace fci_glyphs {
namespace {

enum Token {
    G_A, G_B, G_X, G_Y, G_BL, G_WH, G_START, G_BACK,
    G_AS, G_ASL, G_ASG, G_ASI, G_ASS, G_ASR, G_ASD,
    G_DP, G_DPLR, G_DPUD, G_DPU, G_DPD, G_DPL, G_DPR, G_DPLRU, G_DPLRD,
    G_TRIG, G_TRIGL, G_TRIGG, G_TRIGI, G_TRIGS, G_TRIGR, G_TRIGD,
    G_COUNT
};

struct Page {
    uint32_t w, h;
    uint64_t hash;  // FNV-1a of the DXT3 data as the .xbt file holds it
    uint32_t bytes;
};
struct Glyph {
    Token t;
    uint8_t page;
    uint16_t x0, y0, x1, y1;  // pixels, [x0, x1) x [y0, y1)
};
struct Font {
    const char* title;
    Page pages[3];
    Glyph glyphs[G_COUNT];
};

// "Geometr706 BdCn BT Bold 20" (.mft records from 0x8F, 23 bytes each; tokens from the replacement tables at
// Instincts' 0x636AA0 and Evolution's 0x4EE570, both mapping the 31 button tokens to the same characters)
const Font kFonts[] = {
    {"Instincts",
     {{256, 128, 0x42C1CE6EDE6B5699ull, 32768}, {256, 128, 0x4FDB450633C900B0ull, 32768}, {128, 128, 0x6611BAC891211C04ull, 16384}},
     {{G_A, 1, 101, 97, 123, 120},    {G_B, 1, 123, 1, 145, 24},      {G_X, 1, 123, 24, 145, 47},
      {G_Y, 1, 123, 47, 145, 70},     {G_BL, 1, 145, 23, 166, 45},    {G_WH, 1, 145, 1, 166, 23},
      {G_START, 1, 124, 70, 145, 92}, {G_BACK, 1, 144, 92, 165, 114}, {G_AS, 1, 77, 1, 100, 25},
      {G_ASL, 1, 100, 25, 123, 49},   {G_ASG, 1, 77, 49, 100, 73},    {G_ASI, 1, 101, 73, 124, 97},
      {G_ASS, 1, 100, 1, 123, 25},    {G_ASR, 1, 100, 49, 123, 73},   {G_ASD, 1, 77, 25, 100, 49},
      {G_DP, 1, 52, 1, 77, 28},       {G_DPLR, 1, 52, 28, 77, 55},    {G_DPUD, 1, 1, 82, 26, 109},
      {G_DPU, 1, 51, 82, 76, 109},    {G_DPD, 1, 26, 82, 51, 109},    {G_DPL, 1, 52, 55, 77, 81},
      {G_DPR, 1, 76, 81, 101, 107},   {G_DPLRU, 1, 27, 1, 52, 28},    {G_DPLRD, 1, 27, 55, 52, 82},
      {G_TRIG, 1, 166, 1, 185, 27},   {G_TRIGL, 1, 184, 53, 203, 79}, {G_TRIGG, 1, 184, 79, 203, 104},
      {G_TRIGI, 1, 145, 45, 164, 71}, {G_TRIGS, 1, 165, 71, 184, 97}, {G_TRIGR, 1, 165, 97, 184, 123},
      {G_TRIGD, 1, 164, 45, 183, 71}}},
    {"Evolution",
     {{256, 128, 0x846703D68DCAA126ull, 32768}, {256, 128, 0x40649CE039B15AC4ull, 32768}, {256, 128, 0x8B5218E64E38503Full, 32768}},
     {{G_A, 1, 163, 1, 185, 24},      {G_B, 1, 175, 73, 197, 96},     {G_X, 1, 175, 96, 197, 119},
      {G_Y, 1, 177, 24, 199, 47},     {G_BL, 1, 199, 23, 220, 45},    {G_WH, 1, 197, 96, 218, 118},
      {G_START, 1, 185, 1, 206, 23},  {G_BACK, 1, 197, 74, 218, 96},  {G_AS, 1, 129, 80, 152, 104},
      {G_ASL, 1, 152, 104, 175, 128}, {G_ASG, 1, 140, 1, 163, 25},    {G_ASI, 1, 154, 49, 177, 73},
      {G_ASS, 1, 152, 80, 175, 104},  {G_ASR, 1, 154, 25, 177, 49},   {G_ASD, 1, 129, 104, 152, 128},
      {G_DP, 1, 104, 82, 129, 109},   {G_DPLR, 1, 115, 1, 140, 28},   {G_DPUD, 1, 79, 28, 104, 55},
      {G_DPU, 1, 104, 55, 129, 82},   {G_DPD, 1, 79, 55, 104, 82},    {G_DPL, 1, 129, 28, 154, 54},
      {G_DPR, 1, 129, 54, 154, 80},   {G_DPLRU, 1, 79, 82, 104, 109}, {G_DPLRD, 1, 104, 28, 129, 55},
      {G_TRIG, 1, 237, 27, 256, 53},  {G_TRIGL, 1, 237, 79, 256, 105}, {G_TRIGG, 2, 1, 1, 20, 26},
      {G_TRIGI, 1, 177, 47, 196, 73}, {G_TRIGS, 1, 218, 99, 237, 125}, {G_TRIGR, 1, 220, 1, 239, 27},
      {G_TRIGD, 1, 218, 73, 237, 99}}},
};

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// ---- a straight-alpha RGBA picture, drawn with 4 x 4 samples per pixel ----
struct Rgba {
    float r, g, b, a;
};
constexpr int kSs = 4;

struct Canvas {
    int w, h;
    std::vector<Rgba> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_, Rgba{0, 0, 0, 0}) {}
    // source over, with coverage cov in 0..1
    void Over(int x, int y, Rgba c, float cov) {
        if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0) return;
        Rgba& d = px[size_t(y) * w + x];
        float a = c.a * cov, out = a + d.a * (1 - a);
        if (out <= 0) return;
        d.r = (c.r * a + d.r * d.a * (1 - a)) / out;
        d.g = (c.g * a + d.g * d.a * (1 - a)) / out;
        d.b = (c.b * a + d.b * d.a * (1 - a)) / out;
        d.a = out;
    }
    // a shape given by inside(x, y) in pixel units
    template <class F>
    void Fill(Rgba c, F inside) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int n = 0;
                for (int sy = 0; sy < kSs; sy++)
                    for (int sx = 0; sx < kSs; sx++)
                        n += inside(x + (sx + 0.5f) / kSs, y + (sy + 0.5f) / kSs) ? 1 : 0;
                Over(x, y, c, float(n) / (kSs * kSs));
            }
    }
};

Rgba Hex(uint32_t rgb, float a = 1) {
    return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a};
}

bool InRoundRect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    float cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

bool InCircle(float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

bool InTriangle(float x, float y, const float* t) {
    auto side = [&](int a, int b) {
        return (t[b * 2] - t[a * 2]) * (y - t[a * 2 + 1]) - (t[b * 2 + 1] - t[a * 2 + 1]) * (x - t[a * 2]);
    };
    float d0 = side(0, 1), d1 = side(1, 2), d2 = side(2, 0);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

// text through GDI at the sample resolution, centred in the box, shrunk until it fits
void Text(Canvas& c, const wchar_t* s, Rgba col, float x0, float y0, float x1, float y1, const wchar_t* face) {
    int W = c.w * kSs, H = c.h * kSs;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bm || !bits) {
        if (bm) DeleteObject(bm);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bm);
    std::memset(bits, 0, size_t(W) * H * 4);
    int bw = int((x1 - x0) * kSs), bh = int((y1 - y0) * kSs), len = int(wcslen(s));
    HFONT f = nullptr;
    SIZE sz{};
    for (int px = bh; px >= 6; px--) {
        if (f) DeleteObject(f);
        f = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        SelectObject(dc, f);
        GetTextExtentPoint32W(dc, s, len, &sz);
        if (sz.cx <= bw && sz.cy <= bh * 13 / 10) break;
    }
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    // centre the cap height rather than the cell: the cell's descent would push capitals up
    int ink = tm.tmAscent - tm.tmInternalLeading;
    int tx = int(x0 * kSs) + (bw - sz.cx) / 2;
    int ty = int(y0 * kSs) + (bh - ink) / 2 - tm.tmInternalLeading;
    TextOutW(dc, tx, ty, s, len);
    GdiFlush();
    const uint8_t* p = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < c.h; y++)
        for (int x = 0; x < c.w; x++) {
            int sum = 0;
            for (int sy = 0; sy < kSs; sy++)
                for (int sx = 0; sx < kSs; sx++) sum += p[(size_t(y * kSs + sy) * W + x * kSs + sx) * 4 + 1];
            c.Over(x, y, col, sum / (255.0f * kSs * kSs));
        }
    SelectObject(dc, old);
    if (f) DeleteObject(f);
    DeleteObject(bm);
    DeleteDC(dc);
}

const wchar_t* kFace = L"Bahnschrift SemiBold Condensed";
const wchar_t* kSymbols = L"Segoe UI Symbol";
const Rgba kDark = Hex(0x1C1C1C), kRim = Hex(0x6A6A6A), kWhite = Hex(0xFFFFFF), kGrey = Hex(0x8C8C8C);

// ---- a modern pad ----
void FaceButton(Canvas& c, uint32_t colour, const wchar_t* letter) {
    float cx = c.w / 2.0f, cy = c.h / 2.0f, r = std::min(c.w, c.h) / 2.0f - 0.5f;
    c.Fill(kDark, [&](float x, float y) { return InCircle(x, y, cx, cy, r); });
    c.Fill(Hex(colour), [&](float x, float y) { return InCircle(x, y, cx, cy, r - 1.5f); });
    Text(c, letter, kWhite, cx - r * 0.8f, cy - r * 0.78f, cx + r * 0.8f, cy + r * 0.78f, kFace);
}

void Bumper(Canvas& c, const wchar_t* label) {
    float y0 = c.h * 0.2f, y1 = c.h * 0.8f;
    c.Fill(kRim, [&](float x, float y) { return InRoundRect(x, y, 0.5f, y0, c.w - 0.5f, y1, 4); });
    c.Fill(kDark, [&](float x, float y) { return InRoundRect(x, y, 1.5f, y0 + 1, c.w - 1.5f, y1 - 1, 3); });
    Text(c, label, kWhite, 2, y0 + 1.5f, c.w - 2.0f, y1 - 1.5f, kFace);
}

void Trigger(Canvas& c, const wchar_t* label) {
    float x0 = 1, x1 = c.w - 1.0f, y0 = 1, y1 = c.h - 1.0f, top = (x1 - x0) / 2;
    auto shape = [&](float x, float y, float in) {
        float r = top - in;
        if (y < y0 + in + r)  // a rounded top
            return InCircle(x, y, (x0 + x1) / 2, y0 + in + r, r);
        return InRoundRect(x, y, x0 + in, y0 + in, x1 - in, y1 - in, 2);
    };
    c.Fill(kRim, [&](float x, float y) { return shape(x, y, 0); });
    c.Fill(kDark, [&](float x, float y) { return shape(x, y, 1); });
    Text(c, label, kWhite, x0 + 1.5f, c.h * 0.38f, x1 - 1.5f, y1 - 2, kFace);
}

void Stick(Canvas& c, const wchar_t* label) {
    float cx = c.w / 2.0f, cy = c.h / 2.0f, r = std::min(c.w, c.h) / 2.0f - 0.5f;
    c.Fill(kRim, [&](float x, float y) { return InCircle(x, y, cx, cy, r); });
    c.Fill(kDark, [&](float x, float y) { return InCircle(x, y, cx, cy, r - 1.5f); });
    c.Fill(Hex(0x3A3A3A), [&](float x, float y) { return InCircle(x, y, cx, cy, r - 4); });
    Text(c, label, kWhite, cx - r * 0.65f, cy - r * 0.5f, cx + r * 0.65f, cy + r * 0.5f, kFace);
}

void Menu(Canvas& c, bool view) {
    float cx = c.w / 2.0f, cy = c.h / 2.0f, r = std::min(c.w, c.h) / 2.0f - 0.5f;
    c.Fill(kRim, [&](float x, float y) { return InCircle(x, y, cx, cy, r); });
    c.Fill(kDark, [&](float x, float y) { return InCircle(x, y, cx, cy, r - 1.5f); });
    float s = r * 0.42f;
    if (!view) {  // three lines
        for (int i = -1; i <= 1; i++) {
            float ly = cy + i * s * 0.75f;
            c.Fill(kWhite, [&](float x, float y) { return x >= cx - s && x <= cx + s && std::fabs(y - ly) <= 0.75f; });
        }
        return;
    }
    // two overlapping windows
    auto frame = [&](float ox, float oy) {
        return [=](float x, float y) {
            float x0 = cx - s + ox, y0 = cy - s * 0.75f + oy, x1 = x0 + s * 1.3f, y1 = y0 + s * 1.05f;
            bool in = x >= x0 && x <= x1 && y >= y0 && y <= y1;
            bool hole = x > x0 + 1.2f && x < x1 - 1.2f && y > y0 + 1.2f && y < y1 - 1.2f;
            return in && !hole;
        };
    };
    c.Fill(kWhite, frame(0, 0));
    c.Fill(kWhite, frame(s * 0.6f, s * 0.5f));
}

// up, down, left, right lit
void Dpad(Canvas& c, bool u, bool d, bool l, bool r) {
    float cx = c.w / 2.0f, cy = c.h / 2.0f, R = std::min(c.w, c.h) / 2.0f - 0.5f, t = R * 0.36f;
    auto cross = [&](float in) {
        return [=](float x, float y) {
            return InRoundRect(x, y, cx - t + in, cy - R + in, cx + t - in, cy + R - in, 2) ||
                   InRoundRect(x, y, cx - R + in, cy - t + in, cx + R - in, cy + t - in, 2);
        };
    };
    c.Fill(kRim, cross(0));
    c.Fill(kDark, cross(1.2f));
    bool any = u || d || l || r;
    auto arm = [&](bool lit, float dx, float dy) {
        Rgba col = lit ? kWhite : (any ? Hex(0x4A4A4A) : kGrey);
        float ax = cx + dx * R * 0.62f, ay = cy + dy * R * 0.62f, a = t * 0.8f;
        float tri[6] = {ax + dx * a, ay + dy * a, ax - dx * a * 0.4f + dy * a, ay - dy * a * 0.4f + dx * a,
                        ax - dx * a * 0.4f - dy * a, ay - dy * a * 0.4f - dx * a};
        c.Fill(col, [&](float x, float y) { return InTriangle(x, y, tri); });
    };
    arm(u, 0, -1);
    arm(d, 0, 1);
    arm(l, -1, 0);
    arm(r, 1, 0);
}

// ---- mouse and keyboard ----
constexpr int kMouse1 = 256;  // fci_input.c: inputs past the virtual keys
const Rgba kCap = Hex(0xE6E6E6), kCapEdge = Hex(0x8A8A8A), kInk = Hex(0x1A1A1A), kAccent = Hex(0xF0A020);

void Keycap(Canvas& c, const std::wstring& label, const wchar_t* face) {
    float y0 = c.h * 0.12f, y1 = c.h * 0.88f;
    c.Fill(kCapEdge, [&](float x, float y) { return InRoundRect(x, y, 0.5f, y0, c.w - 0.5f, y1, 3); });
    c.Fill(kCap, [&](float x, float y) { return InRoundRect(x, y, 1.5f, y0 + 1, c.w - 1.5f, y1 - 2.5f, 2.5f); });
    Text(c, label.c_str(), kInk, 2.5f, y0 + 2, c.w - 2.5f, y1 - 3.5f, face);
}

// button 0 left, 1 right, 2 middle, 3 wheel up, 4 wheel down, -1 none (movement)
void Mouse(Canvas& c, int button) {
    float x0 = c.w * 0.2f, x1 = c.w * 0.8f, y0 = 0.5f, y1 = c.h - 0.5f, mid = (x0 + x1) / 2, split = c.h * 0.45f;
    float r = (x1 - x0) / 2;
    c.Fill(kCapEdge, [&](float x, float y) { return InRoundRect(x, y, x0, y0, x1, y1, r); });
    c.Fill(kCap, [&](float x, float y) { return InRoundRect(x, y, x0 + 1, y0 + 1, x1 - 1, y1 - 1, r - 1); });
    auto lit = [&](float x, float y) {
        bool in = InRoundRect(x, y, x0 + 1, y0 + 1, x1 - 1, y1 - 1, r - 1) && y < split;
        if (button == 0) return in && x < mid - 0.6f;
        if (button == 1) return in && x > mid + 0.6f;
        return false;
    };
    if (button == 0 || button == 1) c.Fill(kAccent, lit);
    c.Fill(kCapEdge, [&](float x, float y) { return (std::fabs(x - mid) <= 0.6f && y < split && y > y0) ||
                                                    (std::fabs(y - split) <= 0.6f && x > x0 && x < x1); });
    float wy = split * 0.5f + 0.5f;
    c.Fill(button == 2 ? kAccent : kInk, [&](float x, float y) { return InRoundRect(x, y, mid - 1.2f, wy - 2.5f, mid + 1.2f, wy + 2.5f, 1.2f); });
    if (button == 3 || button == 4) {
        float d = button == 3 ? -1 : 1, ay = c.h * 0.72f, a = c.w * 0.16f;
        float tri[6] = {mid, ay + d * a, mid - a, ay - d * a * 0.4f, mid + a, ay - d * a * 0.4f};
        c.Fill(kAccent, [&](float x, float y) { return InTriangle(x, y, tri); });
    }
}

std::wstring KeyLabel(int code, const wchar_t** face) {
    *face = kFace;
    switch (code) {
    case VK_SPACE: return L"SPC";
    case VK_RETURN: return L"ENT";
    case VK_ESCAPE: return L"ESC";
    case VK_TAB: return L"TAB";
    case VK_BACK: *face = kSymbols; return L"⌫";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: *face = kSymbols; return L"⇧";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return L"CTRL";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return L"ALT";
    case VK_CAPITAL: return L"CAPS";
    case VK_UP: *face = kSymbols; return L"↑";
    case VK_DOWN: *face = kSymbols; return L"↓";
    case VK_LEFT: *face = kSymbols; return L"←";
    case VK_RIGHT: *face = kSymbols; return L"→";
    case VK_INSERT: return L"INS";
    case VK_DELETE: return L"DEL";
    case VK_HOME: return L"HOME";
    case VK_END: return L"END";
    case VK_PRIOR: return L"PGUP";
    case VK_NEXT: return L"PGDN";
    }
    if (code >= VK_F1 && code <= VK_F24) return L"F" + std::to_wstring(code - VK_F1 + 1);
    if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) return L"N" + std::to_wstring(code - VK_NUMPAD0);
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::wstring(1, wchar_t(code));
    UINT ch = MapVirtualKeyW(UINT(code), MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (ch) return std::wstring(1, wchar_t(towupper(wchar_t(ch))));
    return L"?";
}

void Key(Canvas& c, int code);

// one control's first binding: a mouse picture or a keycap; false when nothing is bound
bool Bound(Canvas& c, const char* control) {
    int code = fci_input_binding(control);
    if (code < 0) return false;
    if (code >= kMouse1) {
        int b = code - kMouse1;  // MOUSE1-5, WHEELUP, WHEELDOWN
        if (b <= 2 || b >= 5) {
            Mouse(c, b <= 2 ? b : b - 2);
            return true;
        }
        Keycap(c, L"M" + std::to_wstring(b + 1), kFace);
        return true;
    }
    Key(c, code);
    return true;
}

// several controls' first keys on one keycap ("WASD", the arrows)
void Keys(Canvas& c, std::initializer_list<const char*> controls) {
    std::wstring s;
    const wchar_t* face = kFace;
    for (const char* ctl : controls) {
        int code = fci_input_binding(ctl);
        if (code < 0 || code >= kMouse1) continue;
        const wchar_t* f;
        s += KeyLabel(code, &f);
        if (f == kSymbols) face = kSymbols;
    }
    Keycap(c, s.empty() ? L"?" : s, face);
}

void DrawPad(Canvas& c, Token t) {
    switch (t) {
    case G_A: FaceButton(c, 0x2E9E3E, L"A"); break;
    case G_B: FaceButton(c, 0xD8312C, L"B"); break;
    case G_X: FaceButton(c, 0x2A6FD6, L"X"); break;
    case G_Y: FaceButton(c, 0xE0A800, L"Y"); break;
    case G_BL: Bumper(c, L"LB"); break;
    case G_WH: Bumper(c, L"RB"); break;
    case G_START: Menu(c, false); break;
    case G_BACK: Menu(c, true); break;
    case G_AS: case G_ASL: case G_ASG: case G_ASI: case G_ASS: Stick(c, L"L"); break;
    case G_ASR: case G_ASD: Stick(c, L"R"); break;
    case G_DP: Dpad(c, false, false, false, false); break;
    case G_DPLR: Dpad(c, false, false, true, true); break;
    case G_DPUD: Dpad(c, true, true, false, false); break;
    case G_DPU: Dpad(c, true, false, false, false); break;
    case G_DPD: Dpad(c, false, true, false, false); break;
    case G_DPL: Dpad(c, false, false, true, false); break;
    case G_DPR: Dpad(c, false, false, false, true); break;
    case G_DPLRU: Dpad(c, true, false, true, true); break;
    case G_DPLRD: Dpad(c, false, true, true, true); break;
    case G_TRIG: case G_TRIGL: case G_TRIGG: case G_TRIGI: case G_TRIGS: Trigger(c, L"LT"); break;
    case G_TRIGR: case G_TRIGD: Trigger(c, L"RT"); break;
    default: break;
    }
}

// a key's cap
void Key(Canvas& c, int code) {
    const wchar_t* face;
    std::wstring s = KeyLabel(code, &face);
    Keycap(c, s, face);
}

void DrawKeys(Canvas& c, Token t, bool typing) {
    if (typing) {  // the on-screen keyboard takes the keys (fci_vkb.c): these are what press its buttons
        switch (t) {
        case G_A: Key(c, VK_RETURN); return;
        case G_B: Key(c, VK_ESCAPE); return;
        case G_X: Key(c, VK_SPACE); return;
        case G_Y: Key(c, VK_BACK); return;
        default: break;
        }
    }
    bool ok = true;
    switch (t) {
    case G_A: ok = Bound(c, "A"); break;
    case G_B: ok = Bound(c, "B"); break;
    case G_X: ok = Bound(c, "X"); break;
    case G_Y: ok = Bound(c, "Y"); break;
    case G_BL: ok = Bound(c, "BLACK"); break;
    case G_WH: ok = Bound(c, "WHITE"); break;
    case G_START: ok = Bound(c, "START"); break;
    case G_BACK: ok = Bound(c, "BACK"); break;
    case G_AS: case G_ASL: case G_ASG: case G_ASI: case G_ASS:
        Keys(c, {"FORWARD", "STRAFE_LEFT", "BACKWARD", "STRAFE_RIGHT"});
        break;
    case G_ASR: case G_ASD: Mouse(c, -1); break;  // the mouse looks
    case G_DP: Keys(c, {"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"}); break;
    case G_DPLR: Keys(c, {"DPAD_LEFT", "DPAD_RIGHT"}); break;
    case G_DPUD: Keys(c, {"DPAD_UP", "DPAD_DOWN"}); break;
    case G_DPU: ok = Bound(c, "DPAD_UP"); break;
    case G_DPD: ok = Bound(c, "DPAD_DOWN"); break;
    case G_DPL: ok = Bound(c, "DPAD_LEFT"); break;
    case G_DPR: ok = Bound(c, "DPAD_RIGHT"); break;
    case G_DPLRU: Keys(c, {"DPAD_LEFT", "DPAD_RIGHT", "DPAD_UP"}); break;
    case G_DPLRD: Keys(c, {"DPAD_LEFT", "DPAD_RIGHT", "DPAD_DOWN"}); break;
    case G_TRIG: case G_TRIGL: case G_TRIGG: case G_TRIGI: case G_TRIGS: ok = Bound(c, "LT"); break;
    case G_TRIGR: case G_TRIGD: ok = Bound(c, "RT"); break;
    default: break;
    }
    if (!ok) DrawPad(c, t);  // an unbound control still shows what the pad would press
}

// ---- DXT3 (BC2) blocks: 4-bit alpha, then a colour block that is always four-colour, whichever endpoint is
// larger (BC1's three-colour mode does not exist in BC2) ----
void Unpack565(uint16_t v, float* c) {
    c[0] = ((v >> 11) & 31) / 31.0f;
    c[1] = ((v >> 5) & 63) / 63.0f;
    c[2] = (v & 31) / 31.0f;
}

uint16_t Pack565(const float* c) {
    auto q = [](float v, int m) { return uint16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * m)); };
    return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

void DecodeBlock(const uint8_t* b, Rgba* out) {
    float pal[4][3];
    uint16_t c0 = uint16_t(b[8] | b[9] << 8), c1 = uint16_t(b[10] | b[11] << 8);
    Unpack565(c0, pal[0]);
    Unpack565(c1, pal[1]);
    for (int k = 0; k < 3; k++) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    uint32_t idx = uint32_t(b[12] | b[13] << 8 | b[14] << 16 | uint32_t(b[15]) << 24);
    for (int i = 0; i < 16; i++) {
        int a = (b[i / 2] >> ((i & 1) * 4)) & 15, p = (idx >> (i * 2)) & 3;
        out[i] = {pal[p][0], pal[p][1], pal[p][2], a / 15.0f};
    }
}

void EncodeBlock(const Rgba* in, uint8_t* b) {
    std::memset(b, 0, 16);
    for (int i = 0; i < 16; i++) b[i / 2] |= uint8_t(std::lround(std::clamp(in[i].a, 0.0f, 1.0f) * 15) << ((i & 1) * 4));
    // the colour line: the principal axis of the visible texels' colours
    float mean[3] = {}, wsum = 0;
    for (int i = 0; i < 16; i++) {
        float w = in[i].a > 0.02f ? 1.0f : 0.0f;
        mean[0] += in[i].r * w, mean[1] += in[i].g * w, mean[2] += in[i].b * w, wsum += w;
    }
    if (wsum == 0) return;  // fully transparent: colour does not matter
    for (float& m : mean) m /= wsum;
    float cov[6] = {};
    for (int i = 0; i < 16; i++) {
        if (in[i].a <= 0.02f) continue;
        float d[3] = {in[i].r - mean[0], in[i].g - mean[1], in[i].b - mean[2]};
        cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; it++) {
        float v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                      cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                      cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-6f) break;
        for (int k = 0; k < 3; k++) axis[k] = v[k] / n;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; i++) {
        if (in[i].a <= 0.02f) continue;
        float t = (in[i].r - mean[0]) * axis[0] + (in[i].g - mean[1]) * axis[1] + (in[i].b - mean[2]) * axis[2];
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) e0[k] = mean[k] + axis[k] * hi, e1[k] = mean[k] + axis[k] * lo;
    uint16_t c0 = Pack565(e0), c1 = Pack565(e1);
    float pal[4][3];
    Unpack565(c0, pal[0]);
    Unpack565(c1, pal[1]);
    for (int k = 0; k < 3; k++) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        float bd = 1e9f;
        for (int p = 0; p < 4; p++) {
            float dr = in[i].r - pal[p][0], dg = in[i].g - pal[p][1], db = in[i].b - pal[p][2];
            float d = dr * dr + dg * dg + db * db;
            if (d < bd) bd = d, best = p;
        }
        idx |= uint32_t(best) << (i * 2);
    }
    b[8] = uint8_t(c0), b[9] = uint8_t(c0 >> 8), b[10] = uint8_t(c1), b[11] = uint8_t(c1 >> 8);
    b[12] = uint8_t(idx), b[13] = uint8_t(idx >> 8), b[14] = uint8_t(idx >> 16), b[15] = uint8_t(idx >> 24);
}

int Enabled() {
    static int on = -1;
    if (on < 0) {
        const char* e = std::getenv("RECOMP_FCI_PROMPTS");
        on = !(e && *e == '0');
    }
    return on;
}

int Mode() { return fci_input_on_kbm() ? (fci_vkb_typing() ? 2 : 1) : 0; }

int g_mode = -1;                // 0 pad, 1 mouse and keyboard, 2 typing into the on-screen keyboard, as last drawn
std::vector<uint32_t> g_pages;  // guest addresses of the pages drawn over

}  // namespace

bool Patch(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t level, std::vector<uint8_t>& px, uint32_t pitch) {
    if (!Enabled() || level != 0 || d.cube || d.depth > 1 || fi.host != gpu::Host::BC2) return false;
    const Font* font = nullptr;
    int page = -1;
    uint64_t hash = 0;
    uint32_t hashed = 0;  // the bytes hash covers: one hash per upload, whichever pages share its size
    for (const Font& f : kFonts)
        for (int p = 0; p < 3; p++) {
            const Page& pg = f.pages[p];
            if (d.width != pg.w || d.height != pg.h || px.size() < pg.bytes) continue;
            if (hashed != pg.bytes) hash = Fnv(px.data(), pg.bytes), hashed = pg.bytes;
            if (hash == pg.hash) font = &f, page = p;
        }
    if (!font || std::none_of(std::begin(font->glyphs), std::end(font->glyphs), [&](const Glyph& g) { return g.page == page; }))
        return false;
    int mode = Mode();
    if (std::find(g_pages.begin(), g_pages.end(), d.data) == g_pages.end()) {
        g_pages.push_back(d.data);
        std::fprintf(stderr, "[FCI] prompts: %s font page %d at %08X drawn for the %s\n", font->title, page, d.data,
                     mode ? "mouse and keyboard" : "pad");
        std::fflush(stderr);
    }
    g_mode = mode;
    uint32_t bw = (d.width + 3) / 4;
    for (const Glyph& g : font->glyphs) {
        if (g.page != page) continue;
        int w = g.x1 - g.x0, h = g.y1 - g.y0;
        Canvas c(w, h);
        if (mode)
            DrawKeys(c, g.t, mode == 2);
        else
            DrawPad(c, g.t);
        // every block the glyph touches: decoded, the glyph's texels replaced, encoded again
        for (uint32_t by = g.y0 / 4; by <= uint32_t(g.y1 - 1) / 4 && by * 4 < d.height; by++)
            for (uint32_t bx = g.x0 / 4; bx <= uint32_t(g.x1 - 1) / 4 && bx < bw; bx++) {
                uint8_t* b = px.data() + size_t(by) * pitch + bx * 16;
                Rgba t[16];
                DecodeBlock(b, t);
                for (int i = 0; i < 16; i++) {
                    int x = int(bx * 4) + (i & 3), y = int(by * 4) + (i >> 2);
                    if (x >= g.x0 && x < g.x1 && y >= g.y0 && y < g.y1) t[i] = c.px[size_t(y - g.y0) * w + (x - g.x0)];
                }
                EncodeBlock(t, b);
            }
    }
    return true;
}

void Poll() {
    if (!Enabled() || g_pages.empty()) return;
    int mode = Mode();
    if (mode == g_mode) return;
    g_mode = mode;
    for (uint32_t data : g_pages) gpu::MarkDataDirty(data);  // uploaded again, so drawn again for the new device
}

}  // namespace fci_glyphs
