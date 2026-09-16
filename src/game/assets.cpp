#include "game/assets.h"

#include "core/png_read.h"
#include "audio/oggstream.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/procedural.h"
#ifdef REDLINE_HAVE_WAD
#include "wad/wad.h"
#endif

namespace rl::game {

namespace fs = std::filesystem;

namespace {
const char* kTierNames[kEnemyTiers] = {"ZOMBIE", "IMP", "DEMON", "CACODEMON", "BARON", "CYBERDEMON", "SPIDER MASTERMIND"};
}

namespace {
std::string readFirstLine(const fs::path& p) {
    std::string line;
    if (std::FILE* f = std::fopen(p.string().c_str(), "r")) {
        char buf[1024] = {};
        if (std::fgets(buf, sizeof buf, f)) line = buf;
        std::fclose(f);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}
}  // namespace

std::string Assets::savedWadPath(const std::string& prefDir) {
    if (prefDir.empty()) return "";
    return readFirstLine(fs::path(prefDir) / "wad.txt");
}

std::string Assets::savedExtrasPath(const std::string& prefDir) {
    if (prefDir.empty()) return "";
    return readFirstLine(fs::path(prefDir) / "extras.txt");
}

// redline.cfg is a tiny key=value file an installer can drop next to the
// executable: `wad=C:\Games\DOOM\doom.wad`, optionally `extras=...`.
namespace {
std::string configValue(const std::string& baseDir, const std::string& key) {
    if (baseDir.empty()) return "";
    std::string out;
    if (std::FILE* f = std::fopen((fs::path(baseDir) / "redline.cfg").string().c_str(), "r")) {
        char buf[1024];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.rfind(key + "=", 0) == 0) { out = line.substr(key.size() + 1); break; }
        }
        std::fclose(f);
    }
    return out;
}
}  // namespace

std::string Assets::configWadPath(const std::string& baseDir) { return configValue(baseDir, "wad"); }
std::string Assets::configExtrasPath(const std::string& baseDir) { return configValue(baseDir, "extras"); }

