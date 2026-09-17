#pragma once
// Pixel-art upscaler for the HUD font: Doom's 8-pixel glyphs look blocky once
// the HUD blows them up four to six times. xBR (Hyllian's edge-directed
// interpolation, applied 2x at a time) turns stair-steps into smooth,
// anti-aliased diagonals and curves while leaving flat edges and corners crisp.
#include "core/image.h"

namespace rl {

// Returns src enlarged by `factor` (a power of two, 2..8).
Image scalePixelArt(const Image& src, int factor);

}  // namespace rl
