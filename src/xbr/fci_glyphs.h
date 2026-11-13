#pragma once
// Button prompts drawn for the device in use (fci_glyphs.cpp)
#include <cstdint>
#include <vector>

#include "gpu/texture.h"

namespace fci_glyphs {
// a texture level about to be uploaded: true when it was the font page and its button glyphs were redrawn
bool Patch(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t level, std::vector<uint8_t>& px, uint32_t pitch);
// each present: the device changed, so the font page is uploaded (and drawn) again
void Poll();
}  // namespace fci_glyphs
