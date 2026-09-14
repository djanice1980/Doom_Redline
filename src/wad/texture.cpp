// Wall textures: PNAMES + TEXTURE1/TEXTURE2 parsing and patch composition.
#include "wad/wad.h"

#include "wad/bytes.h"

#include <algorithm>

namespace rl::wad {

using detail::inRange;
using detail::rdI16;
using detail::rdI32;

// ---------------------------------------------------------------------------
// Lump formats
// ---------------------------------------------------------------------------
//
// PNAMES:   int32 count, then count x char[8] patch lump names.
// TEXTUREx: int32 numtextures, int32 offsets[numtextures] (from lump start)
//   maptexture_t: char name[8], int32 masked (ignored), int16 width,
//                 int16 height, int32 columndirectory (ignored),
//                 int16 patchcount, then patchcount x mappatch_t
//   mappatch_t:   int16 originx, int16 originy, int16 patch (PNAMES index),
//                 int16 stepdir (ignored), int16 colormap (ignored)

static std::vector<std::string> parsePnames(std::span<const uint8_t> d) {
    std::vector<std::string> names;
    if (!inRange(d, 0, 4)) return names;
    int32_t count = rdI32(d, 0);
    if (count <= 0) return names;
    // Clamp to the entries that physically fit.
    size_t n = std::min(static_cast<size_t>(count), (d.size() - 4) / 8);
    names.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        char raw[8];
        for (size_t k = 0; k < 8; ++k) raw[k] = static_cast<char>(d[4 + i * 8 + k]);
        names.emplace_back(normalizeName(std::string_view(raw, 8)));
    }
    return names;
}

void TextureSet::parseTextureLump(std::span<const uint8_t> d, std::vector<Def>& out) {
    if (!inRange(d, 0, 4)) return;
    int32_t count = rdI32(d, 0);
    if (count <= 0) return;
    size_t n = std::min(static_cast<size_t>(count), (d.size() - 4) / 4);

    for (size_t i = 0; i < n; ++i) {
        int32_t off = rdI32(d, 4 + i * 4);
        if (off < 0) continue;
        size_t base = static_cast<size_t>(off);
        if (!inRange(d, base, 22)) continue;  // maptexture_t header is 22 bytes

        Def def;
        char raw[8];
        for (size_t k = 0; k < 8; ++k) raw[k] = static_cast<char>(d[base + k]);
        def.info.name = normalizeName(std::string_view(raw, 8));
        def.info.width = rdI16(d, base + 12);
        def.info.height = rdI16(d, base + 14);
        int patchCount = rdI16(d, base + 20);
        if (!detail::plausibleImageSize(def.info.width, def.info.height) || patchCount < 0) continue;

        for (int p = 0; p < patchCount; ++p) {
            size_t q = base + 22 + static_cast<size_t>(p) * 10;
            if (!inRange(d, q, 10)) break;  // truncated patch list: keep what we have
            PatchRef ref;
            ref.originX = rdI16(d, q);
            ref.originY = rdI16(d, q + 2);
            ref.patch = rdI16(d, q + 4);
            def.patches.push_back(ref);
        }
        out.push_back(std::move(def));
    }
}

// ---------------------------------------------------------------------------
// TextureSet
// ---------------------------------------------------------------------------

std::optional<TextureSet> TextureSet::load(const Wad& wad, const Palette& pal) {
    const Lump* pnames = wad.find("PNAMES");
    const Lump* tex1 = wad.find("TEXTURE1");
    if (!pnames || !tex1) return std::nullopt;

    TextureSet set;

    // Decode every patch PNAMES names. Names in PNAMES are occasionally
    // lowercase in the official IWADs, hence the case-insensitive find().
    std::vector<std::string> patchNames = parsePnames(wad.data(*pnames));
    set.patches_.reserve(patchNames.size());
    for (const std::string& name : patchNames) {
        const Lump* l = wad.find(name);
        set.patches_.push_back(l ? decodePatch(wad.data(*l), pal) : std::nullopt);
    }

    parseTextureLump(wad.data(*tex1), set.defs_);
    if (const Lump* tex2 = wad.find("TEXTURE2")) parseTextureLump(wad.data(*tex2), set.defs_);
    if (set.defs_.empty()) return std::nullopt;

    for (size_t i = 0; i < set.defs_.size(); ++i) set.index_[set.defs_[i].info.name] = i;  // last wins
    return set;
}

std::optional<TextureInfo> TextureSet::info(std::string_view textureName) const {
    auto it = index_.find(normalizeName(textureName));
    if (it == index_.end()) return std::nullopt;
    return defs_[it->second].info;
}

std::optional<rl::Image> TextureSet::get(std::string_view textureName) const {
    auto it = index_.find(normalizeName(textureName));
    if (it == index_.end()) return std::nullopt;
    const Def& def = defs_[it->second];

    rl::Image img(def.info.width, def.info.height);  // starts fully transparent
    for (const PatchRef& ref : def.patches) {
        if (ref.patch < 0 || static_cast<size_t>(ref.patch) >= patches_.size()) continue;
        const std::optional<rl::Image>& patch = patches_[static_cast<size_t>(ref.patch)];
        if (!patch) continue;

        // Clip the patch rectangle against the texture and copy opaque pixels.
        int x0 = std::max(0, ref.originX);
        int y0 = std::max(0, ref.originY);
        int x1 = std::min(img.width, ref.originX + patch->width);
        int y1 = std::min(img.height, ref.originY + patch->height);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const uint8_t* src = patch->px(x - ref.originX, y - ref.originY);
                if (src[3] == 0) continue;  // transparent: leave what is underneath
                img.set(x, y, src[0], src[1], src[2], src[3]);
            }
        }
    }
    return img;
}

std::vector<std::string> TextureSet::names() const {
    std::vector<std::string> out;
    out.reserve(defs_.size());
    for (const Def& d : defs_) out.push_back(d.info.name);
    return out;
}

std::vector<TextureInfo> listTextures(const Wad& wad) {
    std::vector<TextureSet::Def> defs;
    if (const Lump* l = wad.find("TEXTURE1")) TextureSet::parseTextureLump(wad.data(*l), defs);
    if (const Lump* l = wad.find("TEXTURE2")) TextureSet::parseTextureLump(wad.data(*l), defs);
    std::vector<TextureInfo> out;
    out.reserve(defs.size());
    for (TextureSet::Def& d : defs) out.push_back(std::move(d.info));
    return out;
}

}  // namespace rl::wad
