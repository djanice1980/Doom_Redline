// Unit test for rl::wad. Builds a small synthetic WAD in memory (no real WAD
// is required), exercises every public entry point, and feeds truncated and
// garbage buffers to the decoders to make sure they never crash.
#include "wad/wad.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace rl::wad;

// ---------------------------------------------------------------------------
// Tiny check framework
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        auto va_ = (a);                                                          \
        auto vb_ = (b);                                                          \
        if (!(va_ == vb_)) {                                                     \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d: %s == %s  (got %lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                        static_cast<long long>(va_), static_cast<long long>(vb_)); \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Byte helpers / WAD builder
// ---------------------------------------------------------------------------

using Bytes = std::vector<uint8_t>;

static void putU16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
static void putI16(Bytes& b, int16_t v) { putU16(b, static_cast<uint16_t>(v)); }
static void putU32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
static void putI32(Bytes& b, int32_t v) { putU32(b, static_cast<uint32_t>(v)); }
static void putName(Bytes& b, std::string_view name) {
    for (size_t i = 0; i < 8; ++i) b.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : 0);
}

struct WadBuilder {
    struct Entry {
        std::string name;
        Bytes data;
    };
    std::vector<Entry> entries;

    void add(std::string name, Bytes data = {}) { entries.push_back({std::move(name), std::move(data)}); }

