#include "core/pixel_scale.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace rl {

namespace {
// A pixel as one 32-bit value plus a cheap "distance" for the edge detector.
// Glyphs are white-on-transparent with a few shades, so alpha dominates the
// distance and intensity breaks ties.
struct Px { uint8_t r, g, b, a; };

Px at(const Image& img, int x, int y) {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return {0, 0, 0, 0};
    const uint8_t* p = img.px(x, y);
    return {p[0], p[1], p[2], p[3]};
}
bool eq(Px a, Px b) {
    if ((a.a < 128) != (b.a < 128)) return false;
    if (a.a < 128) return true;
    return std::abs(std::max({a.r, a.g, a.b}) - std::max({b.r, b.g, b.b})) < 48;
}
unsigned df(Px a, Px b) {
    const int ia = a.a < 128 ? 0 : 64 + std::max({a.r, a.g, a.b}) / 2;
    const int ib = b.a < 128 ? 0 : 64 + std::max({b.r, b.g, b.b}) / 2;
    return static_cast<unsigned>(std::abs(ia - ib));
}
Px blend(Px dst, Px src, int w) {   // w/256 of src over dst, premultiplied so transparent texels stay clean
    const int wa = 256 - w;
    const int a = (dst.a * wa + src.a * w) / 256;
    if (a == 0) return {0, 0, 0, 0};
    auto ch = [&](int d, int s) { return static_cast<uint8_t>((d * dst.a * wa + s * src.a * w) / (a * 256)); };
    return {ch(dst.r, src.r), ch(dst.g, src.g), ch(dst.b, src.b), static_cast<uint8_t>(a)};
}

// xBR 2x (Hyllian), one corner of the 2x2 output at a time. The neighbourhood is
// rotated by the caller so that the corner being decided is always "N3".
//   A1 B1 C1
// A0 PA PB PC C4
// D0 PD PE PF F4
// G0 PG PH PI I4
//    G5 H5 I5
void corner(Px PE, Px PI, Px PH, Px PF, Px PG, Px PC, Px PD, Px PB, Px PA, Px G5, Px C4, Px G0, Px D0, Px C1, Px B1, Px F4, Px I4, Px H5, Px I5, Px A0, Px A1,
            Px& N1, Px& N2, Px& N3) {
    if (eq(PE, PH) || eq(PE, PF)) return;
    const unsigned e = df(PE, PC) + df(PE, PG) + df(PI, H5) + df(PI, F4) + (df(PH, PF) << 2);
    const unsigned i = df(PH, PD) + df(PH, I5) + df(PF, I4) + df(PF, PB) + (df(PE, PI) << 2);
    if (e > i) return;
    const Px px = df(PE, PF) <= df(PE, PH) ? PF : PH;
    if (e < i && ((!eq(PF, PB) && !eq(PH, PD)) || (eq(PE, PI) && (!eq(PF, I4) && !eq(PH, I5))) || eq(PE, PG) || eq(PE, PC))) {
        const unsigned ke = df(PF, PG), ki = df(PH, PC);
        const bool left = (ke << 1) <= ki && !eq(PE, PG) && !eq(PD, PG);
        const bool up = ke >= (ki << 1) && !eq(PE, PC) && !eq(PB, PC);
        if (left && up) { N3 = blend(N3, px, 224); N2 = blend(N2, px, 64); N1 = N2; }
        else if (left) { N3 = blend(N3, px, 192); N2 = blend(N2, px, 64); }
        else if (up) { N3 = blend(N3, px, 192); N1 = blend(N1, px, 64); }
        else N3 = blend(N3, px, 128);
    } else {
        N3 = blend(N3, px, 128);
    }
    (void)PA; (void)G5; (void)C4; (void)G0; (void)D0; (void)C1; (void)B1; (void)A0; (void)A1;
}

Image xbr2x(const Image& src) {
    Image out(src.width * 2, src.height * 2);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            const Px A1 = at(src, x - 1, y - 2), B1 = at(src, x, y - 2), C1 = at(src, x + 1, y - 2);
            const Px A0 = at(src, x - 2, y - 1), PA = at(src, x - 1, y - 1), PB = at(src, x, y - 1), PC = at(src, x + 1, y - 1), C4 = at(src, x + 2, y - 1);
            const Px D0 = at(src, x - 2, y), PD = at(src, x - 1, y), PE = at(src, x, y), PF = at(src, x + 1, y), F4 = at(src, x + 2, y);
            const Px G0 = at(src, x - 2, y + 1), PG = at(src, x - 1, y + 1), PH = at(src, x, y + 1), PI = at(src, x + 1, y + 1), I4 = at(src, x + 2, y + 1);
            const Px G5 = at(src, x - 1, y + 2), H5 = at(src, x, y + 2), I5 = at(src, x + 1, y + 2);
            Px E0 = PE, E1 = PE, E2 = PE, E3 = PE;   // top-left, top-right, bottom-left, bottom-right
            // Bottom-right corner, then the other three by rotating the neighbourhood.
            corner(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, E1, E2, E3);
            corner(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, E3, E0, E1);
            corner(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I5, I4, E2, E3, E0);
            corner(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C4, C1, E0, E1, E2);
            auto put = [&](int ox, int oy, Px p) { uint8_t* d = out.px(2 * x + ox, 2 * y + oy); d[0] = p.r; d[1] = p.g; d[2] = p.b; d[3] = p.a; };
            put(0, 0, E0); put(1, 0, E1); put(0, 1, E2); put(1, 1, E3);
        }
    }
    return out;
}
}  // namespace

Image scalePixelArt(const Image& src, int factor) {
    if (!src.valid() || factor < 2) return src;
    Image img = src;
    for (int s = 1; s < factor; s *= 2) img = xbr2x(img);
    return img;
}

}  // namespace rl
