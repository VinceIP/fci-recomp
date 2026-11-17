// FCI: quick save (F5) and quick load (F9).
//
// The host input path (fci_input.c) sets a request flag when the configured key is pressed. At the
// frame boundary (D3DDevice_Swap, the game thread, guest lock held) the request is posted to the
// game's own command path, sub_00012000, with the command text in edx and the game object (0x6B3984)
// pushed, a ret 4 call.
//
// The last message (success or refusal, or another host message) shows for ~2 s: the renderer's present
// draws Notice() over the picture (fci_enhance.cpp), at any render scale.
#pragma once
#include <cstdint>
#include <string>
#include "cpu/x86_context.h"

namespace fci_quicksave {
// Called when a quicksave or quickload request is taken at the frame boundary.
void Execute(X86Context& ctx, int is_load);
// A game command queued by the control path (RECOMP_FCI_CONTROL "post <text>"), any thread; one at a
// time, a new one replaces the older unposted one. Taken and posted at the frame boundary (D3DDevice_Swap)
// with the quick save and load's own mechanism, refusing only when there is no game object.
void QueuePost(const char* text);
std::string TakePost();
void PostControl(X86Context& ctx, const char* text);
// Shows another host message (the fog toggle) the same way.
void ShowNotice(const char* msg);
// The message to show now, empty when none or expired (any thread).
std::string Notice();
// A character's seven rows of five bits (bit 4 the left column); false when the font lacks it.
bool GlyphRows(char c, uint8_t rows[7]);
}  // namespace fci_quicksave
