// FCI: quick save and load post the game's own "SaveGame checkpoint=" / "LoadGame" commands, as its level
// scripts do, at the frame boundary on the game thread; Refusal() lists when it will not
#include "fci_quicksave.h"

#include <cstdio>
#include <cstring>

#include "kernel/kernel.h"
#include "xdk/hle.h"

extern "C" int fci_input_quicksave_supported(void);  // fci_input.c

#ifdef _WIN32
#include <windows.h>
#endif

namespace fci_quicksave {
namespace {

// The game object the commands post to, and the command path. These are Instincts' addresses;
// Evolution's differ (data/evolution/) and are not wired up here: the title check below refuses.
constexpr uint32_t kObject = 0x6B3984;
// The command path: the in-game console's post function. edx = the command text, the object pushed
// on the stack, a ret 4 call (the callee skips the object).
constexpr uint32_t kPost = 0x00012000;
// The command texts, as the game's own console spells them (q62-checkpoints.md).
constexpr const char* kSaveText = "SaveGame checkpoint=Quick";
constexpr const char* kLoadText = "LoadGame";
// The multiplayer test (sub_00013760, recomp_0000.c:6007): byte +0x329 != 0 OR dword +0x60 > 1.
constexpr uint32_t kNetFlag = 0x329;
constexpr uint32_t kPlayers = 0x60;
constexpr uint32_t kLevel = 0x24;  // the level object (null-checked by LoadGame)
// The notice expiry, in milliseconds.
constexpr unsigned long long kNoticeMs = 2000;

// ---- host-side notice state ----
// The last message and its expiry (milliseconds since boot). Drawn on every Swap until it expires.
char notice_msg[64] = {};
unsigned long long notice_expiry = 0;

unsigned long long NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

void SetNotice(const char* msg) {
    std::snprintf(notice_msg, sizeof notice_msg, "%s", msg);
    notice_expiry = NowMs() + kNoticeMs;
}

// ---- a 5x7 bitmap font, for the on-screen notice ----
// The GDI text renderer in fci_glyphs.cpp draws to a Canvas (its own buffer type) and is not
// exported; a small bitmap font is simpler and has no GDI dependency.
struct Glyph {
    char c;
    uint8_t r[7];
};
constexpr Glyph kFont[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
};

const Glyph* GlyphOf(char c) {
    for (const Glyph& g : kFont)
        if (g.c == c) return &g;
    return nullptr;
}

inline void SetPx(uint32_t* frame, uint32_t pitch, int x, int y, uint32_t bgr) {
    frame[size_t(y) * (pitch / 4) + x] = bgr;
}

void DrawGlyph(uint32_t* frame, uint32_t pitch, int x, int y, const Glyph& g, uint32_t ink, uint32_t rim) {
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 5; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || py < 0) continue;
            if (dx >= 0 && dx < 5 && dy >= 0 && dy < 7) {
                if ((g.r[dy] >> (4 - dx)) & 1) SetPx(frame, pitch, px, py, ink);
            } else {
                SetPx(frame, pitch, px, py, rim);
            }
        }
    }
}

void DrawText(uint32_t* frame, uint32_t pitch, int cx, int y, const char* s, uint32_t ink, uint32_t rim) {
    int len = int(strlen(s));
    int x = cx - (len * 6 - 1) / 2;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = char(c - 32);
        if (const Glyph* g = GlyphOf(c)) DrawGlyph(frame, pitch, x, y, *g, ink, rim);
        x += 6;
    }
}

// the refusal reason into the buffer, or null when the command may go
const char* Refusal(uint32_t object, int is_load, char* buf, size_t n) {
    const char* verb = is_load ? "quick load" : "quick save";
    const char* why = nullptr;
    if (!fci_input_quicksave_supported())
        why = "not supported for this image";
    else if (object == 0)
        why = "no game object";
    // sub_00013760 (recomp_0000.c:6007): the game's "network or multi-player game" test.
    // Returns 1 when byte object+0x329 != 0 OR dword object+0x60 > 1.
    else if (kernel::R8(object + kNetFlag) != 0 || kernel::R32(object + kPlayers) > 1)
        why = "multiplayer";
    // the level object the game's own LoadGame null-checks (sub_0004C260 at 0004CC23); whether menus clear
    // it is unverified, so every post logs it
    else if (kernel::R32(object + kLevel) == 0)
        why = "no level";
    if (!why) return nullptr;
    std::snprintf(buf, n, "%s refused: %s", verb, why);
    return buf;
}

}  // namespace

void DrawNotice(uint32_t* frame, uint32_t w, uint32_t h, uint32_t pitch) {
    if (!frame || w < 64 || h < 24 || !notice_msg[0]) return;
    if (NowMs() >= notice_expiry) {
        notice_msg[0] = 0;
        return;
    }
    DrawText(frame, pitch, int(w) / 2, int(h) / 8, notice_msg, 0xFFFFFF, 0x101010);
}

void Execute(X86Context& ctx, int is_load, uint32_t* frame, uint32_t w, uint32_t h, uint32_t pitch) {
    uint32_t object = kernel::R32(kObject);
    char reason[96];
    if (const char* r = Refusal(object, is_load, reason, sizeof reason)) {
        std::fprintf(stderr, "[FCI] %s\n", r);
        std::fflush(stderr);
        SetNotice(is_load ? "QUICK LOAD REFUSED" : "QUICK SAVE REFUSED");
        DrawNotice(frame, w, h, pitch);
        return;
    }

    const char* text = is_load ? kLoadText : kSaveText;
    // the text sits in 0x100 bytes reserved below the old esp: the callees' frames grow down from the new
    // esp, under the text, and the saved context puts esp back
    std::fprintf(stderr, "[FCI] %s: posting \"%s\" to %08X (level %08X)\n", is_load ? "quick load" : "quick save",
                 text, object, kernel::R32(object + kLevel));
    std::fflush(stderr);
    X86Context saved = ctx;
    ctx.esp -= 0x100;
    uint32_t at = ctx.esp;
    size_t n = std::strlen(text);
    for (size_t i = 0; i <= n; i++) kernel::W8(at + i, i < n ? text[i] : 0);

    // The whole context is saved and restored: the save and load run the game's own code (the command
    // dispatcher, the file layer), and none of their register or FPU side effects may reach the frame
    // that follows. hle::Call places the text in edx and the object on the stack (a ret 4 call).
    constexpr hle::Map map = hle::Make({hle::edx, hle::stack});
    hle::Call(ctx, kPost, map, {at, object});
    ctx = saved;

    SetNotice(is_load ? "QUICK LOAD" : "QUICK SAVE");
    DrawNotice(frame, w, h, pitch);
}

}  // namespace fci_quicksave
