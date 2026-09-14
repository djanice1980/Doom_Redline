#pragma once
// Shared CPU-side RGBA8 image type used by the WAD decoder, procedural
// asset generator and the texture atlas packer.
#include <cstdint>
#include <vector>

namespace rl {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, row-major, top row first

    // Sprite/patch origin offsets (Doom patch header). For a sprite,
    // (offsetX, offsetY) is the pixel that sits at the sprite's world origin:
    // offsetX columns from the left edge, offsetY rows from the top edge.
    // Zero for images that have no meaningful origin (flats, textures, glyphs).
    int offsetX = 0;
    int offsetY = 0;

    Image() = default;
    Image(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * h * 4, 0) {}

    bool valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }

    uint8_t* px(int x, int y) { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
    const uint8_t* px(int x, int y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        uint8_t* p = px(x, y);
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
};

}  // namespace rl