    Bytes build(bool iwad) const {
        Bytes out;
        const char* id = iwad ? "IWAD" : "PWAD";
        out.insert(out.end(), id, id + 4);
        putI32(out, static_cast<int32_t>(entries.size()));
        putI32(out, 0);  // directory offset, patched below
        std::vector<uint32_t> offsets;
        for (const Entry& e : entries) {
            offsets.push_back(static_cast<uint32_t>(out.size()));
            out.insert(out.end(), e.data.begin(), e.data.end());
        }
        uint32_t dirOff = static_cast<uint32_t>(out.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            putU32(out, offsets[i]);
            putU32(out, static_cast<uint32_t>(entries[i].data.size()));
            putName(out, entries[i].name);
        }
        Bytes dir;
        putU32(dir, dirOff);
        std::memcpy(out.data() + 8, dir.data(), 4);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Patch encoder (inverse of decodePatch): grid of palette index or -1
// ---------------------------------------------------------------------------

struct Grid {
    int w, h;
    std::vector<int> px;  // -1 = transparent, else palette index
    Grid(int w_, int h_, int fill = -1) : w(w_), h(h_), px(static_cast<size_t>(w_ * h_), fill) {}
    int& at(int x, int y) { return px[static_cast<size_t>(y * w + x)]; }
    int at(int x, int y) const { return px[static_cast<size_t>(y * w + x)]; }
};

static Bytes encodePatch(const Grid& g, int16_t left = 0, int16_t top = 0) {
    Bytes out;
    putI16(out, static_cast<int16_t>(g.w));
    putI16(out, static_cast<int16_t>(g.h));
    putI16(out, left);
    putI16(out, top);
    size_t tablePos = out.size();
    for (int x = 0; x < g.w; ++x) putU32(out, 0);  // placeholders

    for (int x = 0; x < g.w; ++x) {
        uint32_t colOff = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + tablePos + static_cast<size_t>(x) * 4, &colOff, 4);  // host is LE; fine for a test
        int y = 0;
        while (y < g.h) {
            if (g.at(x, y) < 0) {
                ++y;
                continue;
            }
            int start = y;
            while (y < g.h && g.at(x, y) >= 0) ++y;
            out.push_back(static_cast<uint8_t>(start));         // topdelta
            out.push_back(static_cast<uint8_t>(y - start));     // length
            out.push_back(0);                                   // pad
            for (int k = start; k < y; ++k) out.push_back(static_cast<uint8_t>(g.at(x, k)));
            out.push_back(0);                                   // pad
        }
        out.push_back(0xFF);  // end of column
    }
    return out;
}

// The synthetic palette: index i -> (i, 255-i, i/2)
static Bytes makePlaypal() {
    Bytes b;
    for (int p = 0; p < 14; ++p) {
        for (int i = 0; i < 256; ++i) {
            if (p == 0) {
                b.push_back(static_cast<uint8_t>(i));
                b.push_back(static_cast<uint8_t>(255 - i));
                b.push_back(static_cast<uint8_t>(i / 2));
            } else {  // other palettes: something recognisably different
                b.push_back(static_cast<uint8_t>(255 - i));
                b.push_back(static_cast<uint8_t>(i));
                b.push_back(static_cast<uint8_t>(p));
            }
        }
    }
    return b;
}

static bool pixelIs(const rl::Image& img, int x, int y, int palIndex) {
    const uint8_t* p = img.px(x, y);
    return p[0] == palIndex && p[1] == 255 - palIndex && p[2] == palIndex / 2 && p[3] == 255;
}
static bool pixelTransparent(const rl::Image& img, int x, int y) { return img.px(x, y)[3] == 0; }

static Bytes dmxSound(uint16_t rate, uint32_t declared, const Bytes& pcm) {
    Bytes b;
    putU16(b, 3);
    putU16(b, rate);
    putU32(b, declared);
    b.insert(b.end(), pcm.begin(), pcm.end());
    return b;
}

// ---------------------------------------------------------------------------
// Build the synthetic WAD
// ---------------------------------------------------------------------------

static Bytes buildTestWad(bool iwad) {
    WadBuilder w;
    w.add("PLAYPAL", makePlaypal());

    // Duplicate names: the second must win.
    w.add("DUPE", {1, 2, 3});
    w.add("DUPE", {9, 9});

    // Sprites -------------------------------------------------------------
    w.add("S_START");
    {
        // TROOA0: 4x3, rotation 0 (all angles), offsets (2,1)
        Grid g(4, 3);
        g.at(0, 0) = 5; g.at(3, 0) = 7;
        g.at(1, 1) = 6; g.at(2, 1) = 6;
        for (int x = 0; x < 4; ++x) g.at(x, 2) = 8;
        w.add("TROOA0", encodePatch(g, 2, 1));
    }
    w.add("TROOB1", encodePatch(Grid(2, 2, 1)));
    {
        Grid g(2, 2);
        g.at(0, 0) = 2;
        w.add("TROOB2B8", encodePatch(g));  // rotation 2 as-is, rotation 8 mirrored
    }
    w.add("TROOB3", encodePatch(Grid(2, 2, 3)));
    w.add("JUNK", {1, 2, 3});                     // odd lump between the markers
    w.add("TROOPX", encodePatch(Grid(1, 1, 4)));  // bad rotation char: ignored
    w.add("POSSA1", encodePatch(Grid(3, 3, 9)));  // a different sprite
    w.add("S_END");
    w.add("SS_START");
    w.add("TROOC0", encodePatch(Grid(5, 1, 11)));
    w.add("SS_END");

    // Flats ---------------------------------------------------------------
    w.add("F_START");
    {
        Bytes flat(4096);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) flat[static_cast<size_t>(y * 64 + x)] = static_cast<uint8_t>((x + y) & 255);
        w.add("FLOOR0_1", flat);
    }
    w.add("F_END");

    // Font ----------------------------------------------------------------
    {
        Grid g(3, 5, 12);
        g.at(1, 1) = -1;
        w.add("STCFN065", encodePatch(g, 0, 0));  // 'A'
    }

