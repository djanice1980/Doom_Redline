// PNG reader / inflate checks: a round trip through the game's own writer (stored
// deflate blocks) and every sprite of the bundled community pack (real zlib streams).
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/png.h"
#include "core/png_read.h"

using rl::Image;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

std::vector<uint8_t> readAll(const std::filesystem::path& p) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

void testRoundTrip() {
    Image img(37, 23);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
            p[0] = static_cast<uint8_t>(x * 7); p[1] = static_cast<uint8_t>(y * 11); p[2] = static_cast<uint8_t>(x ^ y); p[3] = static_cast<uint8_t>(255 - x);
        }
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "redline_png_test.png";
    CHECK(rl::writePng(tmp.string(), img));
    std::string err;
    auto back = rl::decodePng(readAll(tmp), &err);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->width == img.width && back->height == img.height);
        CHECK(back->rgba == img.rgba);
    }
    std::filesystem::remove(tmp);
    // Garbage is rejected, not crashed on.
    std::vector<uint8_t> junk(100, 0x42);
    CHECK(!rl::decodePng(junk, &err));
}

void testBundled() {
    const std::filesystem::path dir = std::filesystem::path(REDLINE_SOURCE_DIR) / "assets" / "brutal" / "sprites";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) { std::fprintf(stderr, "note: no bundled pack, skipped\n"); return; }
    int count = 0, withOffsets = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".png") continue;
        std::string err;
        auto img = rl::decodePng(readAll(e.path()), &err);
        if (!img) std::fprintf(stderr, "%s: %s\n", e.path().filename().string().c_str(), err.c_str());
        CHECK(img.has_value());
        if (!img) continue;
        ++count;
        CHECK(img->width > 0 && img->height > 0);
        bool anyOpaque = false;
        for (size_t i = 3; i < img->rgba.size(); i += 4) if (img->rgba[i] > 0) { anyOpaque = true; break; }
        CHECK(anyOpaque);
        if (img->offsetX != 0 || img->offsetY != 0) ++withOffsets;
    }
    CHECK(count >= 90);
    CHECK(withOffsets > count / 2);   // sprites carry grAb offsets
    std::fprintf(stderr, "decoded %d pack sprites (%d with offsets)\n", count, withOffsets);
}
}  // namespace

int main() {
    testRoundTrip();
    testBundled();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("png_test: all passed");
    return 0;
}
