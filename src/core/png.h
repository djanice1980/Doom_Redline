#pragma once
// Minimal PNG writer (uncompressed deflate "stored" blocks). Used for
// screenshots and atlas dumps; no external dependency.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/image.h"

namespace rl {

namespace png_detail {
inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
inline void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8)); v.push_back(static_cast<uint8_t>(x));
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32(&out[start], out.size() - start) ^ 0xFFFFFFFFu;
    be32(out, crc);
}
}  // namespace png_detail

inline bool writePng(const std::string& path, const Image& img) {
    using namespace png_detail;
    if (!img.valid()) return false;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(img.height) * (img.width * 4 + 1));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), img.px(0, y), img.px(0, y) + static_cast<size_t>(img.width) * 4);
    }
    // zlib stream with stored blocks (max 65535 bytes each)
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF)); z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF)); z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (raw.empty()) break;
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, static_cast<uint32_t>(img.width));
    be32(ihdr, static_cast<uint32_t>(img.height));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace rl
