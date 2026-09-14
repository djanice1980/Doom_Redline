#include "game/assets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "game/procedural.h"
#ifdef REDLINE_HAVE_WAD
#include "wad/wad.h"
#endif

namespace rl::game {

namespace fs = std::filesystem;

std::optional<fs::path> Assets::findWad(const std::optional<fs::path>& explicitPath) {
    std::vector<fs::path> candidates;
    if (explicitPath) candidates.push_back(*explicitPath);
    if (const char* e = std::getenv("REDLINE_WAD")) candidates.emplace_back(e);
    std::error_code ec;
    if (fs::is_directory("wads", ec))
        for (auto& entry : fs::directory_iterator("wads", ec)) candidates.push_back(entry.path());
    const char* home = std::getenv("HOME");
    if (home) {
        std::string h = home;
        const char* names[] = {"doom.wad", "DOOM.WAD", "doom2.wad", "DOOM2.WAD", "freedoom1.wad", "freedoom2.wad"};
        const char* dirs[] = {"/.steam/steam/steamapps/common/Ultimate Doom/rerelease/",
                              "/.steam/steam/steamapps/common/Ultimate Doom/base/",
                              "/.local/share/Steam/steamapps/common/Ultimate Doom/rerelease/",
                              "/.steam/steam/steamapps/common/Doom 2/rerelease/",
                              "/.steam/steam/steamapps/common/Doom 2/base/",
                              "/.local/share/games/doom/", "/.config/gzdoom/"};
        for (const char* d : dirs)
            for (const char* n : names) candidates.emplace_back(h + d + n);
        candidates.emplace_back("/usr/share/games/doom/freedoom1.wad");
        candidates.emplace_back("/usr/share/games/doom/doom.wad");
    }
    for (const fs::path& p : candidates) {
        std::string ext = p.extension().string();
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext != ".wad") continue;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return std::nullopt;
}

int Assets::textWidth(const std::string& s, float scale) const {
    int w = 0;
    for (char ch : s) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        auto it = font.find(c);
        int gw = (it != font.end()) ? atlas_.get(it->second).w : fontHeight / 2;
        w += static_cast<int>((gw + 1) * scale);
    }
    return w;
}

bool Assets::load(const std::optional<fs::path>& wadPath, audio::Audio& audio) {
    bool ok = false;
#ifdef REDLINE_HAVE_WAD
    if (wadPath) ok = loadFromWad(*wadPath, audio);
#endif
    if (!ok) loadProcedural(audio);
    if (!atlas_.build(4096)) {
        std::fprintf(stderr, "[assets] atlas overflow\n");
        return false;
    }
    std::fprintf(stderr, "[assets] atlas %dx%d, %zu regions, source: %s\n", atlas_.image().width, atlas_.image().height, atlas_.count(),
                 usingWad_ ? wadName_.c_str() : "procedural");
    return true;
}

