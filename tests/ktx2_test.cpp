// KTX2 reader checks on a synthetic file plus the bundled material maps when present.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "render/ktx2.h"

using rl::render::Ktx2Image;
using rl::render::loadKtx2;

namespace {

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

void put32(std::vector<uint8_t>& v, uint32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4); }
void put64(std::vector<uint8_t>& v, uint64_t x) { uint8_t b[8]; std::memcpy(b, &x, 8); v.insert(v.end(), b, b + 8); }

// A 2x2 RGBA8 texture with two mip levels, laid out like the GZDoom: Ray Traced pack
// (no supercompression, empty DFD/KVD blocks, level 0 first in the index).
std::vector<uint8_t> makeKtx2(uint32_t vkFormat, uint32_t levels, uint32_t supercompression = 0) {
    std::vector<uint8_t> d = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    put32(d, vkFormat);
    put32(d, 4);       // typeSize
    put32(d, 2);       // width
    put32(d, 2);       // height
    put32(d, 0);       // depth
    put32(d, 0);       // layers
    put32(d, 1);       // faces
    put32(d, levels);
    put32(d, supercompression);
    // dfd / kvd / sgd offsets and lengths (all empty)
    put32(d, 0); put32(d, 0); put32(d, 0); put32(d, 0); put64(d, 0); put64(d, 0);
    const size_t index = d.size();
    for (uint32_t i = 0; i < levels; ++i) { put64(d, 0); put64(d, 0); put64(d, 0); }
    // level 0: 2x2x4 = 16 bytes, level 1: 1x1x4 = 4 bytes
    const uint8_t l0[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t l1[4] = {100, 101, 102, 103};
    const uint64_t off0 = d.size();
    d.insert(d.end(), l0, l0 + 16);
    const uint64_t off1 = d.size();
    d.insert(d.end(), l1, l1 + 4);
    auto setEntry = [&](uint32_t i, uint64_t off, uint64_t len) {
        std::memcpy(&d[index + 24 * i], &off, 8);
        std::memcpy(&d[index + 24 * i + 8], &len, 8);
        std::memcpy(&d[index + 24 * i + 16], &len, 8);
    };
    setEntry(0, off0, 16);
    if (levels > 1) setEntry(1, off1, 4);
    return d;
}

void testSynthetic() {
    Ktx2Image img;
    std::string err;
    CHECK(loadKtx2(makeKtx2(37, 2), img, &err));
    CHECK(img.vkFormat == 37);
    CHECK(img.width == 2 && img.height == 2);
    CHECK(img.levels.size() == 2);
    CHECK(img.levels[0].size() == 16 && img.levels[0][0] == 1 && img.levels[0][15] == 16);
    CHECK(img.levels[1].size() == 4 && img.levels[1][0] == 100);
    CHECK(!img.blockCompressed());

    CHECK(loadKtx2(makeKtx2(145, 1), img, &err));
    CHECK(img.blockCompressed());
    CHECK(img.levels.size() == 1);

    // Rejections: bad magic, supercompressed, truncated index.
    std::vector<uint8_t> bad = makeKtx2(37, 2);
    bad[0] = 0;
    CHECK(!loadKtx2(bad, img, &err));
    CHECK(!loadKtx2(makeKtx2(37, 2, 1), img, &err));
    std::vector<uint8_t> trunc = makeKtx2(37, 2);
    trunc.resize(90);
    CHECK(!loadKtx2(trunc, img, &err));
    std::vector<uint8_t> range = makeKtx2(37, 2);
    uint64_t huge = 1u << 20;
    std::memcpy(&range[80 + 8], &huge, 8);
    CHECK(!loadKtx2(range, img, &err));
}

// The six bundled maps, when the test runs from the repo: each must load with its
// mip chain and the expected formats (BC7 or RGBA8 normals, RGBA8 roughness).
void testBundled() {
    const char* names[6] = {"STARTAN3_remix_normal", "STARTAN3_remix_roughness", "FLOOR4_8_remix_normal",
                            "FLOOR4_8_remix_roughness", "CEIL3_5_remix_normal", "CEIL3_5_remix_roughness"};
    int found = 0;
    for (const char* n : names) {
        std::string path = std::string(REDLINE_SOURCE_DIR) + "/assets/materials/" + n + ".ktx2";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> bytes;
        uint8_t buf[65536];
        size_t r;
        while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + r);
        std::fclose(f);
        ++found;
        Ktx2Image img;
        std::string err;
        CHECK(loadKtx2(bytes, img, &err));
        if (!err.empty()) std::fprintf(stderr, "%s: %s\n", n, err.c_str());
        CHECK(img.vkFormat == 37 || img.vkFormat == 145);
        CHECK(img.width >= 64 && img.width == img.height);
        // A mip chain (the pack stops at 2x2): 1 .. log2(w) + 1 levels, RGBA8 levels sized exactly.
        uint32_t maxLevels = 1;
        for (uint32_t w = img.width; w > 1; w >>= 1) ++maxLevels;
        CHECK(img.levels.size() >= 2 && img.levels.size() <= maxLevels);
        for (size_t i = 0; i < img.levels.size(); ++i) {
            const uint32_t w = std::max(1u, img.width >> i);
            if (img.vkFormat == 37) CHECK(img.levels[i].size() == static_cast<size_t>(w) * w * 4);
            else CHECK(img.levels[i].size() == static_cast<size_t>(std::max(1u, (w + 3) / 4)) * std::max(1u, (w + 3) / 4) * 16);   // BC7: 16 bytes per 4x4 block
        }
    }
    if (found == 0) std::fprintf(stderr, "note: bundled material maps not found, skipped\n");
    else CHECK(found == 6);
}

}  // namespace

int main() {
    testSynthetic();
    testBundled();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("ktx2_test: all passed");
    return 0;
}