    // Sounds --------------------------------------------------------------
    {
        // Vanilla-style padded: 16 copies of first sample, 100 samples, 16 copies of last.
        Bytes pcm;
        Bytes real;
        for (int i = 0; i < 100; ++i) real.push_back(static_cast<uint8_t>(i == 0 ? 200 : (i * 7) & 255));
        for (int i = 0; i < 16; ++i) pcm.push_back(real.front());
        pcm.insert(pcm.end(), real.begin(), real.end());
        for (int i = 0; i < 16; ++i) pcm.push_back(real.back());
        w.add("DSPISTOL", dmxSound(11025, static_cast<uint32_t>(pcm.size()), pcm));
    }
    {
        Bytes pcm;
        for (int i = 0; i < 50; ++i) pcm.push_back(static_cast<uint8_t>((i * 13) & 255));
        w.add("DSUNPAD", dmxSound(22050, 50, pcm));
    }
    {
        Bytes pcm(20, 128);
        w.add("DSCLAMP", dmxSound(11025, 1000, pcm));  // claims more than it has
    }
    w.add("DSBAD", {3, 0, 1});  // too short for a header
    {
        Bytes b;
        putU16(b, 0);  // PC-speaker format, not DMX
        putU16(b, 11025);
        putU32(b, 4);
        b.insert(b.end(), {1, 2, 3, 4});
        w.add("DSFMT0", b);
    }

    // Textures ------------------------------------------------------------
    w.add("P_START");
    w.add("WALL1", encodePatch(Grid(4, 4, 10)));
    {
        Grid g(4, 4, 20);
        g.at(0, 0) = -1;  // one transparent pixel: must NOT overwrite what is below
        w.add("WALL2", encodePatch(g));
    }
    w.add("P_END");
    {
        Bytes p;
        putI32(p, 2);
        putName(p, "WALL1");
        putName(p, "wall2");  // lowercase, like some entries in the real IWADs
        w.add("PNAMES", p);
    }
    {
        // TEX1 8x8: WALL1 at (0,0), WALL2 at (2,2). TEX2 4x4: WALL2 at (-2,-2).
        Bytes t;
        putI32(t, 2);
        putI32(t, 4 + 8);              // offset of TEX1
        putI32(t, 4 + 8 + 22 + 2 * 10);  // offset of TEX2
        auto tex = [&](std::string_view name, int16_t wdt, int16_t hgt, std::vector<std::array<int16_t, 3>> patches) {
            putName(t, name);
            putI32(t, 0);  // masked
            putI16(t, wdt);
            putI16(t, hgt);
            putI32(t, 0);  // columndirectory
            putI16(t, static_cast<int16_t>(patches.size()));
            for (auto& p : patches) {
                putI16(t, p[0]);
                putI16(t, p[1]);
                putI16(t, p[2]);
                putI16(t, 1);  // stepdir
                putI16(t, 0);  // colormap
            }
        };
        tex("TEX1", 8, 8, {{0, 0, 0}, {2, 2, 1}});
        tex("TEX2", 4, 4, {{-2, -2, 1}});
        w.add("TEXTURE1", t);
    }

