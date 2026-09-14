#include "render/atlas.h"

#include <algorithm>
#include <cstring>

namespace rl::render {

void Atlas::add(const std::string& key, const Image& img) {
    if (!img.valid()) return;
    pending_.push_back({key, img});
}

bool Atlas::build(int maxSize) {
    // A 4x4 white block is always present so untextured geometry has something to sample.
    {
        Image white(4, 4);
        std::fill(white.rgba.begin(), white.rgba.end(), 255);
        pending_.insert(pending_.begin(), Entry{"__white", white});
    }
    // Tallest first gives good shelf packing.
    std::stable_sort(pending_.begin(), pending_.end(), [](const Entry& a, const Entry& b) { return a.img.height > b.img.height; });

    const int pad = 1;
    for (int size = 512; size <= maxSize; size *= 2) {
        int x = pad, y = pad, shelfH = 0;
        bool ok = true;
        std::vector<AtlasRegion> placed(pending_.size());
        for (size_t i = 0; i < pending_.size() && ok; ++i) {
            const Image& im = pending_[i].img;
            if (im.width + 2 * pad > size || im.height + 2 * pad > size) { ok = false; break; }
            if (x + im.width + pad > size) {   // next shelf
                x = pad;
                y += shelfH + pad;
                shelfH = 0;
            }
            if (y + im.height + pad > size) { ok = false; break; }
            placed[i] = {0, 0, 0, 0, x, y, im.width, im.height, im.offsetX, im.offsetY};
            x += im.width + pad;
            shelfH = std::max(shelfH, im.height);
        }
        if (!ok) continue;

        atlas_ = Image(size, size);
        regions_.clear();
        const float inv = 1.f / static_cast<float>(size);
        for (size_t i = 0; i < pending_.size(); ++i) {
            const Image& im = pending_[i].img;
            AtlasRegion r = placed[i];
            for (int row = 0; row < im.height; ++row)
                std::memcpy(atlas_.px(r.x, r.y + row), im.px(0, row), static_cast<size_t>(im.width) * 4);
            // Half-texel inset so nearest sampling never bleeds into the padding.
            r.u0 = (static_cast<float>(r.x) + 0.5f * 0.f) * inv;
            r.v0 = (static_cast<float>(r.y)) * inv;
            r.u1 = (static_cast<float>(r.x + r.w)) * inv;
            r.v1 = (static_cast<float>(r.y + r.h)) * inv;
            regions_[pending_[i].key] = r;
        }
        white_ = regions_["__white"];
        // Sample the centre of the white block so filtering never touches its edge.
        white_.u0 = white_.u1 = (static_cast<float>(white_.x) + 2.f) * inv;
        white_.v0 = white_.v1 = (static_cast<float>(white_.y) + 2.f) * inv;
        regions_["__white"] = white_;
        pending_.clear();
        return true;
    }
    return false;
}

const AtlasRegion* Atlas::find(const std::string& key) const {
    auto it = regions_.find(key);
    return it == regions_.end() ? nullptr : &it->second;
}

const AtlasRegion& Atlas::get(const std::string& key) const {
    const AtlasRegion* r = find(key);
    return r ? *r : white_;
}

}  // namespace rl::render
