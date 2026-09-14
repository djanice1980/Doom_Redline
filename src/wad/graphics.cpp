// Picture decoding: patches, flats, fonts, and the SpriteSet.
#include "wad/wad.h"

#include "wad/bytes.h"

#include <algorithm>

namespace rl::wad {

using detail::inRange;
using detail::rdI16;
using detail::rdU32;

// ---------------------------------------------------------------------------
// Patch format
// ---------------------------------------------------------------------------
//
// Header:  int16 width, int16 height, int16 leftoffset, int16 topoffset
//          uint32 columnofs[width]        (offsets from the start of the lump)
// Column:  posts until a byte 0xFF:
//          uint8 topdelta, uint8 length, uint8 pad, uint8 pixels[length], uint8 pad
// Pixels are palette indices. Anything no post covers is transparent.

std::optional<rl::Image> decodePatch(std::span<const uint8_t> d, const Palette& pal) {
    if (!inRange(d, 0, 8)) return std::nullopt;
    int width = rdI16(d, 0);
    int height = rdI16(d, 2);
    int left = rdI16(d, 4);
    int top = rdI16(d, 6);
    if (!detail::plausibleImageSize(width, height)) return std::nullopt;

    // The column offset table must fit; this also bounds width by lump size / 4.
    size_t tableBytes = static_cast<size_t>(width) * 4;
    if (!inRange(d, 8, tableBytes)) return std::nullopt;

    rl::Image img(width, height);
    img.offsetX = left;
    img.offsetY = top;

    for (int x = 0; x < width; ++x) {
        uint32_t colOff = rdU32(d, 8 + static_cast<size_t>(x) * 4);
        if (colOff >= d.size()) return std::nullopt;  // column starts past the end: corrupt
        size_t pos = colOff;
        int prevTop = -1;
        for (;;) {
            // A column that runs off the end of the lump before its 0xFF
            // terminator is tolerated (a few real WADs ship such patches).
            if (pos >= d.size()) break;
            uint8_t topdelta = d[pos];
            if (topdelta == 0xFF) break;
            if (!inRange(d, pos, 4)) return std::nullopt;  // topdelta, length, pad, ... incomplete
            uint8_t length = d[pos + 1];
            // pixels start at pos+3 and are followed by one trailing pad byte
            if (!inRange(d, pos + 3, static_cast<size_t>(length) + 1)) return std::nullopt;

            // "Tall patch" extension: when topdelta does not increase it is a
            // delta from the previous post's top, allowing heights > 254.
            int postTop = (topdelta <= prevTop) ? prevTop + topdelta : topdelta;
            prevTop = postTop;

            for (int i = 0; i < length; ++i) {
                int y = postTop + i;
                if (y >= height) break;  // clip, do not fail
                uint8_t idx = d[pos + 3 + static_cast<size_t>(i)];
                img.set(x, y, pal.rgb[idx][0], pal.rgb[idx][1], pal.rgb[idx][2], 255);
            }
            pos += 4 + static_cast<size_t>(length);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// Flats
// ---------------------------------------------------------------------------

std::optional<rl::Image> decodeFlat(std::span<const uint8_t> d, const Palette& pal) {
    // 64x64 palette indices, row-major. Some WADs ship slightly larger flats
    // (e.g. 64x65 for scrolling); the extra bytes are ignored.
    if (d.size() < 4096) return std::nullopt;
    rl::Image img(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint8_t idx = d[static_cast<size_t>(y) * 64 + static_cast<size_t>(x)];
            img.set(x, y, pal.rgb[idx][0], pal.rgb[idx][1], pal.rgb[idx][2], 255);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// Named patches and fonts
// ---------------------------------------------------------------------------

std::optional<rl::Image> loadPatch(const Wad& wad, const Palette& pal, std::string_view lumpName) {
    const Lump* l = wad.find(lumpName);
    if (!l) return std::nullopt;
    return decodePatch(wad.data(*l), pal);
}

std::map<char, rl::Image> loadFont(const Wad& wad, const Palette& pal, std::string_view prefix) {
    std::map<char, rl::Image> out;
    std::string p = normalizeName(prefix);
    // Walk in file order so a later duplicate glyph replaces an earlier one.
    for (const Lump* l : fontLumps(wad, p)) {
        int code = 0;
        for (size_t i = p.size(); i < l->name.size(); ++i) code = code * 10 + (l->name[i] - '0');
        if (code > 255) continue;
        auto img = decodePatch(wad.data(*l), pal);
        if (!img) continue;
        out.insert_or_assign(static_cast<char>(static_cast<unsigned char>(code)), std::move(*img));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------

static bool isFrameLetter(char c) { return c >= 'A' && c <= 'Z'; }
static bool isRotationDigit(char c) { return c >= '0' && c <= '8'; }

void SpriteSet::install(char frame, int rotation, int imageIndex, bool mirrored) {
    Frame& f = frames_[frame];
    f.rot[rotation] = Slot{imageIndex, mirrored};
}

std::optional<SpriteSet> SpriteSet::load(const Wad& wad, const Palette& pal, std::string_view spriteName) {
    std::string base = normalizeName(spriteName);
    if (base.size() != 4) return std::nullopt;

    SpriteSet set;
    set.name_ = base;

    auto scan = [&](const std::vector<const Lump*>& lumps) {
        for (const Lump* l : lumps) {
            const std::string& n = l->name;
            // Exact 4-char match plus at least one valid frame/rotation pair.
            if (n.size() != 6 && n.size() != 8) continue;
            if (n.compare(0, 4, base) != 0) continue;
            if (!isFrameLetter(n[4]) || !isRotationDigit(n[5])) continue;
            if (n.size() == 8 && (!isFrameLetter(n[6]) || !isRotationDigit(n[7]))) continue;

            auto img = decodePatch(wad.data(*l), pal);
            if (!img) continue;  // odd or corrupt lump between the markers: skip

            int idx = static_cast<int>(set.images_.size());
            set.images_.push_back(std::move(*img));
            set.install(n[4], n[5] - '0', idx, false);
            if (n.size() == 8) set.install(n[6], n[7] - '0', idx, true);  // mirrored reuse
        }
    };
    scan(wad.between("S_START", "S_END"));
    scan(wad.between("SS_START", "SS_END"));

    if (set.frames_.empty()) return std::nullopt;
    return set;
}

std::optional<SpriteFrameView> SpriteSet::get(char frame, int rotation) const {
    if (frame >= 'a' && frame <= 'z') frame = static_cast<char>(frame - 'a' + 'A');
    auto it = frames_.find(frame);
    if (it == frames_.end()) return std::nullopt;
    if (rotation < 0 || rotation > 8) return std::nullopt;
    if (rotation == 0) rotation = 1;

    const Frame& f = it->second;
    auto view = [&](const Slot& s) { return SpriteFrameView{&images_[static_cast<size_t>(s.image)], s.mirrored}; };

    if (f.rot[0].image >= 0) return view(f.rot[0]);         // one image for all angles
    if (f.rot[rotation].image >= 0) return view(f.rot[rotation]);
    if (f.rot[1].image >= 0) return view(f.rot[1]);         // fall back to the front view
    for (int r = 2; r <= 8; ++r) {
        if (f.rot[r].image >= 0) return view(f.rot[r]);     // anything at all
    }
    return std::nullopt;
}

std::vector<char> SpriteSet::frames() const {
    std::vector<char> out;
    out.reserve(frames_.size());
    for (const auto& [frame, _] : frames_) out.push_back(frame);  // std::map is already sorted
    return out;
}

}  // namespace rl::wad
