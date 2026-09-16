#pragma once
// PNG decoder for sprite-style images: 8-bit grey/RGB/RGBA/palette (with
// tRNS), non-interlaced, plus the Doom "grAb" chunk for sprite offsets.
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "core/image.h"

namespace rl {

std::optional<Image> decodePng(std::span<const uint8_t> bytes, std::string* err = nullptr);

}  // namespace rl
