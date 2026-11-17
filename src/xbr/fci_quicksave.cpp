// FCI: quick save and load post the game's own "SaveGame checkpoint=" / "StartLevel" commands, as its
// Checkpoint trigger and Story Resume do, at the frame boundary on the game thread; Refusal() lists when it will not
#include "fci_quicksave.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "kernel/kernel.h"
#include "xdk/hle.h"

extern "C" int fci_input_quicksave_supported(void);  // fci_input.c
extern "C" const char* fci_save_dir;                 // fci_main.c

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
// The commands, as the game spells them: the save as a Checkpoint trigger's Game:SaveCheckpoint, named per
// level (Quick_<level>) so a load never restores another level's state; the load as Story Resume (format
// 0x6095A0, sub_001509A0), which restores the level, the player's place and facing from the save.
// RestartStartLevel (0x600150) restores the state but spawns the player at the level start.
constexpr const char* kSaveFormat = "SaveGame checkpoint=Quick_%s";
constexpr const char* kLoadFormat = "StartLevel level=%s mission=%s checkpoint=Quick_%s";
constexpr uint32_t kLevelName = 0x2A4;    // the game object's level and mission names (strings), as
constexpr uint32_t kMissionName = 0x2C0;  // sub_0004D010 sets them when a level starts
// The multiplayer test (sub_00013760, recomp_0000.c:6007): byte +0x329 != 0 OR dword +0x60 > 1.
constexpr uint32_t kNetFlag = 0x329;
constexpr uint32_t kPlayers = 0x60;
constexpr uint32_t kLevel = 0x24;  // the level object (null-checked by LoadGame)
// The notice expiry, in milliseconds.
constexpr unsigned long long kNoticeMs = 2000;

// ---- the notice ----
// The last message and its expiry (milliseconds since boot): set on the game thread, read by the
// renderer's present (fci_enhance.cpp draws it over the picture).
std::mutex notice_mu;
std::string notice_msg;
unsigned long long notice_expiry = 0;

unsigned long long NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

void SetNotice(const char* msg) {
    std::lock_guard lock(notice_mu);
    notice_msg = msg;
    notice_expiry = NowMs() + kNoticeMs;
}

// ---- a 5x7 bitmap font, for the on-screen notice ----
// The renderer draws it with a pixel shader from these bits; the GDI glyph code (fci_glyphs.cpp) only
// patches the game's font textures.
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

// a guest string object (+4 the text, inline while the capacity +0x18 is under 16, +0x14 the length):
// empty unless it is a plain name, so nothing odd reaches a command line or a file name
std::string GuestName(uint32_t at) {
    uint32_t len = kernel::R32(at + 0x14), cap = kernel::R32(at + 0x18);
    if (len == 0 || len > 40 || cap < len) return {};
    uint32_t data = cap < 16 ? at + 4 : kernel::R32(at + 4);
    std::string out;
    for (uint32_t i = 0; i < len; i++) {
        char c = char(kernel::R8(data + i));
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return {};
        out += c;
    }
    return out;
}

// a Quick_<level>.fcs in any of the title's profile folders (UDATA\55530008\<profile>)
bool HaveQuickSave(const std::string& level) {
    std::error_code ec;
    std::filesystem::path udata = std::filesystem::path(fci_save_dir ? fci_save_dir : "") / "UDATA" / "55530008";
    for (const auto& e : std::filesystem::directory_iterator(udata, ec))
        if (e.is_directory(ec) && std::filesystem::exists(e.path() / ("Quick_" + level + ".fcs"), ec)) return true;
    return false;
}

// the refusal reason into the buffer, or null when the command may go
// player 0 alive, the test a Checkpoint trigger makes before it saves (Script:IsAlive, sub_00205290): the
// entity from sub_0004CD60 (eax the game object, ecx the player), then sub_00020390 with esi the entity
bool PlayerAlive(X86Context& ctx, uint32_t object) {
    X86Context saved = ctx;
    uint32_t ent = hle::Call(ctx, 0x0004CD60, hle::Make({hle::eax, hle::ecx}), {object, 0});
    bool alive = ent && (hle::Call(ctx, 0x00020390, hle::Make({hle::esi}), {ent}) & 0xFF) != 0;
    ctx = saved;
    return alive;
}

const char* Refusal(X86Context& ctx, uint32_t object, int is_load, char* buf, size_t n) {
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
    else if (GuestName(object + kLevelName).empty() || GuestName(object + kMissionName).empty())
        why = "no level name";
    else if (!is_load && !PlayerAlive(ctx, object))
        why = "the player is not alive";
    else if (is_load && !HaveQuickSave(GuestName(object + kLevelName)))
        why = "no quick save for this level";
    if (!why) return nullptr;
    std::snprintf(buf, n, "%s refused: %s", verb, why);
    return buf;
}

}  // namespace

void ShowNotice(const char* msg) { SetNotice(msg); }

std::string Notice() {
    std::lock_guard lock(notice_mu);
    if (notice_msg.empty() || NowMs() >= notice_expiry) return {};
    return notice_msg;
}

bool GlyphRows(char c, uint8_t rows[7]) {
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    const Glyph* g = GlyphOf(c);
    if (!g) return false;
    std::memcpy(rows, g->r, 7);
    return true;
}

void Execute(X86Context& ctx, int is_load) {
    uint32_t object = kernel::R32(kObject);
    char reason[96];
    if (const char* r = Refusal(ctx, object, is_load, reason, sizeof reason)) {
        std::fprintf(stderr, "[FCI] %s\n", r);
        std::fflush(stderr);
        SetNotice(is_load ? "QUICK LOAD REFUSED" : "QUICK SAVE REFUSED");
        return;
    }

    std::string level = GuestName(object + kLevelName), mission = GuestName(object + kMissionName);
    char text[200];
    if (is_load)
        std::snprintf(text, sizeof text, kLoadFormat, level.c_str(), mission.c_str(), level.c_str());
    else
        std::snprintf(text, sizeof text, kSaveFormat, level.c_str());
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
}

}  // namespace fci_quicksave
