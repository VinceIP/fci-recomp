// FCI: quick save (F5) and quick load (F9).
//
// The host input path (fci_input.c) sets a request flag when the configured key is pressed. At the
// frame boundary (D3DDevice_Swap, the game thread, guest lock held) the request is posted to the
// game's own command path, sub_00012000, with the command text in edx and the game object (0x6B3984)
// pushed, a ret 4 call.
//
// DrawNotice is called on every Swap: it draws the last quicksave/quickload message (success or
// refusal) until it expires (~2 s), so the player sees it.
#pragma once
#include <cstdint>
#include "cpu/x86_context.h"

namespace fci_quicksave {
// Called once per frame from D3DDevice_Swap: draws the notice if it is still within its expiry.
void DrawNotice(uint32_t* frame, uint32_t w, uint32_t h, uint32_t pitch);
// Called when a quicksave or quickload request is taken at the frame boundary.
void Execute(X86Context& ctx, int is_load, uint32_t* frame, uint32_t w, uint32_t h, uint32_t pitch);
// Shows another host message (the fog toggle) the same way.
void ShowNotice(const char* msg);
}  // namespace fci_quicksave
