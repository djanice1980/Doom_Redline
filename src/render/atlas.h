#pragma once
// Packs many small RGBA images into one atlas texture (shelf packing with
// 1px padding). Regions are addressed by string key.
#include <string>
#include <unordered_map>
#include <vector>

#include "core/image.h"

namespace rl::render {

struct AtlasRegion {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // normalised
    int x = 0, y = 0, w = 0, h = 0;         // pixels
    int offsetX = 0, offsetY = 0;           // sprite origin (from Image)
};

class Atlas {
public:
    void add(const std::string& key, const Image& img);
    // Packs everything added so far. Returns false if it does not fit even in maxSize.
    bool build(int maxSize = 4096);
    const Image& image() const { return atlas_; }
    const AtlasRegion* find(const std::string& key) const;
    const AtlasRegion& get(const std::string& key) const;   // falls back to a 1x1 white region
    bool has(const std::string& key) const { return regions_.count(key) != 0; }
    size_t count() const { return regions_.size(); }

private:
    struct Entry { std::string key; Image img; };
    std::vector<Entry> pending_;
    Image atlas_;
    std::unordered_map<std::string, AtlasRegion> regions_;
    AtlasRegion white_;
};

}  // namespace rl::render