    return w.build(iwad);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testContainer(const Wad& wad) {
    CHECK(wad.isIwad());
    CHECK_EQ(wad.lumps().size(), 30u);

    // Case-insensitive find, last-wins.
    CHECK(wad.find("playpal") != nullptr);
    CHECK(wad.find("PlayPal") == wad.find("PLAYPAL"));
    CHECK(wad.find("NOPE") == nullptr);
    CHECK(wad.find("PLAYPALXX") == nullptr);  // longer than 8 chars can never match
    const Lump* dupe = wad.find("dupe");
    CHECK(dupe != nullptr);
    if (dupe) {
        CHECK_EQ(dupe->size, 2u);
        auto d = wad.data(*dupe);
        CHECK(d.size() == 2 && d[0] == 9 && d[1] == 9);
    }
    CHECK(wad.data("missing").empty());
    CHECK_EQ(wad.data("PLAYPAL").size(), 14u * 768);

    // between()
    auto s = wad.between("S_START", "S_END");
    CHECK_EQ(s.size(), 7u);
    if (s.size() == 7) {
        CHECK(s[0]->name == "TROOA0");
        CHECK(s[4]->name == "JUNK");
        CHECK(s[6]->name == "POSSA1");
    }
    auto f = wad.between("f_start", "f_end");
    CHECK_EQ(f.size(), 1u);
    if (!f.empty()) CHECK(f[0]->name == "FLOOR0_1");
    CHECK(wad.between("X_START", "X_END").empty());

    // Name helpers
    CHECK(normalizeName(std::string_view("abc\0\0\0\0\0", 8)) == "ABC");
    CHECK(normalizeName("TOOLONGNAME") == "TOOLONGN");
    CHECK(nameEquals("s_start", "S_START"));
    CHECK(!nameEquals("S_START", "S_END"));

    std::string desc = describe(wad);
    CHECK(desc.find("IWAD, 30 lumps") == 0);
    CHECK(desc.find("2 sprites") != std::string::npos);
    CHECK(desc.find("2 textures") != std::string::npos);
    CHECK(desc.find("1 flats") != std::string::npos);
    CHECK(desc.find("1 font glyphs") != std::string::npos);
    CHECK(desc.find("4 sounds") != std::string::npos);
    std::printf("describe: %s\n", desc.c_str());

    auto names = spriteNames(wad);
    CHECK(names.size() == 2 && names[0] == "TROO" && names[1] == "POSS");
}

static void testPalette(const Wad& wad) {
    auto pal = loadPalette(wad);
    CHECK(pal.has_value());
    if (pal) {
        CHECK(pal->rgb[10][0] == 10 && pal->rgb[10][1] == 245 && pal->rgb[10][2] == 5);
        CHECK(pal->rgb[255][0] == 255 && pal->rgb[255][1] == 0);
    }
    auto pal1 = loadPalette(wad, 1);
    CHECK(pal1.has_value());
    if (pal1) CHECK(pal1->rgb[10][0] == 245 && pal1->rgb[10][2] == 1);
    CHECK(loadPalette(wad, 13).has_value());
    CHECK(!loadPalette(wad, 14).has_value());
    CHECK(!loadPalette(wad, -1).has_value());
}

static void testPatchAndFlat(const Wad& wad, const Palette& pal) {
    auto img = loadPatch(wad, pal, "trooa0");
    CHECK(img.has_value());
    if (img) {
        CHECK_EQ(img->width, 4);
        CHECK_EQ(img->height, 3);
        CHECK_EQ(img->offsetX, 2);
        CHECK_EQ(img->offsetY, 1);
        CHECK(pixelIs(*img, 0, 0, 5));
        CHECK(pixelTransparent(*img, 1, 0));
        CHECK(pixelTransparent(*img, 2, 0));
        CHECK(pixelIs(*img, 3, 0, 7));
        CHECK(pixelTransparent(*img, 0, 1));
        CHECK(pixelIs(*img, 1, 1, 6));
        CHECK(pixelIs(*img, 2, 1, 6));
        CHECK(pixelTransparent(*img, 3, 1));
        for (int x = 0; x < 4; ++x) CHECK(pixelIs(*img, x, 2, 8));
    }
    CHECK(!loadPatch(wad, pal, "NOSUCH").has_value());

    auto flat = decodeFlat(wad.data("FLOOR0_1"), pal);
    CHECK(flat.has_value());
    if (flat) {
        CHECK_EQ(flat->width, 64);
        CHECK_EQ(flat->height, 64);
        CHECK(pixelIs(*flat, 10, 20, 30));
        CHECK(pixelIs(*flat, 63, 63, 126));
        CHECK_EQ(flat->offsetX, 0);
    }
}

static void testSprites(const Wad& wad, const Palette& pal) {
    auto set = SpriteSet::load(wad, pal, "troo");
    CHECK(set.has_value());
    if (!set) return;
    CHECK(set->name() == "TROO");
    auto frames = set->frames();
    CHECK(frames.size() == 3 && frames[0] == 'A' && frames[1] == 'B' && frames[2] == 'C');
    CHECK_EQ(set->imageCount(), 5u);  // A0, B1, B2B8, B3, C0

    // Rotation 0 serves every angle.
    auto a1 = set->get('A', 1);
    auto a5 = set->get('A', 5);
    CHECK(a1 && a5 && a1->image == a5->image && !a1->mirrored && !a5->mirrored);
    if (a1) {
        CHECK_EQ(a1->image->width, 4);
        CHECK_EQ(a1->image->offsetX, 2);
        CHECK(pixelIs(*a1->image, 0, 0, 5));
    }

    // Specific rotations, mirrored reuse, fallback.
    auto b1 = set->get('B', 1);
    auto b2 = set->get('B', 2);
    auto b8 = set->get('B', 8);
    auto b3 = set->get('B', 3);
    auto b4 = set->get('B', 4);
    CHECK(b1 && !b1->mirrored);
    CHECK(b2 && !b2->mirrored);
    CHECK(b8 && b8->mirrored);
    CHECK(b2 && b8 && b2->image == b8->image);
    CHECK(b3 && !b3->mirrored && b3->image != b2->image);
    CHECK(b4 && b4->image == b1->image && !b4->mirrored);  // missing rotation -> rotation 1
    if (b1) CHECK(pixelIs(*b1->image, 0, 0, 1));
    if (b2) {
        CHECK(pixelIs(*b2->image, 0, 0, 2));
        CHECK(pixelTransparent(*b2->image, 1, 1));
    }
    auto b0 = set->get('b', 0);  // lowercase frame, rotation 0 request -> rotation 1
    CHECK(b0 && b0->image == b1->image);

    auto c = set->get('C', 7);
    CHECK(c && c->image->width == 5);
    CHECK(!set->get('Z').has_value());
    CHECK(!set->get('P').has_value());  // TROOPX was rejected
    CHECK(!set->get('B', 9).has_value());

    auto poss = SpriteSet::load(wad, pal, "POSS");
    CHECK(poss && poss->frames().size() == 1 && poss->imageCount() == 1);
    CHECK(!SpriteSet::load(wad, pal, "XXXX").has_value());
    CHECK(!SpriteSet::load(wad, pal, "TRO").has_value());
}

static void testFont(const Wad& wad, const Palette& pal) {
    auto font = loadFont(wad, pal);
    CHECK_EQ(font.size(), 1u);
    CHECK(font.count('A') == 1);
    CHECK(font.count('B') == 0);
    if (font.count('A')) {
        const rl::Image& g = font['A'];
        CHECK_EQ(g.width, 3);
        CHECK_EQ(g.height, 5);
        CHECK(pixelIs(g, 0, 0, 12));
        CHECK(pixelTransparent(g, 1, 1));
    }
    CHECK(loadFont(wad, pal, "NOFONT").empty());
}

static void testTextures(const Wad& wad, const Palette& pal) {
    auto set = TextureSet::load(wad, pal);
    CHECK(set.has_value());
    if (!set) return;
    auto names = set->names();
    CHECK(names.size() == 2 && names[0] == "TEX1" && names[1] == "TEX2");
    auto info = set->info("tex1");
    CHECK(info && info->width == 8 && info->height == 8);

    auto t1 = set->get("tex1");
    CHECK(t1.has_value());
    if (t1) {
        CHECK_EQ(t1->width, 8);
        CHECK_EQ(t1->height, 8);
        CHECK(pixelIs(*t1, 0, 0, 10));  // patch 1 only
        CHECK(pixelIs(*t1, 3, 3, 20));  // overlap: patch 2 overwrote patch 1
        CHECK(pixelIs(*t1, 2, 2, 10));  // patch 2's transparent pixel leaves patch 1 visible
        CHECK(pixelIs(*t1, 5, 5, 20));  // patch 2 only
        CHECK(pixelTransparent(*t1, 6, 6));
        CHECK(pixelTransparent(*t1, 7, 0));
        CHECK(pixelTransparent(*t1, 0, 7));
    }
    auto t2 = set->get("TEX2");
    CHECK(t2.has_value());
    if (t2) {
        CHECK(pixelIs(*t2, 0, 0, 20));  // patch at (-2,-2): visible part is its lower-right quadrant
        CHECK(pixelIs(*t2, 1, 1, 20));
        CHECK(pixelTransparent(*t2, 2, 2));
        CHECK(pixelTransparent(*t2, 3, 0));
    }
    CHECK(!set->get("NOPE").has_value());

    auto list = listTextures(wad);
    CHECK(list.size() == 2 && list[1].name == "TEX2" && list[1].width == 4);
}

static void testSounds(const Wad& wad) {
    auto s = loadSound(wad, "dspistol");
    CHECK(s.has_value());
    if (s) {
        CHECK_EQ(s->sampleRate, 11025);
        CHECK_EQ(s->samples.size(), 100u);  // padding stripped
        CHECK(std::abs(s->samples[0] - (200 - 128) / 128.0f) < 1e-6f);
        CHECK(std::abs(s->samples[1] - (7 - 128) / 128.0f) < 1e-6f);
    }
    auto u = loadSound(wad, "DSUNPAD");
    CHECK(u.has_value());
    if (u) {
        CHECK_EQ(u->sampleRate, 22050);
        CHECK_EQ(u->samples.size(), 50u);
        CHECK(std::abs(u->samples[1] - (13 - 128) / 128.0f) < 1e-6f);
    }
    auto c = loadSound(wad, "DSCLAMP");
    CHECK(c && c->samples.size() == 20);
    CHECK(!loadSound(wad, "DSBAD").has_value());
    CHECK(!loadSound(wad, "DSFMT0").has_value());
    CHECK(!loadSound(wad, "DSNOPE").has_value());
    CHECK_EQ(soundLumps(wad).size(), 4u);  // DSBAD is too short to count
}

// Deterministic pseudo-random bytes for the garbage tests.
struct Lcg {
    uint32_t s;
    uint8_t next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<uint8_t>(s >> 24);
    }
};

