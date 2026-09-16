#include "core/png_read.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/inflate.h"

namespace rl {

namespace {
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}
}  // namespace

std::optional<Image> decodePng(std::span<const uint8_t> d, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return std::optional<Image>{}; };
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (d.size() < 33 || std::memcmp(d.data(), kSig, 8) != 0) return fail("not a PNG");
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    int offX = 0, offY = 0;
    size_t pos = 8;
    bool sawIhdr = false;
    while (pos + 8 <= d.size()) {
        uint32_t len = be32(&d[pos]);
        const uint8_t* type = &d[pos + 4];
        if (pos + 12 + len > d.size()) return fail("truncated chunk");
        const uint8_t* body = &d[pos + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
            sawIhdr = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) palette.assign(body, body + len);
        else if (std::memcmp(type, "tRNS", 4) == 0) trns.assign(body, body + len);
        else if (std::memcmp(type, "IDAT", 4) == 0) idat.insert(idat.end(), body, body + len);
        else if (std::memcmp(type, "grAb", 4) == 0 && len >= 8) {
            offX = static_cast<int32_t>(be32(body));
            offY = static_cast<int32_t>(be32(body + 4));
        } else if (std::memcmp(type, "IEND", 4) == 0) break;
        pos += 12 + len;   // CRCs are not checked: mod files are often re-saved by tools that get them wrong
    }
    if (!sawIhdr || w == 0 || h == 0 || w > 8192 || h > 8192) return fail("bad header");
    if (depth != 8) return fail("only 8-bit PNGs are supported");
    if (interlace != 0) return fail("interlaced PNGs are not supported");
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (channels == 0) return fail("unsupported colour type");
    if (ctype == 3 && palette.empty()) return fail("palette image without PLTE");

    std::vector<uint8_t> raw;
    std::string ierr;
    if (!inflateZlib(idat.data(), idat.size(), raw, &ierr)) { if (err) *err = "IDAT: " + ierr; return std::nullopt; }
    const size_t stride = static_cast<size_t>(w) * static_cast<size_t>(channels);
    if (raw.size() < (stride + 1) * h) return fail("short image data");

    // Undo the per-row filters in place (rows are prefixed by the filter byte).
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    Image img(static_cast<int>(w), static_cast<int>(h));
    img.offsetX = offX;
    img.offsetY = offY;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = &raw[y * (stride + 1)];
        const int f = row[0];
        const uint8_t* src = row + 1;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= static_cast<size_t>(channels) ? cur[i - static_cast<size_t>(channels)] : 0;
            const int b = prev[i];
            const int c = i >= static_cast<size_t>(channels) ? prev[i - static_cast<size_t>(channels)] : 0;
            int v = src[i];
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            cur[i] = static_cast<uint8_t>(v);
        }
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * w + x) * 4];
            const uint8_t* s = &cur[static_cast<size_t>(x) * static_cast<size_t>(channels)];
            switch (ctype) {
                case 0: p[0] = p[1] = p[2] = s[0]; p[3] = 255; break;
                case 2: p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = 255; break;
                case 3: {
                    const size_t idx = s[0];
                    if (idx * 3 + 2 < palette.size()) { p[0] = palette[idx * 3]; p[1] = palette[idx * 3 + 1]; p[2] = palette[idx * 3 + 2]; }
                    else p[0] = p[1] = p[2] = 0;
                    p[3] = idx < trns.size() ? trns[idx] : 255;
                    break;
                }
                case 4: p[0] = p[1] = p[2] = s[0]; p[3] = s[1]; break;
                default: p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = s[3]; break;
            }
        }
        std::swap(prev, cur);
    }
    return img;
}

}  // namespace rl