std::optional<fs::path> Assets::findWad(const std::optional<fs::path>& explicitPath, const std::string& prefDir, const std::string& baseDir) {
    std::vector<fs::path> candidates;
    if (explicitPath) candidates.push_back(*explicitPath);
    if (const char* e = std::getenv("REDLINE_WAD")) candidates.emplace_back(e);
    if (std::string s = savedWadPath(prefDir); !s.empty()) candidates.emplace_back(s);
    if (std::string s = configWadPath(baseDir); !s.empty()) candidates.emplace_back(s);
    std::error_code ec;
    for (const fs::path& dir : {fs::path("wads"), fs::path(baseDir.empty() ? "." : baseDir) / "wads"})
        if (fs::is_directory(dir, ec))
            for (auto& entry : fs::directory_iterator(dir, ec)) candidates.push_back(entry.path());
    const char* names[] = {"doom.wad", "DOOM.WAD", "doom2.wad", "DOOM2.WAD", "freedoom1.wad", "freedoom2.wad"};
#ifdef _WIN32
    {
        std::vector<std::string> roots;
        for (const char* env : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"})
            if (const char* v = std::getenv(env)) roots.emplace_back(v);
        roots.emplace_back("C:\\Program Files (x86)");
        roots.emplace_back("C:\\Program Files");
        const char* dirs[] = {"\\Steam\\steamapps\\common\\Ultimate Doom\\rerelease\\", "\\Steam\\steamapps\\common\\Ultimate Doom\\base\\",
                              "\\Steam\\steamapps\\common\\Doom 2\\rerelease\\", "\\Steam\\steamapps\\common\\Doom 2\\base\\",
                              "\\GOG Galaxy\\Games\\DOOM\\", "\\GOG Galaxy\\Games\\DOOM II\\", "\\GOG Galaxy\\Games\\DOOM + DOOM II\\"};
        for (const std::string& r : roots)
            for (const char* d : dirs)
                for (const char* n : names) candidates.emplace_back(r + d + n);
        for (const char* n : names) candidates.emplace_back(std::string("C:\\DOOM\\") + n);
    }
    const char* home = std::getenv("USERPROFILE");
    if (home) {
        std::string h = home;
        const char* dirs[] = {"\\Saved Games\\id Software\\DOOM\\", "\\Documents\\DOOM\\"};
        for (const char* d : dirs)
            for (const char* n : names) candidates.emplace_back(h + d + n);
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        std::string h = home;
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
#endif
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

bool Assets::load(const std::optional<fs::path>& wadPath, audio::Audio& audio, const std::string& extrasFile) {
    bool ok = false;
    extrasHint_ = extrasFile;
#ifdef REDLINE_HAVE_WAD
    if (wadPath) ok = loadFromWad(*wadPath, audio);
#endif
    if (!ok) loadProcedural(audio);
    loadBrutalPack(audio);
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
    const glm::vec4 tints[kEnemyTiers] = {{0.7f, 0.7f, 0.8f, 1.f}, {1.f, 1.f, 1.f, 1.f}, {1.f, 0.6f, 0.6f, 1.f}, {1.f, 0.4f, 0.4f, 1.f}, {0.5f, 1.f, 0.5f, 1.f}, {0.6f, 0.6f, 0.6f, 1.f}, {0.9f, 0.9f, 0.5f, 1.f}};
    const float sizes[kEnemyTiers] = {0.026f, 0.031f, 0.034f, 0.045f, 0.055f, 0.07f, 0.075f};
    for (int t = 0; t < kEnemyTiers; ++t) {
        EnemyArt& e = enemies[t];
        e.name = kTierNames[t];
        e.tint = tints[t];
        e.metresPerPixel = sizes[t];
        anim(e.walk, "enemy_idle", 2, 3.f, [](int i) { return proc::enemyFrame(i == 0 ? 0 : 2, 64); });
        anim(e.attack, "enemy_attack", 1, 4.f, [](int) { return proc::enemyFrame(1, 64); });
        anim(e.pain, "enemy_pain", 1, 4.f, [](int) { return proc::enemyFrame(2, 64); });
        anim(e.death, "enemy_death", 4, 8.f, [](int i) { return proc::enemyFrame(3 + i, 64); });
        e.xdeath = SpriteAnim{};
        e.sightSound = "enemy_sight";
        e.painSound = "enemy_pain";
        e.deathSound = "enemy_die";
        e.attackSound = t == 0 ? "shoot" : "fireball";
    }
    // Gore placeholders: soft blobs (red drops and pool, grey puff).
    auto blob = [](int size, uint8_t r, uint8_t g, uint8_t b, float edge) {
        Image im(size, size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                float dx = (x + 0.5f) / size - 0.5f, dy = (y + 0.5f) / size - 0.5f;
                float d = std::sqrt(dx * dx + dy * dy) * 2.f;
                float wob = 0.85f + 0.15f * std::sin(std::atan2(dy, dx) * 5.f);
                float a = std::clamp((wob - d) / edge, 0.f, 1.f);
                uint8_t* p = &im.rgba[(static_cast<size_t>(y) * size + x) * 4];
                p[0] = r; p[1] = g; p[2] = b; p[3] = static_cast<uint8_t>(a * 255.f);
            }
        return im;
    };
    anim(blood, "blood", 3, 10.f, [&](int i) { return blob(8 + 2 * i, 150, 10, 10, 0.5f); });
    anim(puff, "puff", 4, 12.f, [&](int i) { return blob(10 + 2 * i, 120, 120, 120, 0.7f); });
    if (!atlas_.has("blood_pool")) atlas_.add("blood_pool", blob(32, 120, 8, 8, 0.35f));
    bloodPool = "blood_pool";
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
    anim(torchRed, "fireball", 1, 1.f, [](int) { return proc::fireball(16); });
    torchBlue = torchGreen = torchRed;
    anim(candelabra, "fireball", 1, 1.f, [](int) { return proc::fireball(16); });
    lamp = barrel = candelabra;
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
    add("bfg", proc::sndExplode());
    add("gib", proc::sndHit());
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
    palette_.assign(&pal->rgb[0][0], &pal->rgb[0][0] + 768);
    std::fprintf(stderr, "[assets] %s: %s\n", path.filename().string().c_str(), wad::describe(*wad).c_str());
    usingWad_ = true;
    wadName_ = path.filename().string();

    // Block-mode tiles are ours; the arena uses Doom textures/flats.
    atlas_.add("block", proc::blockTexture(32));
    atlas_.add("block_red", proc::redBlockTexture(32));
    atlas_.add("crosshair", proc::crosshair(15));

    auto textures = wad::TextureSet::load(*wad, *pal);
    auto addTexture = [&](const std::string& key, std::initializer_list<const char*> names, Image fallback, std::string& lump) {
        lump.clear();
        if (textures)
            for (const char* n : names)
                if (auto img = textures->get(n)) { atlas_.add(key, *img); lump = n; return; }
        atlas_.add(key, fallback);
    };
    auto addFlat = [&](const std::string& key, std::initializer_list<const char*> names, Image fallback, std::string& lump) {
        lump.clear();
        for (const char* n : names) {
            auto bytes = wad->data(n);
            if (bytes.empty()) continue;
            if (auto img = wad::decodeFlat(bytes, *pal)) { atlas_.add(key, *img); lump = n; return; }
        }
        atlas_.add(key, fallback);
    };
    addTexture("wall", {"STARTAN3", "STARG3", "BROWN1", "STONE2"}, proc::wallTexture(64), wallLump);
    addFlat("floor", {"FLOOR4_8", "FLAT5_4", "FLOOR0_1"}, proc::floorTexture(64), floorLump);
    addFlat("ceiling", {"CEIL3_5", "FLAT20", "CEIL5_1"}, proc::wallTexture(64), ceilingLump);

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
    struct Def { const char* sprite; const char* walk; const char* attack; const char* pain; const char* death; float px; const char* xdeath; };
    const Def defs[kEnemyTiers] = {
        {"POSS", "ABCD", "EF", "G", "HIJKL", 0.031f, "NOPQRSTU"},      // zombieman: hitscan
        {"TROO", "ABCD", "EFG", "H", "IJKL", 0.031f, "NOPQRSTU"},      // imp: fireball
        {"SARG", "ABCD", "EFG", "H", "IJKLMN", 0.031f, ""},    // demon: melee
        {"HEAD", "A", "BCD", "E", "FGHIJK", 0.034f, ""},       // cacodemon: floats, fast fireball
        {"BOSS", "ABCD", "EFG", "H", "IJKLMNO", 0.033f, ""},   // baron: green fireball, tanky
        {"CYBR", "ABCD", "EF", "G", "HIJKLMNOP", 0.030f, ""},  // cyberdemon: rockets
        {"SPID", "ABCDEF", "GH", "I", "JKLMNOPQRS", 0.030f, ""}, // spider mastermind: chaingun
    };
    const char* sightSnd[kEnemyTiers] = {"DSPOSIT1", "DSBGSIT1", "DSSGTSIT", "DSCACSIT", "DSBRSSIT", "DSCYBSIT", "DSSPISIT"};
    const char* painSnd[kEnemyTiers] = {"DSPOPAIN", "DSPOPAIN", "DSDMPAIN", "DSDMPAIN", "DSDMPAIN", "DSDMPAIN", "DSDMPAIN"};
    const char* deathSnd[kEnemyTiers] = {"DSPODTH1", "DSBGDTH1", "DSSGTDTH", "DSCACDTH", "DSBRSDTH", "DSCYBDTH", "DSSPIDTH"};
    const char* attackSnd[kEnemyTiers] = {"DSPISTOL", "DSFIRSHT", "DSSGTATK", "DSFIRSHT", "DSFIRSHT", "DSRLAUNC", "DSPISTOL"};
    for (int t = 0; t < kEnemyTiers; ++t) {
        EnemyArt& e = enemies[t];
        e.name = kTierNames[t];
        e.metresPerPixel = defs[t].px;
        ok &= addSprite(e.walk, defs[t].sprite, defs[t].walk, 5.f);
        ok &= addSprite(e.attack, defs[t].sprite, defs[t].attack, 6.f);
        ok &= addSprite(e.pain, defs[t].sprite, defs[t].pain, 6.f);
        ok &= addSprite(e.death, defs[t].sprite, defs[t].death, 9.f);
        e.xdeath = SpriteAnim{};
        if (*defs[t].xdeath) addSprite(e.xdeath, defs[t].sprite, defs[t].xdeath, 10.f);
        e.sightSound = std::string("sight") + std::to_string(t);
        e.painSound = std::string("pain") + std::to_string(t);
        e.deathSound = std::string("death") + std::to_string(t);
        e.attackSound = std::string("attack") + std::to_string(t);
    }
    // Doom's plasma rifle fires on frame A with the PLSF flash (which includes the glowing
    // barrel) drawn over it; PLSGB0 is the vent/cool-down frame shown once the trigger is
    // released, with a very different origin, so it must never be the firing frame.
    // The flash sprites are cut to sit on the frame Doom fires from (SHTG/PLSG A, MISG B,
    // CHGG A/B), so each weapon fires on that frame and draws the flash only while it shows;
    // the shotgun's pump frames B-D and the plasma vent frame B have other origins.
    struct WDef { const char* name; const char* gun; const char* idle; const char* fire; float fps; const char* flash; const char* flashFrames; const char* sound; const char* cool; float flashFor; };
    const WDef wdefs[kWeaponArt] = {
        {"SHOTGUN", "SHTG", "A", "ABCDCB", 9.f, "SHTF", "AB", "shoot", "", 0.11f},
        {"CHAINGUN", "CHGG", "A", "AB", 16.f, "CHGF", "AB", "fire_chain", "", 0.12f},
        {"ROCKET LAUNCHER", "MISG", "A", "BA", 6.f, "MISF", "ABCD", "fire_rocket", "", 0.16f},
        {"PLASMA RIFLE", "PLSG", "A", "A", 12.f, "PLSF", "AB", "fire_plasma", "B", 0.17f},
    };
    for (int w = 0; w < kWeaponArt; ++w) {
        WeaponArt& a = weapons[w];
        a.name = wdefs[w].name;
        a.fireSound = wdefs[w].sound;
        ok &= addSprite(a.idle, wdefs[w].gun, wdefs[w].idle, 1.f);
        ok &= addSprite(a.fire, wdefs[w].gun, wdefs[w].fire, wdefs[w].fps);
        ok &= addSprite(a.flash, wdefs[w].flash, wdefs[w].flashFrames, 12.f);
        a.cooldown = SpriteAnim{};
        a.flashFor = wdefs[w].flashFor;
        if (*wdefs[w].cool) addSprite(a.cooldown, wdefs[w].gun, wdefs[w].cool, 1.f);
    }
    const char* pickupSprites[kPickupArt] = {"STIM", "MEDI", "CLIP", "ROCK", "CELL", "MGUN", "LAUN", "PLAS"};
    for (int k = 0; k < kPickupArt; ++k) ok &= addSprite(pickups[k], pickupSprites[k], "A", 1.f);
    ok &= addSprite(explosion, "MISL", "BCD", 12.f);
    // Gore (brutal mode): blood drops, the pool of blood and flesh, bullet puffs. Missing lumps just lose the effect.
    addSprite(blood, "BLUD", "ABC", 10.f);
    addSprite(puff, "PUFF", "ABCD", 12.f);
    {
        SpriteAnim pool;
        if (addSprite(pool, "POL5", "A", 1.f) && !pool.empty()) bloodPool = pool.frames[0];
    }
    // Decor (optional: a missing lump only loses that prop).
    addSprite(torchRed, "TRED", "ABCD", 8.f);
    addSprite(torchBlue, "TBLU", "ABCD", 8.f);
    addSprite(torchGreen, "TGRN", "ABCD", 8.f);
    addSprite(candelabra, "CBRA", "A", 1.f);
    addSprite(lamp, "COLU", "A", 1.f);
    addSprite(barrel, "BAR1", "AB", 4.f);
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

    // Music and the OPL instrument bank.
    for (const wad::Lump& l : wad->lumps()) {
        if (l.name.size() > 2 && l.name[0] == 'D' && l.name[1] == '_' && l.size > 16) {
            auto bytes = wad->data(l);
            if (bytes.size() >= 4 && bytes[0] == 'M' && bytes[1] == 'U' && bytes[2] == 'S') music[l.name] = std::vector<uint8_t>(bytes.begin(), bytes.end());
        }
    }
    {
        auto bytes = wad->data("GENMIDI");
        genmidi.assign(bytes.begin(), bytes.end());
    }
    std::fprintf(stderr, "[assets] %zu music tracks, GENMIDI %s\n", music.size(), genmidi.empty() ? "missing" : "loaded");

    // extras.wad (chosen path, $REDLINE_EXTRAS, or the IWAD's folder): index the Ogg music without loading 600 MB.
    {
        std::vector<fs::path> candidates;
        if (!extrasHint_.empty()) candidates.emplace_back(extrasHint_);
        if (const char* e = std::getenv("REDLINE_EXTRAS")) candidates.emplace_back(e);
        candidates.push_back(path.parent_path() / "extras.wad");
        candidates.push_back(path.parent_path() / "EXTRAS.WAD");
        for (const fs::path& ex : candidates) {
            std::error_code ec;
            if (!fs::is_regular_file(ex, ec)) continue;
            std::FILE* f = std::fopen(ex.string().c_str(), "rb");
            if (!f) continue;
            uint8_t hdr[12];
            if (std::fread(hdr, 1, 12, f) == 12) {
                uint32_t n = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
                uint32_t dir = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (static_cast<uint32_t>(hdr[11]) << 24);
                std::vector<uint8_t> entries(static_cast<size_t>(n) * 16);
                if (n < 100000 && std::fseek(f, static_cast<long>(dir), SEEK_SET) == 0 && std::fread(entries.data(), 1, entries.size(), f) == entries.size()) {
                    for (uint32_t i = 0; i < n; ++i) {
                        const uint8_t* e = entries.data() + static_cast<size_t>(i) * 16;
                        uint32_t off = e[0] | (e[1] << 8) | (e[2] << 16) | (static_cast<uint32_t>(e[3]) << 24);
                        uint32_t size = e[4] | (e[5] << 8) | (e[6] << 16) | (static_cast<uint32_t>(e[7]) << 24);
                        std::string name(reinterpret_cast<const char*>(e + 8), 8);
                        name = name.c_str();
                        if (name.size() < 3 || name[1] != '_' || (name[0] != 'H' && name[0] != 'O') || size < 64) continue;
                        uint8_t head[4];
                        if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0 || std::fread(head, 1, 4, f) != 4) continue;
                        if (std::memcmp(head, "OggS", 4) != 0) continue;
                        oggMusic.push_back({name, ex.string(), off, size});
                    }
                }
            }
            std::fclose(f);
            if (!oggMusic.empty()) { extrasPath = ex.string(); break; }
        }
        if (!oggMusic.empty()) std::fprintf(stderr, "[assets] %s: %zu streamed music tracks (modern + SC-55)\n", fs::path(extrasPath).filename().string().c_str(), oggMusic.size());
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
    addSound("bfg", {"DSBFG", "DSRXPLOD"}, proc::sndExplode());
    addSound("gib", {"DSSLOP"}, proc::sndHit());
    for (int t = 0; t < kEnemyTiers; ++t) {
        addSound(enemies[t].sightSound, {sightSnd[t]}, proc::sndRedLine());
        addSound(enemies[t].painSound, {painSnd[t]}, proc::sndHit());
        addSound(enemies[t].deathSound, {deathSnd[t]}, proc::sndEnemyDie());
        addSound(enemies[t].attackSound, {attackSnd[t]}, proc::sndFireball());
    }
    return true;
}
#endif


// ---------------------------------------------------------------------------
// Community gore pack (assets/brutal): PNG sprites with grAb offsets, WAV/OGG/DMX
// sounds and KVX gibs, curated from the Brutal Doom Community Expansion and the
// Brutal Voxel Cyber Horror Monster Mix (credits and licences in CREDITS.txt there).
std::optional<fs::path> Assets::findBrutalPack(const std::string& baseDir) {
    std::vector<fs::path> candidates;
    if (const char* e = std::getenv("REDLINE_BRUTAL_PACK")) candidates.emplace_back(e);
    if (!baseDir.empty()) {
        fs::path base(baseDir);
        candidates.push_back(base / "brutal");                                   // Windows zip / installer
        candidates.push_back(base / ".." / "share" / "redline" / "brutal");  // Linux: bin/../share
        candidates.push_back(base / "assets" / "brutal");
        candidates.push_back(base / ".." / "assets" / "brutal");             // build/ next to the checkout
    }
    candidates.emplace_back("assets/brutal");
    candidates.emplace_back("/usr/share/redline/brutal");
    candidates.emplace_back("/usr/local/share/redline/brutal");
    std::error_code ec;
    for (const fs::path& c : candidates) if (fs::is_directory(c / "sprites", ec)) return c;
    return std::nullopt;
}

namespace {
std::vector<uint8_t> readAll(const fs::path& p) {
    std::vector<uint8_t> bytes;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// PCM WAV (8/16-bit, mono or stereo) to mono floats.
bool decodeWav(const std::vector<uint8_t>& d, int& rate, std::vector<float>& out) {
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return false;
    size_t pos = 12;
    int channels = 0, bits = 0;
    rate = 0;
    while (pos + 8 <= d.size()) {
        const uint32_t len = le32(&d[pos + 4]);
        const uint8_t* body = &d[pos + 8];
        if (pos + 8 + len > d.size()) return false;
        if (std::memcmp(&d[pos], "fmt ", 4) == 0 && len >= 16) {
            if (le16(body) != 1) return false;   // PCM only
            channels = le16(body + 2);
            rate = static_cast<int>(le32(body + 4));
            bits = le16(body + 14);
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            if (channels <= 0 || rate <= 0 || (bits != 8 && bits != 16)) return false;
            const size_t frameBytes = static_cast<size_t>(channels) * static_cast<size_t>(bits / 8);
            const size_t frames = len / frameBytes;
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0.f;
                for (int c = 0; c < channels; ++c) {
                    const uint8_t* s = body + i * frameBytes + static_cast<size_t>(c) * static_cast<size_t>(bits / 8);
                    sum += bits == 8 ? (static_cast<float>(s[0]) - 128.f) / 128.f : static_cast<float>(static_cast<int16_t>(le16(s))) / 32768.f;
                }
                out[i] = sum / static_cast<float>(channels);
            }
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

// DMX ("DS*" lump written to a file): format 3, rate, count, unsigned 8-bit samples with 16-byte padding.
bool decodeDmx(const std::vector<uint8_t>& d, int& rate, std::vector<float>& out) {
    if (d.size() < 8 || le16(&d[0]) != 3) return false;
    rate = le16(&d[2]);
    size_t count = std::min<size_t>(le32(&d[4]), d.size() - 8);
    const uint8_t* pcm = &d[8];
    if (count > 32) { pcm += 16; count -= 32; }
    out.resize(count);
    for (size_t i = 0; i < count; ++i) out[i] = (static_cast<float>(pcm[i]) - 128.f) / 128.f;
    return rate > 0;
}

bool decodeOgg(const fs::path& p, int& rate, std::vector<float>& out) {
#ifdef REDLINE_HAVE_VORBIS
    rate = audio::Audio::kRate;
    auto s = audio::OggStream::open(p.string(), 0, 0, static_cast<uint32_t>(rate));
    if (!s) return false;
    std::vector<float> buf;
    for (;;) {
        buf.assign(4096 * 2, 0.f);
        int n = s->read(buf.data(), 4096, 1.f);
        for (int i = 0; i < n; ++i) out.push_back(0.5f * (buf[static_cast<size_t>(i) * 2] + buf[static_cast<size_t>(i) * 2 + 1]));
        if (n < 4096) break;
    }
    return !out.empty();
#else
    (void)p; (void)rate; (void)out;
    return false;
#endif
}
}  // namespace

void Assets::loadBrutalPack(audio::Audio& audio) {
    brutalPack = false;
    for (int c = 0; c < 3; ++c) for (SpriteAnim* a : {&brChunk[c], &brChunkBig[c], &brPool[c], &brSplat[c], &brSpray[c]}) *a = SpriteAnim{};
    for (SpriteAnim* a : {&brSmoke, &brCasingBullet, &brCasingShell, &brBlast, &brSparks, &brPlasmaHit, &brMuzzleFlare}) *a = SpriteAnim{};
    for (SpriteAnim& a : brFlare) a = SpriteAnim{};
    brGibSounds = brShellSounds = brCasingSounds = brDripSounds = brSparkSounds = brRicochetSounds = brDirtSounds = 0;
    if (!brutalPackDir) return;
    const fs::path dir = *brutalPackDir;
    int sprites = 0, sounds = 0;
    // Halves an image (box filter), offsets included: the 256px fireballs need no more.
    auto halve = [](const Image& in) {
        Image out(std::max(1, in.width / 2), std::max(1, in.height / 2));
        out.offsetX = in.offsetX / 2;
        out.offsetY = in.offsetY / 2;
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(in.width - 1, x * 2 + dx), sy = std::min(in.height - 1, y * 2 + dy);
                            sum += in.rgba[(static_cast<size_t>(sy) * in.width + sx) * 4 + c];
                        }
                    out.rgba[(static_cast<size_t>(y) * out.width + x) * 4 + c] = static_cast<uint8_t>(sum / 4);
                }
        return out;
    };
    auto pngAnim = [&](SpriteAnim& a, const char* prefix, const char* frames, float fps, int shrink = 0) {
        a = SpriteAnim{};
        a.fps = fps;
        for (const char* f = frames; *f; ++f) {
            const std::string name = std::string(prefix) + *f + "0";
            const std::string key = std::string(prefix) + "_" + *f;
            if (!atlas_.has(key)) {
                std::string err;
                std::vector<uint8_t> bytes = readAll(dir / "sprites" / (name + ".png"));
                auto img = decodePng(bytes, &err);
#ifdef REDLINE_HAVE_WAD
                if (!img && palette_.size() == 768 && bytes.size() > 8) {   // a few pack files are Doom patches with a .png name
                    wad::Palette pal;
                    std::memcpy(&pal.rgb[0][0], palette_.data(), 768);
                    img = wad::decodePatch(bytes, pal);
                }
#endif
                if (!img) { if (!err.empty()) std::fprintf(stderr, "[brutal] %s.png: %s\n", name.c_str(), err.c_str()); continue; }
                for (int i = 0; i < shrink; ++i) *img = halve(*img);
                atlas_.add(key, *img);
                ++sprites;
            }
            a.frames.push_back(key);
            a.mirrored.push_back(false);
        }
    };
    // Red, green (barons) and blue (cacodemons) blood: the same five effects per colour.
    const char* chunkSets[3] = {"XDB1", "XDB5", "XDB3"};
    const char* chunkBigSets[3] = {"XME1", "XME5", "XME3"};
    const char* poolSets[3] = {"BLOR", "BLOG", "BLOB"};
    const char* splatSets[3] = {"BSP1", "BSP5", "BSP3"};
    const char* spraySets[3] = {"BLHT", "BLHG", "BLHB"};
    for (int c = 0; c < 3; ++c) {
        pngAnim(brChunk[c], chunkSets[c], "ABCDEFGHJKOP", 1.f);       // small meat chunks (one frame per chunk)
        pngAnim(brChunkBig[c], chunkBigSets[c], "ABCD", 1.f);         // bigger gibs
        pngAnim(brPool[c], poolSets[c], "ABCDEFGHIJK", 14.f);         // pool spreading on the floor
        pngAnim(brSplat[c], splatSets[c], "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 12.f);   // wall splat: hits, spreads, dries dark
        pngAnim(brSpray[c], spraySets[c], "ABCDEFGHIJ", 24.f);        // blood cloud at the hit point
    }
    pngAnim(brBlast, "EXP4", "ABCDEFGHIJKLMNOPQRSTUVWXY", 40.f, 1);   // fireball at half size (128px)
    pngAnim(brSparks, "SPKN", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 40.f, 1); // sparks at half size
    pngAnim(brPlasmaHit, "PLX6", "ABCDEFGHIJKLMNOPQRSTUVWXY", 50.f);  // blue plasma burst (128px)
    const char* flareSets[5] = {"LENR", "LENY", "LENB", "LENG", "LENW"};
    for (int c = 0; c < 5; ++c) pngAnim(brFlare[c], flareSets[c], "A", 1.f, 2);   // 256 -> 64px soft discs
    pngAnim(brMuzzleFlare, "FLAR", "B", 1.f);
    pngAnim(brSmoke, "PUF2", "ABCDEFGHIJKL", 18.f);      // smoke puff
    pngAnim(brCasingBullet, "C4S1", "ABCDEFGHIJKLM", 20.f);   // A-H tumbling, I-M lying
    pngAnim(brCasingShell, "C4S2", "ABCDEFGHIJKLMN", 20.f);
    auto sound = [&](const std::string& name, const std::string& file) {
        const fs::path p = dir / "sounds" / file;
        std::vector<uint8_t> bytes = readAll(p);
        if (bytes.empty()) return false;
        int rate = 0;
        std::vector<float> pcm;
        bool ok = false;
        if (bytes.size() > 4 && std::memcmp(bytes.data(), "RIFF", 4) == 0) ok = decodeWav(bytes, rate, pcm);
        else if (bytes.size() > 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) ok = decodeOgg(p, rate, pcm);
        else ok = decodeDmx(bytes, rate, pcm);
        if (!ok) { std::fprintf(stderr, "[brutal] %s: unsupported sound\n", file.c_str()); return false; }
        audio.addSound(name, rate, std::move(pcm));
        ++sounds;
        return true;
    };
    for (int i = 1; i <= 6; ++i) if (sound("gibdeath" + std::to_string(i), std::string("DSXDTH1") + static_cast<char>('A' + i - 1) + ".lmp")) brGibSounds = i;
    for (int i = 1; i <= 3; ++i) if (sound("shell" + std::to_string(i), "DSSHELL" + std::to_string(i) + ".wav")) brShellSounds = i;
    for (int i = 1; i <= 3; ++i) if (sound("casing" + std::to_string(i), "DSCASIN" + std::to_string(i) + ".ogg")) brCasingSounds = i;
    for (int i = 1; i <= 3; ++i) if (sound("drip" + std::to_string(i), "LQDRIP" + std::to_string(i) + ".ogg")) brDripSounds = i;
    for (int i = 1; i <= 4; ++i) if (sound("sparks" + std::to_string(i), "SPARKS" + std::to_string(i) + ".ogg")) brSparkSounds = i;
    {
        const char* rico[3] = {"RICOCHE2.wav", "RICOCHE3.wav", "RICOCHE5.wav"};
        for (int i = 0; i < 3; ++i) if (sound("ricochet" + std::to_string(i + 1), rico[i])) brRicochetSounds = i + 1;
    }
    for (int i = 1; i <= 3; ++i) if (sound("bhit" + std::to_string(i), "BHITDIR" + std::to_string(i) + ".ogg")) brDirtSounds = i;
    brutalPack = sprites > 0;
    std::fprintf(stderr, "[brutal] %s: %d sprites, %d sounds\n", dir.string().c_str(), sprites, sounds);
}

}  // namespace rl::game