static void testRobustness(const Wad& wad, const Palette& pal) {
    // Zero length / tiny.
    CHECK(!decodePatch({}, pal).has_value());
    CHECK(!decodePatch(Bytes{1, 0, 1}, pal).has_value());
    CHECK(!decodeFlat({}, pal).has_value());
    CHECK(!decodeFlat(Bytes(4095, 0), pal).has_value());
    CHECK(decodeFlat(Bytes(4096, 0), pal).has_value());
    CHECK(decodeFlat(Bytes(4160, 0), pal).has_value());  // oversize flat is fine

    // Header claiming a huge width in a small buffer.
    {
        Bytes b;
        putI16(b, 30000);
        putI16(b, 10);
        putI16(b, 0);
        putI16(b, 0);
        b.resize(64, 0);
        CHECK(!decodePatch(b, pal).has_value());
    }
    // Negative / zero dimensions.
    {
        Bytes b;
        putI16(b, -4);
        putI16(b, 4);
        putI32(b, 0);
        b.resize(64, 0);
        CHECK(!decodePatch(b, pal).has_value());
        Bytes z(64, 0);
        CHECK(!decodePatch(z, pal).has_value());
    }
    // Column offsets past the end.
    {
        Bytes b;
        putI16(b, 2);
        putI16(b, 2);
        putI16(b, 0);
        putI16(b, 0);
        putU32(b, 0xFFFFFFFFu);
        putU32(b, 16);
        b.push_back(0xFF);
        CHECK(!decodePatch(b, pal).has_value());
    }
    // Post whose declared length overruns the lump.
    {
        Bytes b;
        putI16(b, 1);
        putI16(b, 4);
        putI16(b, 0);
        putI16(b, 0);
        putU32(b, 12);
        b.insert(b.end(), {0, 200, 0, 1, 2});
        CHECK(!decodePatch(b, pal).has_value());
    }
    // Missing terminator at the very end is tolerated.
    {
        Bytes b;
        putI16(b, 1);
        putI16(b, 2);
        putI16(b, 0);
        putI16(b, 0);
        putU32(b, 12);
        b.insert(b.end(), {0, 2, 0, 5, 6, 0});  // no 0xFF
        auto img = decodePatch(b, pal);
        CHECK(img && pixelIs(*img, 0, 1, 6));
    }
    // Tall-patch style column: second post's topdelta <= first -> cumulative.
    {
        Bytes b;
        putI16(b, 1);
        putI16(b, 6);
        putI16(b, 0);
        putI16(b, 0);
        putU32(b, 12);
        b.insert(b.end(), {1, 1, 0, 9, 0});  // post at row 1
        b.insert(b.end(), {1, 1, 0, 3, 0});  // topdelta 1 <= 1 -> row 2
        b.push_back(0xFF);
        auto img = decodePatch(b, pal);
        CHECK(img && pixelIs(*img, 0, 1, 9) && pixelIs(*img, 0, 2, 3) && pixelTransparent(*img, 0, 3));
    }
    // Posts running past the image height are clipped, not fatal.
    {
        Bytes b;
        putI16(b, 1);
        putI16(b, 2);
        putI16(b, 0);
        putI16(b, 0);
        putU32(b, 12);
        b.insert(b.end(), {0, 5, 0, 1, 2, 3, 4, 5, 0, 0xFF});
        auto img = decodePatch(b, pal);
        CHECK(img && pixelIs(*img, 0, 1, 2));
    }

    // Every truncation of a valid patch must not crash.
    Bytes good(wad.data("TROOA0").begin(), wad.data("TROOA0").end());
    for (size_t n = 0; n < good.size(); ++n) {
        Bytes t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        (void)decodePatch(t, pal);
        ++g_checks;
    }

    // Garbage buffers of assorted sizes through every decoder.
    Lcg rng{12345};
    for (int iter = 0; iter < 400; ++iter) {
        size_t n = static_cast<size_t>(rng.next()) * 3 + (iter % 7);
        Bytes g(n);
        for (auto& v : g) v = rng.next();
        (void)decodePatch(g, pal);
        (void)decodeFlat(g, pal);
        (void)Wad::fromMemory(g);
        WadBuilder b;
        b.add("DSX", g);
        b.add("PNAMES", g);
        b.add("TEXTURE1", g);
        b.add("PLAYPAL", g);
        b.add("S_START");
        b.add("TROOA0", g);
        b.add("S_END");
        auto gw = Wad::fromMemory(b.build(false));
        if (gw) {
            (void)loadSound(*gw, "DSX");
            (void)TextureSet::load(*gw, pal);
            (void)listTextures(*gw);
            (void)loadPalette(*gw);
            (void)SpriteSet::load(*gw, pal, "TROO");
            (void)describe(*gw);
        }
        ++g_checks;
    }
    // Garbage with a valid-looking patch header prefix, to reach the post parser.
    for (int iter = 0; iter < 400; ++iter) {
        Bytes g;
        int w = 1 + rng.next() % 8;
        int h = 1 + rng.next() % 64;
        putI16(g, static_cast<int16_t>(w));
        putI16(g, static_cast<int16_t>(h));
        putI16(g, 0);
        putI16(g, 0);
        size_t body = 8 + static_cast<size_t>(w) * 4 + rng.next();
        for (int x = 0; x < w; ++x) putU32(g, rng.next() % static_cast<uint32_t>(body));
        while (g.size() < body) g.push_back(rng.next());
        (void)decodePatch(g, pal);
        ++g_checks;
    }

    // Container-level corruption.
    CHECK(!Wad::fromMemory({}).has_value());
    CHECK(!Wad::fromMemory(Bytes{'X', 'W', 'A', 'D', 0, 0, 0, 0, 0, 0, 0, 0}).has_value());
    {
        auto empty = Wad::fromMemory(Bytes{'P', 'W', 'A', 'D', 0, 0, 0, 0, 12, 0, 0, 0});
        CHECK(empty && empty->lumps().empty() && !empty->isIwad());
        if (empty) CHECK(describe(*empty) == "PWAD, 0 lumps, 0 sprites, 0 textures, 0 flats, 0 font glyphs, 0 sounds");
    }
    {
        Bytes b{'I', 'W', 'A', 'D'};
        putI32(b, 5);
        putI32(b, 1000);  // directory past the end
        CHECK(!Wad::fromMemory(b).has_value());
    }
    {
        // numlumps far larger than what fits: clamped to the entries present.
        Bytes b{'I', 'W', 'A', 'D'};
        putI32(b, 1000000);
        putI32(b, 12);
        putI32(b, 5000);  // lump data offset past end of file
        putI32(b, 100);
        putName(b, "FOO");
        auto w = Wad::fromMemory(b);
        CHECK(w && w->lumps().size() == 1);
        if (w) {
            CHECK(w->find("FOO") != nullptr);
            CHECK(w->data("FOO").empty());
        }
    }
    {
        // Lump extending past the end of the file: size clamped.
        Bytes b{'I', 'W', 'A', 'D'};
        putI32(b, 1);
        putI32(b, 12);
        putI32(b, 28);
        putI32(b, 100);
        putName(b, "BAR");
        b.insert(b.end(), {1, 2, 3});
        auto w = Wad::fromMemory(b);
        CHECK(w && w->data("BAR").size() == 3);
    }
    // Truncations of the whole synthetic WAD.
    {
        Bytes full = buildTestWad(true);
        for (size_t n = 0; n < full.size(); n += 37) {
            Bytes t(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
            auto w = Wad::fromMemory(t);
            if (w) {
                (void)describe(*w);
                if (auto p = loadPalette(*w)) {
                    (void)SpriteSet::load(*w, *p, "TROO");
                    (void)TextureSet::load(*w, *p);
                    (void)loadFont(*w, *p);
                }
                (void)loadSound(*w, "DSPISTOL");
            }
            ++g_checks;
        }
    }
}

static void testFileLoad() {
    Bytes bytes = buildTestWad(false);
    std::filesystem::path p = std::filesystem::temp_directory_path() / "rl_wad_test_synthetic.wad";
    {
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (!f) return;
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    auto wad = Wad::load(p);
    CHECK(wad.has_value());
    if (wad) {
        CHECK(!wad->isIwad());
        CHECK_EQ(wad->lumps().size(), 30u);
        CHECK_EQ(wad->fileSize(), bytes.size());
        CHECK(wad->find("TROOB2B8") != nullptr);
    }
    CHECK(!Wad::load(p.parent_path() / "rl_wad_does_not_exist.wad").has_value());
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

int main() {
    auto wad = Wad::fromMemory(buildTestWad(true));
    CHECK(wad.has_value());
    if (!wad) {
        std::printf("cannot parse synthetic WAD\n");
        return 1;
    }
    auto pal = loadPalette(*wad);
    CHECK(pal.has_value());
    if (!pal) {
        std::printf("cannot load synthetic palette\n");
        return 1;
    }

    testContainer(*wad);
    testPalette(*wad);
    testPatchAndFlat(*wad, *pal);
    testSprites(*wad, *pal);
    testFont(*wad, *pal);
    testTextures(*wad, *pal);
    testSounds(*wad);
    testRobustness(*wad, *pal);
    testFileLoad();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
