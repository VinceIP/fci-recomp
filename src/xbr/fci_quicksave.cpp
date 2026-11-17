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
extern "C" int fci_input_evolution_game(void);       // fci_input.c
extern "C" const char* fci_save_dir;                 // fci_main.c

#ifdef _WIN32
#include <windows.h>
#endif

namespace fci_quicksave {
namespace {

// Per title: the game object the commands post to, its post function, the level and mission names (guest
// strings, set when a level starts) and the calls the refusals make (qwen/notes/q189-evo-quicksave.md).
struct Title {
    uint32_t object;       // the global holding the game object
    uint32_t post;         // Instincts: edx = text, object pushed (ret 4); Evolution: (object, text) (ret 8)
    bool post_on_stack;    // Evolution's convention
    uint32_t level_name, mission_name;
    uint32_t level;        // the level object LoadGame null-checks (0: no such field)
    uint32_t net_flag, players;  // the multiplayer test: byte net_flag != 0 OR dword players > 1
    uint32_t player, alive;  // the player-entity getter (eax object, ecx index) and IsAlive (esi entity)
    const char* title_id;  // the UDATA folder
};
// Instincts: sub_00012000; names set by sub_0004D010; multiplayer as sub_00013760 (recomp_0000.c:6007)
constexpr Title kInstincts = {0x6B3984, 0x00012000, false, 0x2A4, 0x2C0, 0x24,
                              0x329, 0x60, 0x0004CD60, 0x00020390, "55530008"};
// Evolution: sub_00012050 as its Resume (sub_000FCB1B) calls it; multiplayer as sub_000130A6
constexpr Title kEvolution = {0x56B594, 0x00012050, true, 0x2BC, 0x2D8, 0,
                              0x342, 0x64, 0x0003C6E9, 0x0001B53C, "55530060"};
// The commands, as the game spells them: the save as a Checkpoint trigger's Game:SaveCheckpoint, named per
// level (Quick_<level>) so a load never restores another level's state; the load as Story Resume (format
// 0x6095A0, sub_001509A0), which restores the level, the player's place and facing from the save.
// RestartStartLevel (0x600150) restores the state but spawns the player at the level start.
constexpr const char* kSaveFormat = "SaveGame checkpoint=Quick_%s";
constexpr const char* kLoadFormat = "StartLevel level=%s mission=%s checkpoint=Quick_%s";

// the running title's table; null for the map editors (no game command path mapped)
const Title* RunningTitle() {
    if (fci_input_quicksave_supported()) return &kInstincts;
    if (fci_input_evolution_game()) return &kEvolution;
    return nullptr;
}
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

// a Quick_<level>.fcs in any of the title's profile folders (UDATA\<title id>\<profile>)
bool HaveQuickSave(const Title& t, const std::string& level) {
    std::error_code ec;
    std::filesystem::path udata = std::filesystem::path(fci_save_dir ? fci_save_dir : "") / "UDATA" / t.title_id;
    for (const auto& e : std::filesystem::directory_iterator(udata, ec))
        if (e.is_directory(ec) && std::filesystem::exists(e.path() / ("Quick_" + level + ".fcs"), ec)) return true;
    return false;
}

// player 0 alive, the test a Checkpoint trigger makes before it saves (Instincts Script:IsAlive, sub_00205290):
// the entity from the getter (eax the game object, ecx the player), then IsAlive with esi the entity
bool PlayerAlive(X86Context& ctx, const Title& t, uint32_t object) {
    X86Context saved = ctx;
    uint32_t ent = hle::Call(ctx, t.player, hle::Make({hle::eax, hle::ecx}), {object, 0});
    bool alive = ent && (hle::Call(ctx, t.alive, hle::Make({hle::esi}), {ent}) & 0xFF) != 0;
    ctx = saved;
    return alive;
}

// the refusal reason into the buffer, or null when the command may go
const char* Refusal(X86Context& ctx, const Title* t, uint32_t object, int is_load, char* buf, size_t n) {
    const char* verb = is_load ? "quick load" : "quick save";
    const char* why = nullptr;
    if (!t)
        why = "not supported for this image";
    else if (object == 0)
        why = "no game object";
    else if (kernel::R8(object + t->net_flag) != 0 || kernel::R32(object + t->players) > 1)
        why = "multiplayer";
    // whether menus clear the level object is unverified, so every post logs it
    else if (t->level && kernel::R32(object + t->level) == 0)
        why = "no level";
    else if (GuestName(object + t->level_name).empty() || GuestName(object + t->mission_name).empty())
        why = "no level name";
    else if (!is_load && !PlayerAlive(ctx, *t, object))
        why = "the player is not alive";
    else if (is_load && !HaveQuickSave(*t, GuestName(object + t->level_name)))
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

// posts a game command: the text 0x100 bytes below esp, then the title's post; the context is restored
// so the game's own code leaves no side effects (docs/debugging.md, "Quick save and quick load")
bool PostText(X86Context& ctx, const char* text) {
    const Title* t = RunningTitle();
    if (!t) {
        std::fprintf(stderr, "[FCI] post refused: not supported for this image\n");
        std::fflush(stderr);
        return false;
    }
    uint32_t object = kernel::R32(t->object);
    if (object == 0) {
        std::fprintf(stderr, "[FCI] post refused: no game object\n");
        std::fflush(stderr);
        return false;
    }
    X86Context saved = ctx;
    ctx.esp -= 0x100;
    uint32_t at = ctx.esp;
    size_t n = std::strlen(text);
    for (size_t i = 0; i <= n; i++) kernel::W8(at + i, i < n ? text[i] : 0);

    if (t->post_on_stack)
        hle::Call(ctx, t->post, hle::Make({hle::stack, hle::stack}), {object, at});
    else
        hle::Call(ctx, t->post, hle::Make({hle::edx, hle::stack}), {at, object});
    ctx = saved;
    return true;
}

void Execute(X86Context& ctx, int is_load) {
    const Title* t = RunningTitle();
    uint32_t object = t ? kernel::R32(t->object) : 0;
    char reason[96];
    if (const char* r = Refusal(ctx, t, object, is_load, reason, sizeof reason)) {
        std::fprintf(stderr, "[FCI] %s\n", r);
        std::fflush(stderr);
        SetNotice(is_load ? "QUICK LOAD REFUSED" : "QUICK SAVE REFUSED");
        return;
    }

    std::string level = GuestName(object + t->level_name), mission = GuestName(object + t->mission_name);
    char text[200];
    if (is_load)
        std::snprintf(text, sizeof text, kLoadFormat, level.c_str(), mission.c_str(), level.c_str());
    else
        std::snprintf(text, sizeof text, kSaveFormat, level.c_str());
    std::fprintf(stderr, "[FCI] %s: posting \"%s\" to %08X (level %08X)\n", is_load ? "quick load" : "quick save",
                 text, object, t->level ? kernel::R32(object + t->level) : 0);
    std::fflush(stderr);
    PostText(ctx, text);

    SetNotice(is_load ? "QUICK LOAD" : "QUICK SAVE");
}

// ---- a game command queued by the control path (RECOMP_FCI_CONTROL "post <text>") ----
// The control thread queues one text; the frame boundary (D3DDevice_Swap) posts it with PostText. One
// command at a time: a new one replaces the older unposted one.
std::mutex post_mu;
std::string post_text;

void QueuePost(const char* text) {
    std::lock_guard lock(post_mu);
    post_text = text;
}

std::string TakePost() {
    std::lock_guard lock(post_mu);
    std::string out;
    std::swap(out, post_text);
    return out;
}

void PostControl(X86Context& ctx, const char* text) {
    if (!PostText(ctx, text)) return;
    std::fprintf(stderr, "[FCI] control: posted \"%s\"\n", text);
    std::fflush(stderr);
}

extern "C" void fci_quicksave_queue_post(const char* text) { QueuePost(text); }

}  // namespace fci_quicksave