// ---------------------------------------------------------------------------
void Assets::loadProcedural(audio::Audio& audio) {
    usingWad_ = false;
    atlas_.add("block", proc::blockTexture(32));
    atlas_.add("block_red", proc::redBlockTexture(32));
    atlas_.add("wall", proc::wallTexture(64));
    atlas_.add("floor", proc::floorTexture(64));
    atlas_.add("ceiling", proc::wallTexture(64));
    atlas_.add("crosshair", proc::crosshair(15));

    auto anim = [&](SpriteAnim& a, const std::string& prefix, int count, float fps, auto gen) {
        a = {};
        a.fps = fps;
        for (int i = 0; i < count; ++i) {
            std::string key = prefix + std::to_string(i);
            atlas_.add(key, gen(i));
            a.frames.push_back(key);
            a.mirrored.push_back(false);
        }
    };
    anim(enemyIdle, "enemy_idle", 2, 3.f, [](int i) { return proc::enemyFrame(i == 0 ? 0 : 2, 64); });
    anim(enemyAttack, "enemy_attack", 1, 4.f, [](int) { return proc::enemyFrame(1, 64); });
    anim(enemyPain, "enemy_pain", 1, 4.f, [](int) { return proc::enemyFrame(2, 64); });
    anim(enemyDeath, "enemy_death", 4, 8.f, [](int i) { return proc::enemyFrame(3 + i, 64); });
    anim(gunIdle, "gun_idle", 1, 1.f, [](int) { return proc::gunFrame(0); });
    anim(gunFire, "gun_fire", 2, 10.f, [](int i) { return proc::gunFrame(1 + i); });
    anim(gunFlash, "gun_flash", 1, 10.f, [](int) { return proc::muzzleFlash(48); });
    anim(explosion, "explosion", 5, 14.f, [](int i) { return proc::explosionFrame(i, 64); });
    anim(fireball, "fireball", 1, 1.f, [](int) { return proc::fireball(16); });
    anim(fireballHit, "fireball_hit", 3, 14.f, [](int i) { return proc::explosionFrame(i, 32); });

    font.clear();
    for (auto& [ch, img] : proc::font(1)) {
        std::string key = std::string("font_") + std::to_string(static_cast<int>(ch));
        atlas_.add(key, img);
        font[ch] = key;
    }
    fontHeight = 7;
    fontScale = 3.f;
    title.clear();

    auto add = [&](const char* name, proc::Sound s) { audio.addSound(name, s.rate, std::move(s.samples)); };
    add("shoot", proc::sndShoot());
    add("explode", proc::sndExplode());
    add("hit", proc::sndHit());
    add("move", proc::sndMove());
    add("rotate", proc::sndRotate());
    add("lock", proc::sndLock());
    add("clear", proc::sndClear());
    add("redline", proc::sndRedLine());
    add("pain", proc::sndPain());
    add("enemy_die", proc::sndEnemyDie());
    add("enemy_pain", proc::sndHit());
    add("enemy_sight", proc::sndRedLine());
    add("levelup", proc::sndLevelUp());
    add("fireball", proc::sndFireball());
    add("fireball_hit", proc::sndHit());
    add("gameover", proc::sndGameOver());
}

