// wadinfo — print an inventory of a Doom WAD using rl::wad.
//
//   wadinfo <file.wad>                       summary + sprite names + counts
//   wadinfo <file.wad> --sprites             also list every sprite's frames
//   wadinfo <file.wad> --textures            also list every texture with size
//   wadinfo <file.wad> --sprite TROO A 1     decode one sprite frame, print size/offsets
//   wadinfo <file.wad> --font [PREFIX]       decode a font, print glyph sizes
//   wadinfo <file.wad> --sound DSPISTOL      decode a sound, print rate/count
//   wadinfo <file.wad> --texture STARTAN3    compose one texture, print size/opaque count
//   wadinfo <file.wad> --lumps               dump the directory
#include "wad/wad.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace rl::wad;

static int usage() {
    std::fprintf(stderr,
                 "usage: wadinfo <file.wad> [--sprites] [--textures] [--lumps]\n"
                 "               [--sprite NAME FRAME ROT] [--font [PREFIX]]\n"
                 "               [--sound NAME] [--texture NAME]\n");
    return 2;
}

static size_t opaquePixels(const rl::Image& img) {
    size_t n = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) n += img.rgba[i] != 0;
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const char* path = argv[1];

    auto wad = Wad::load(path);
    if (!wad) {
        std::fprintf(stderr, "wadinfo: cannot read '%s' as a WAD\n", path);
        return 1;
    }
    std::printf("%s\n", describe(*wad).c_str());

    auto pal = loadPalette(*wad);
    if (!pal) std::printf("(no PLAYPAL: graphics cannot be decoded)\n");

    // ---- default inventory ------------------------------------------------
    std::vector<std::string> sprites = spriteNames(*wad);
    std::printf("Sprites (%zu):", sprites.size());
    for (const std::string& s : sprites) std::printf(" %s", s.c_str());
    std::printf("\n");
    std::printf("Textures: %zu\n", listTextures(*wad).size());
    std::printf("Flats: %zu\n", flatLumps(*wad).size());
    std::printf("Font glyphs (STCFN): %zu\n", fontLumps(*wad).size());
    std::printf("Sound lumps (DS*): %zu\n", soundLumps(*wad).size());

    // ---- optional detail flags ----------------------------------------------
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--lumps") {
            for (const Lump& l : wad->lumps()) std::printf("  %-8s off=%u size=%u\n", l.name.c_str(), l.offset, l.size);
        } else if (arg == "--sprites") {
            if (!pal) continue;
            for (const std::string& s : sprites) {
                auto set = SpriteSet::load(*wad, *pal, s);
                if (!set) continue;
                std::printf("  %s: %zu images, frames", s.c_str(), set->imageCount());
                for (char f : set->frames()) std::printf(" %c", f);
                std::printf("\n");
            }
        } else if (arg == "--textures") {
            for (const TextureInfo& t : listTextures(*wad)) std::printf("  %-8s %dx%d\n", t.name.c_str(), t.width, t.height);
        } else if (arg == "--sprite" && i + 3 < argc && pal) {
            const char* name = argv[i + 1];
            char frame = argv[i + 2][0];
            int rot = std::atoi(argv[i + 3]);
            i += 3;
            auto set = SpriteSet::load(*wad, *pal, name);
            if (!set) {
                std::printf("sprite %s: not found\n", name);
                continue;
            }
            std::printf("sprite %s: %zu images, frames", set->name().c_str(), set->imageCount());
            for (char f : set->frames()) std::printf(" %c", f);
            std::printf("\n");
            auto v = set->get(frame, rot);
            if (!v) {
                std::printf("  frame %c rotation %d: not found\n", frame, rot);
            } else {
                std::printf("  frame %c rotation %d: %dx%d offset (%d,%d) mirrored=%d opaque=%zu\n", frame, rot,
                            v->image->width, v->image->height, v->image->offsetX, v->image->offsetY,
                            v->mirrored ? 1 : 0, opaquePixels(*v->image));
            }
        } else if (arg == "--font" && pal) {
            std::string prefix = "STCFN";
            if (i + 1 < argc && argv[i + 1][0] != '-') prefix = argv[++i];
            auto font = loadFont(*wad, *pal, prefix);
            std::printf("font %s: %zu glyphs\n", prefix.c_str(), font.size());
            for (const auto& [c, img] : font) {
                std::printf("  %3d '%c' %dx%d offset (%d,%d)\n", static_cast<unsigned char>(c),
                            (c >= 32 && c < 127) ? c : '?', img.width, img.height, img.offsetX, img.offsetY);
            }
        } else if (arg == "--sound" && i + 1 < argc) {
            const char* name = argv[++i];
            auto snd = loadSound(*wad, name);
            if (!snd) {
                std::printf("sound %s: not found / not DMX\n", name);
            } else {
                std::printf("sound %s: %d Hz, %zu samples (%.3f s), first=%.3f\n", name, snd->sampleRate,
                            snd->samples.size(), static_cast<double>(snd->samples.size()) / snd->sampleRate,
                            snd->samples.empty() ? 0.0 : static_cast<double>(snd->samples[0]));
            }
        } else if (arg == "--verify" && pal) {
            // Decode everything decodable and report what fails: a quick way to
            // find parser assumptions that a real WAD breaks.
            size_t spriteLumps = 0, spriteFail = 0;
            auto checkSprites = [&](std::string_view start, std::string_view end) {
                for (const Lump* l : wad->between(start, end)) {
                    if (l->size == 0) continue;
                    ++spriteLumps;
                    if (!decodePatch(wad->data(*l), *pal)) {
                        ++spriteFail;
                        std::printf("  sprite lump %s (%u bytes) failed to decode\n", l->name.c_str(), l->size);
                    }
                }
            };
            checkSprites("S_START", "S_END");
            checkSprites("SS_START", "SS_END");
            size_t texFail = 0, texCount = 0;
            if (auto set = TextureSet::load(*wad, *pal)) {
                for (const std::string& n : set->names()) {
                    ++texCount;
                    auto img = set->get(n);
                    if (!img) {
                        ++texFail;
                        std::printf("  texture %s failed to compose\n", n.c_str());
                    }
                }
            }
            size_t flatFail = 0;
            auto flats = flatLumps(*wad);
            for (const Lump* l : flats) {
                if (!decodeFlat(wad->data(*l), *pal)) ++flatFail;
            }
            size_t soundFail = 0;
            auto sounds = soundLumps(*wad);
            for (const Lump* l : sounds) {
                if (!loadSound(*wad, l->name)) {
                    ++soundFail;
                    std::printf("  sound %s (%u bytes) failed to decode\n", l->name.c_str(), l->size);
                }
            }
            size_t glyphLumps = fontLumps(*wad).size();
            size_t glyphs = loadFont(*wad, *pal).size();
            std::printf("verify: sprites %zu/%zu ok, textures %zu/%zu ok, flats %zu/%zu ok, sounds %zu/%zu ok, glyphs %zu/%zu ok\n",
                        spriteLumps - spriteFail, spriteLumps, texCount - texFail, texCount, flats.size() - flatFail,
                        flats.size(), sounds.size() - soundFail, sounds.size(), glyphs, glyphLumps);
        } else if (arg == "--texture" && i + 1 < argc && pal) {
            const char* name = argv[++i];
            auto set = TextureSet::load(*wad, *pal);
            auto img = set ? set->get(name) : std::nullopt;
            if (!img) {
                std::printf("texture %s: not found\n", name);
            } else {
                std::printf("texture %s: %dx%d opaque=%zu/%d\n", name, img->width, img->height, opaquePixels(*img),
                            img->width * img->height);
            }
        } else {
            return usage();
        }
    }
    return 0;
}
