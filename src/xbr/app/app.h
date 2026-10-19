#pragma once
// FCI: the subset of Xbox XBE Recomp's app/app.h the vendored GPU code uses, implemented in
// fci_xbr_glue.cpp (see ../README.md). The window, settings and pacing are this runtime's.
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace app {

struct Settings {
    bool vsync = true;
    uint32_t aniso = 0;  // 0 as the game asks
    bool present_never_waits = false;  // FCI: test runs, RECOMP_FCI_XBR_NO_PRESENT_WAIT (fci_xbr_glue.cpp)
};
Settings& Get();
// FCI: the render scale per axis for the title's back buffer (fci_xbr_glue.cpp)
void RenderScale(uint32_t guest_width, uint32_t guest_height, float& sx, float& sy);
bool Widescreen();
void SetWidescreen(bool on);  // FCI: from the title's CreateDevice
std::filesystem::path ExeDir();
std::filesystem::path ShaderCacheDir();  // FCI: RECOMP_FCI_XBR_SHADER_CACHE, empty for none
void* Window();  // HWND
void SplashFirstFrame();
bool HandingOver();
bool Leaving();  // FCI: the window went to the next instance; present nothing more
void FinishHandover();
std::recursive_mutex& UiLock();
void Pace();
// FCI: the title's PresentationInterval (CreateDevice): Pace keeps its frame period
void SetPresentInterval(uint32_t interval);

struct GameInfo {
    void (*ui)(double fps);
};
extern const GameInfo kGame;

}  // namespace app
