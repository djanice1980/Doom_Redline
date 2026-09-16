#pragma once
// Minimal KTX2 reader: uncompressed (no supercompression) 2D textures with
// mip levels, as written by the GZDoom: Ray Traced material pack. Payloads are
// returned as-is for upload: RGBA8 or block-compressed (BC5/BC7) data.
#include <cstdint>
#include <string>
#include <vector>

namespace rl::render {

struct Ktx2Image {
    uint32_t vkFormat = 0;   // VkFormat numeric value (37 = R8G8B8A8_UNORM, 141 = BC5, 145 = BC7 ...)
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;   // level 0 first
    bool blockCompressed() const { return vkFormat >= 131 && vkFormat <= 146; }
};

// Returns false with `err` set for anything but a plain, uncompressed 2D KTX2.
bool loadKtx2(const std::vector<uint8_t>& bytes, Ktx2Image& out, std::string* err = nullptr);

}  // namespace rl::render
