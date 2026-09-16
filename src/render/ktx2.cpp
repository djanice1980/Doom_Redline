#include "render/ktx2.h"

#include <cstring>

namespace rl::render {

namespace {
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
const uint8_t kMagic[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
}  // namespace

bool loadKtx2(const std::vector<uint8_t>& d, Ktx2Image& out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (d.size() < 104 || std::memcmp(d.data(), kMagic, 12) != 0) return fail("not a KTX2 file");
    out = {};
    out.vkFormat = rd32(&d[12]);
    out.width = rd32(&d[20]);
    out.height = rd32(&d[24]);
    const uint32_t depth = rd32(&d[28]), layers = rd32(&d[32]), faces = rd32(&d[36]);
    uint32_t levels = rd32(&d[40]);
    const uint32_t supercompression = rd32(&d[44]);
    if (depth > 1 || layers > 1 || faces != 1) return fail("only plain 2D textures are supported");
    if (supercompression != 0) return fail("supercompressed KTX2 is not supported");
    if (out.width == 0 || out.height == 0) return fail("bad size");
    if (levels == 0) levels = 1;
    if (levels > 16 || 80 + 24 * static_cast<size_t>(levels) > d.size()) return fail("bad level count");
    for (uint32_t i = 0; i < levels; ++i) {
        const uint8_t* e = &d[80 + 24 * static_cast<size_t>(i)];
        const uint64_t off = rd64(e), len = rd64(e + 8);
        if (off + len > d.size() || len == 0) return fail("level out of range");
        out.levels.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + len));
    }
    return true;
}

}  // namespace rl::render