// ---------------------------------------------------------------------------
#ifdef REDLINE_HAVE_WAD
bool Assets::loadFromWad(const fs::path& path, audio::Audio& audio) {
    auto wad = wad::Wad::load(path);
    if (!wad) {
        std::fprintf(stderr, "[assets] cannot read WAD %s\n", path.string().c_str());
        return false;
    }
    auto pal = wad::loadPalette(*wad, 0);
    if (!pal) {
        std::fprintf(stderr, "[assets] WAD has no PLAYPAL\n");
        return false;
    }
    std::fprintf(stderr, "[assets] %s: %s\n", path.filename().string().c_str(), wad::describe(*wad).c_str());
    usingWad_ = true;
    wadName_ = path.filename().string();

    // Block-mode tiles are ours; the arena uses Doom textures/flats.
    atlas_.add("block", proc::blockTexture(32));
    atlas_.add("block_red", proc::redBlockTexture(32));
    atlas_.add("crosshair", proc::crosshair(15));

    auto textures = wad::TextureSet::load(*wad, *pal);
    auto addTexture = [&](const std::string& key, std::initializer_list<const char*> names, Image fallback) {
        if (textures)
            for (const char* n : names)
                if (auto img = textures->get(n)) { atlas_.add(key, *img); return; }
        atlas_.add(key, fallback);
    };
    auto addFlat = [&](const std::string& key, std::initializer_list<const char*> names, Image fallback) {
        for (const char* n : names) {
            auto bytes = wad->data(n);
            if (bytes.empty()) continue;
            if (auto img = wad::decodeFlat(bytes, *pal)) { atlas_.add(key, *img); return; }
        }
        atlas_.add(key, fallback);
    };
    addTexture("wall", {"STARTAN3", "STARG3", "BROWN1", "STONE2"}, proc::wallTexture(64));
    addFlat("floor", {"FLOOR4_8", "FLAT5_4", "FLOOR0_1"}, proc::floorTexture(64));
    addFlat("ceiling", {"CEIL3_5", "FLAT20", "CEIL5_1"}, proc::wallTexture(64));

    // Sprite animations: imp (TROO), shotgun (SHTG/SHTF), rocket explosion (MISL), imp fireball (BAL1).
    auto addSprite = [&](SpriteAnim& a, const char* sprite, const std::string& prefix, const char* frameLetters, float fps) {
        a = {};
        a.fps = fps;
        auto set = wad::SpriteSet::load(*wad, *pal, sprite);
        if (!set) return false;
        for (const char* f = frameLetters; *f; ++f) {
            auto view = set->get(*f, 1);
            if (!view || !view->image) continue;
            std::string key = prefix + std::string(1, *f);
            atlas_.add(key, *view->image);
            a.frames.push_back(key);
            a.mirrored.push_back(view->mirrored);
        }
        return !a.frames.empty();
    };
    bool ok = true;
    ok &= addSprite(enemyIdle, "TROO", "troo_", "ABCD", 5.f);
    ok &= addSprite(enemyAttack, "TROO", "troo_", "EFG", 6.f);
    ok &= addSprite(enemyPain, "TROO", "troo_", "H", 6.f);
    ok &= addSprite(enemyDeath, "TROO", "troo_", "IJKL", 9.f);
    ok &= addSprite(gunIdle, "SHTG", "shtg_", "A", 1.f);
    ok &= addSprite(gunFire, "SHTG", "shtg_", "BCDCB", 9.f);
    ok &= addSprite(gunFlash, "SHTF", "shtf_", "AB", 12.f);
    ok &= addSprite(explosion, "MISL", "misl_", "BCD", 12.f);
    ok &= addSprite(fireball, "BAL1", "bal1_", "AB", 8.f);
    ok &= addSprite(fireballHit, "BAL1", "bal1_", "CDE", 14.f);
    if (!ok) {
        std::fprintf(stderr, "[assets] WAD is missing expected sprites; falling back to procedural art\n");
        atlas_ = {};
        return false;
    }
    // Rocket explosion frames have their origin at the centre already; the
    // enemy/fireball ones are used as-is.

    if (auto pic = wad::loadPatch(*wad, *pal, "M_DOOM")) {
        atlas_.add("title_doom", *pic);
        title = "title_doom";
    }

    font.clear();
    auto glyphs = wad::loadFont(*wad, *pal, "STCFN");
    int maxH = 0;
    for (auto& [ch, img] : glyphs) {
        // Doom's font is red; convert to white intensity so HUD tints work.
        for (int i = 0; i < img.width * img.height; ++i) {
            uint8_t* p = &img.rgba[static_cast<size_t>(i) * 4];
            uint8_t v = std::max({p[0], p[1], p[2]});
            p[0] = p[1] = p[2] = v;
        }
        std::string key = std::string("font_") + std::to_string(static_cast<int>(static_cast<unsigned char>(ch)));
        atlas_.add(key, img);
        font[ch] = key;
        maxH = std::max(maxH, img.height);
    }
    if (font.empty()) {
        for (auto& [ch, img] : proc::font(1)) {
            std::string key = std::string("font_") + std::to_string(static_cast<int>(ch));
            atlas_.add(key, img);
            font[ch] = key;
        }
        maxH = 7;
    }
    fontHeight = maxH > 0 ? maxH : 8;
    fontScale = 3.f;

    // Sounds: prefer the WAD, fall back to procedural for anything missing.
    auto addSound = [&](const char* name, std::initializer_list<const char*> lumps, proc::Sound fallback) {
        for (const char* l : lumps) {
            if (auto s = wad::loadSound(*wad, l)) {
                audio.addSound(name, s->sampleRate, std::move(s->samples));
                return;
            }
        }
        audio.addSound(name, fallback.rate, std::move(fallback.samples));
    };
    addSound("shoot", {"DSSHOTGN"}, proc::sndShoot());
    addSound("explode", {"DSBAREXP"}, proc::sndExplode());
    addSound("hit", {"DSITEMUP"}, proc::sndHit());
    addSound("move", {"DSPSTOP"}, proc::sndMove());
    addSound("rotate", {"DSSWTCHN"}, proc::sndRotate());
    addSound("lock", {"DSSTNMOV"}, proc::sndLock());
    addSound("clear", {"DSITEMUP"}, proc::sndClear());
    addSound("redline", {"DSDMACT"}, proc::sndRedLine());
    addSound("pain", {"DSPLPAIN"}, proc::sndPain());
    addSound("enemy_die", {"DSBGDTH1", "DSBGDTH2"}, proc::sndEnemyDie());
    addSound("enemy_pain", {"DSPOPAIN"}, proc::sndHit());
    addSound("enemy_sight", {"DSBGSIT1", "DSBGSIT2"}, proc::sndRedLine());
    addSound("levelup", {"DSGETPOW"}, proc::sndLevelUp());
    addSound("fireball", {"DSFIRSHT"}, proc::sndFireball());
    addSound("fireball_hit", {"DSFIRXPL"}, proc::sndHit());
    addSound("gameover", {"DSPDIEHI", "DSPLDETH"}, proc::sndGameOver());
    return true;
}
#endif

}  // namespace rl::game
