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

namespace {
const char* kTierNames[kEnemyTiers] = {"ZOMBIE", "IMP", "DEMON", "CACODEMON", "BARON"};
}

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
            if (!atlas_.has(key)) atlas_.add(key, gen(i));
            a.frames.push_back(key);
            a.mirrored.push_back(false);
        }
    };
    // One procedural monster, scaled and tinted per tier.
    const glm::vec4 tints[kEnemyTiers] = {{0.7f, 0.7f, 0.8f, 1.f}, {1.f, 1.f, 1.f, 1.f}, {1.f, 0.6f, 0.6f, 1.f}, {1.f, 0.4f, 0.4f, 1.f}, {0.5f, 1.f, 0.5f, 1.f}};
    const float sizes[kEnemyTiers] = {0.026f, 0.031f, 0.034f, 0.045f, 0.055f};
    for (int t = 0; t < kEnemyTiers; ++t) {
        EnemyArt& e = enemies[t];
        e.name = kTierNames[t];
        e.tint = tints[t];
        e.metresPerPixel = sizes[t];
        anim(e.walk, "enemy_idle", 2, 3.f, [](int i) { return proc::enemyFrame(i == 0 ? 0 : 2, 64); });
        anim(e.attack, "enemy_attack", 1, 4.f, [](int) { return proc::enemyFrame(1, 64); });
        anim(e.pain, "enemy_pain", 1, 4.f, [](int) { return proc::enemyFrame(2, 64); });
        anim(e.death, "enemy_death", 4, 8.f, [](int i) { return proc::enemyFrame(3 + i, 64); });
        e.sightSound = "enemy_sight";
        e.painSound = "enemy_pain";
        e.deathSound = "enemy_die";
        e.attackSound = t == 0 ? "shoot" : "fireball";
    }
    const char* weaponNames[kWeaponArt] = {"SHOTGUN", "CHAINGUN", "ROCKET LAUNCHER", "PLASMA RIFLE"};
    const glm::vec4 weaponTints[kWeaponArt] = {{1.f, 1.f, 1.f, 1.f}, {0.8f, 0.8f, 0.9f, 1.f}, {0.6f, 0.9f, 0.6f, 1.f}, {0.6f, 0.7f, 1.f, 1.f}};
    const char* weaponSounds[kWeaponArt] = {"shoot", "fire_chain", "fire_rocket", "fire_plasma"};
    for (int w = 0; w < kWeaponArt; ++w) {
        WeaponArt& a = weapons[w];
        a.name = weaponNames[w];
        a.tint = weaponTints[w];
        a.fireSound = weaponSounds[w];
        anim(a.idle, "gun_idle", 1, 1.f, [](int) { return proc::gunFrame(0); });
        anim(a.fire, "gun_fire", 2, 10.f, [](int i) { return proc::gunFrame(1 + i); });
        anim(a.flash, "gun_flash", 1, 10.f, [](int) { return proc::muzzleFlash(48); });
    }
    const uint8_t pickupCols[kPickupArt][3] = {{200, 60, 60}, {230, 230, 230}, {200, 170, 60}, {120, 120, 120}, {80, 160, 240}, {160, 160, 170}, {90, 140, 90}, {90, 110, 200}};
    for (int k = 0; k < kPickupArt; ++k) {
        std::string key = "pickup" + std::to_string(k);
        Image img = proc::solid(k >= 5 ? 28 : 14, k >= 5 ? 12 : 10, pickupCols[k][0], pickupCols[k][1], pickupCols[k][2]);
        img.offsetX = img.width / 2;
        img.offsetY = img.height;
        atlas_.add(key, img);
        pickups[k] = {};
        pickups[k].frames.push_back(key);
        pickups[k].mirrored.push_back(false);
    }
    anim(explosion, "explosion", 5, 14.f, [](int i) { return proc::explosionFrame(i, 64); });
    for (int p = 0; p < kProjectileTypes; ++p) {
        anim(projectile[p], "fireball", 1, 1.f, [](int) { return proc::fireball(16); });
        anim(projectileHit[p], "fireball_hit", 3, 14.f, [](int i) { return proc::explosionFrame(i, 32); });
    }

    font.clear();
    for (auto& [ch, img] : proc::font(1)) {
        std::string key = std::string("font_") + std::to_string(static_cast<int>(ch));
        atlas_.add(key, img);
        font[ch] = key;
    }
    fontHeight = 7;
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
    add("menu", proc::sndMove());
    add("menu_select", proc::sndRotate());
    add("fire_chain", proc::sndHit());
    add("fire_rocket", proc::sndShoot());
    add("fire_plasma", proc::sndFireball());
    add("pickup_item", proc::sndClear());
    add("pickup_weapon", proc::sndLevelUp());
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

    auto addSprite = [&](SpriteAnim& a, const char* sprite, const char* frameLetters, float fps) {
        a = {};
        a.fps = fps;
        auto set = wad::SpriteSet::load(*wad, *pal, sprite);
        if (!set) return false;
        for (const char* f = frameLetters; *f; ++f) {
            auto view = set->get(*f, 1);
            if (!view || !view->image) continue;
            std::string key = std::string(sprite) + "_" + std::string(1, *f);
            if (!atlas_.has(key)) atlas_.add(key, *view->image);
            a.frames.push_back(key);
            a.mirrored.push_back(view->mirrored);
        }
        return !a.frames.empty();
    };
    bool ok = true;
    // Monster classes by red-region size. Frame letters follow the Doom state tables.
    struct Def { const char* sprite; const char* walk; const char* attack; const char* pain; const char* death; float px; };
    const Def defs[kEnemyTiers] = {
        {"POSS", "ABCD", "EF", "G", "HIJKL", 0.031f},      // zombieman: hitscan
        {"TROO", "ABCD", "EFG", "H", "IJKL", 0.031f},      // imp: fireball
        {"SARG", "ABCD", "EFG", "H", "IJKLMN", 0.031f},    // demon: melee
        {"HEAD", "A", "BCD", "E", "FGHIJK", 0.034f},       // cacodemon: floats, fast fireball
        {"BOSS", "ABCD", "EFG", "H", "IJKLMNO", 0.033f},   // baron: green fireball, tanky
    };
    const char* sightSnd[kEnemyTiers] = {"DSPOSIT1", "DSBGSIT1", "DSSGTSIT", "DSCACSIT", "DSBRSSIT"};
    const char* painSnd[kEnemyTiers] = {"DSPOPAIN", "DSPOPAIN", "DSDMPAIN", "DSDMPAIN", "DSDMPAIN"};
    const char* deathSnd[kEnemyTiers] = {"DSPODTH1", "DSBGDTH1", "DSSGTDTH", "DSCACDTH", "DSBRSDTH"};
    const char* attackSnd[kEnemyTiers] = {"DSPISTOL", "DSFIRSHT", "DSSGTATK", "DSFIRSHT", "DSFIRSHT"};
    for (int t = 0; t < kEnemyTiers; ++t) {
        EnemyArt& e = enemies[t];
        e.name = kTierNames[t];
        e.metresPerPixel = defs[t].px;
        ok &= addSprite(e.walk, defs[t].sprite, defs[t].walk, 5.f);
        ok &= addSprite(e.attack, defs[t].sprite, defs[t].attack, 6.f);
        ok &= addSprite(e.pain, defs[t].sprite, defs[t].pain, 6.f);
        ok &= addSprite(e.death, defs[t].sprite, defs[t].death, 9.f);
        e.sightSound = std::string("sight") + std::to_string(t);
        e.painSound = std::string("pain") + std::to_string(t);
        e.deathSound = std::string("death") + std::to_string(t);
        e.attackSound = std::string("attack") + std::to_string(t);
    }
    struct WDef { const char* name; const char* gun; const char* idle; const char* fire; float fps; const char* flash; const char* flashFrames; const char* sound; };
    const WDef wdefs[kWeaponArt] = {
        {"SHOTGUN", "SHTG", "A", "BCDCB", 9.f, "SHTF", "AB", "shoot"},
        {"CHAINGUN", "CHGG", "A", "AB", 16.f, "CHGF", "AB", "fire_chain"},
        {"ROCKET LAUNCHER", "MISG", "A", "BA", 6.f, "MISF", "ABCD", "fire_rocket"},
        {"PLASMA RIFLE", "PLSG", "A", "B", 12.f, "PLSF", "AB", "fire_plasma"},
    };
    for (int w = 0; w < kWeaponArt; ++w) {
        WeaponArt& a = weapons[w];
        a.name = wdefs[w].name;
        a.fireSound = wdefs[w].sound;
        ok &= addSprite(a.idle, wdefs[w].gun, wdefs[w].idle, 1.f);
        ok &= addSprite(a.fire, wdefs[w].gun, wdefs[w].fire, wdefs[w].fps);
        ok &= addSprite(a.flash, wdefs[w].flash, wdefs[w].flashFrames, 12.f);
    }
    const char* pickupSprites[kPickupArt] = {"STIM", "MEDI", "CLIP", "ROCK", "CELL", "MGUN", "LAUN", "PLAS"};
    for (int k = 0; k < kPickupArt; ++k) ok &= addSprite(pickups[k], pickupSprites[k], "A", 1.f);
    ok &= addSprite(explosion, "MISL", "BCD", 12.f);
    const char* balls[kProjectileTypes] = {"BAL1", "BAL2", "BAL7", "MISL", "PLSS"};
    const char* flight[kProjectileTypes] = {"AB", "AB", "AB", "A", "AB"};
    for (int p = 0; p < kProjectileTypes; ++p) ok &= addSprite(projectile[p], balls[p], flight[p], 8.f);
    for (int p = 0; p < 3; ++p) ok &= addSprite(projectileHit[p], balls[p], "CDE", 14.f);
    ok &= addSprite(projectileHit[3], "MISL", "BCD", 12.f);
    ok &= addSprite(projectileHit[4], "PLSE", "ABCDE", 16.f);
    if (!ok) {
        std::fprintf(stderr, "[assets] WAD is missing expected sprites; falling back to procedural art\n");
        atlas_ = {};
        return false;
    }

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

    // Sounds: prefer the WAD, fall back to procedural for anything missing.
    auto addSound = [&](const std::string& name, std::initializer_list<const char*> lumps, proc::Sound fallback) {
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
    addSound("levelup", {"DSGETPOW"}, proc::sndLevelUp());
    addSound("fireball", {"DSFIRSHT"}, proc::sndFireball());
    addSound("fireball_hit", {"DSFIRXPL"}, proc::sndHit());
    addSound("gameover", {"DSPDIEHI", "DSPLDETH"}, proc::sndGameOver());
    addSound("menu", {"DSPSTOP"}, proc::sndMove());
    addSound("menu_select", {"DSPISTOL"}, proc::sndRotate());
    addSound("fire_chain", {"DSPISTOL"}, proc::sndHit());
    addSound("fire_rocket", {"DSRLAUNC"}, proc::sndShoot());
    addSound("fire_plasma", {"DSPLASMA"}, proc::sndFireball());
    addSound("rocket_hit", {"DSRXPLOD", "DSBAREXP"}, proc::sndExplode());
    addSound("pickup_item", {"DSITEMUP"}, proc::sndClear());
    addSound("pickup_weapon", {"DSWPNUP"}, proc::sndLevelUp());
    for (int t = 0; t < kEnemyTiers; ++t) {
        addSound(enemies[t].sightSound, {sightSnd[t]}, proc::sndRedLine());
        addSound(enemies[t].painSound, {painSnd[t]}, proc::sndHit());
        addSound(enemies[t].deathSound, {deathSnd[t]}, proc::sndEnemyDie());
        addSound(enemies[t].attackSound, {attackSnd[t]}, proc::sndFireball());
    }
    return true;
}
#endif

}  // namespace rl::game
