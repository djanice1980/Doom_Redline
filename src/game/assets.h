#pragma once
// Builds the texture atlas and sound bank from a Doom IWAD when one is
// available, otherwise from procedural art. Game code refers to art by the
// logical names below and never touches WAD lump names directly.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "audio/audio.h"
#include "render/atlas.h"

namespace rl::game {

struct SpriteAnim {
    std::vector<std::string> frames;   // atlas keys
    std::vector<bool> mirrored;        // per frame
    float fps = 8.f;
    bool empty() const { return frames.empty(); }
};

// One monster class per red-region size tier (zombie, imp, demon, cacodemon, baron).
struct EnemyArt {
    std::string name;
    bool available = false;   // art loaded (the Doom 2 variants need doom2.wad)
    SpriteAnim walk, attack, pain, death;
    SpriteAnim xdeath;        // gib death frames (zombies and imps have them); empty otherwise
    SpriteAnim brDeath[6];    // community pack: per-weapon deaths, indexed by DeathKind (empty when absent)
    float metresPerPixel = 0.031f;
    glm::vec4 tint{1.f};
    std::string sightSound, painSound, deathSound, attackSound;   // logical sound names
};

constexpr int kEnemyTiers = 7;        // the difficulty ladder: zombie, imp, demon, cacodemon, baron, cyberdemon, spider
constexpr int kEnemyKinds = 13;       // + Doom 2 variants: chaingunner, hell knight, revenant, mancubus, arachnotron, arch-vile
constexpr int kProjectileTypes = 8;   // imp / cacodemon / baron fireballs, rocket, plasma, revenant missile, mancubus fireball, arachnotron plasma
constexpr int kWeaponArt = 4;         // shotgun, chaingun, rocket launcher, plasma rifle
constexpr int kPickupArt = 9;         // stim, medikit, bullets, rockets, cells, chaingun, launcher, plasma gun, the crypt key

struct WeaponArt {
    std::string name;
    SpriteAnim idle, fire, flash;
    SpriteAnim cooldown;      // shown briefly after the trigger is released (plasma rifle's vent frame); may be empty
    float flashFor = 0.12f;   // seconds the muzzle flash is drawn after a shot (matches the frame it was drawn for)
    SpriteAnim brIdle, brFire;   // community pack: Brutal Doom's weapon art (flash baked into the fire frames); empty when absent
    std::string fireSound;
    glm::vec4 tint{1.f};
};

class Assets {
public:
    // Searches, in order: explicit path, $REDLINE_WAD, the player's saved choice
    // (<pref>/wad.txt), the installer's redline.cfg next to the executable,
    // ./wads and <exe>/wads, then the Steam / GOG Doom folders of this OS.
    static std::optional<std::filesystem::path> findWad(const std::optional<std::filesystem::path>& explicitPath,
                                                        const std::string& prefDir = "", const std::string& baseDir = "");
    static std::string savedWadPath(const std::string& prefDir);     // <pref>/wad.txt, "" if none
    static std::string savedExtrasPath(const std::string& prefDir);  // <pref>/extras.txt, "" if none
    static std::string configWadPath(const std::string& baseDir);    // wad= line of <exe>/redline.cfg
    static std::string configExtrasPath(const std::string& baseDir); // extras= line of <exe>/redline.cfg

    // `extrasPath` (optional) names the rerelease extras.wad for the Ogg
    // soundtracks; otherwise $REDLINE_EXTRAS and the IWAD's folder are tried.
    bool load(const std::optional<std::filesystem::path>& wadPath, audio::Audio& audio, const std::string& extrasFile = "");

    const render::Atlas& atlas() const { return atlas_; }
    const render::AtlasRegion& region(const std::string& key) const { return atlas_.get(key); }
    bool usingWad() const { return usingWad_; }
    const std::string& wadName() const { return wadName_; }

    // Logical art -----------------------------------------------------------
    std::string block = "block";            // white bevelled tile (tinted per colour)
    std::string redBlock = "block_red";
    std::string wall = "wall";
    std::string floor = "floor";
    std::string ceiling = "ceiling";
    // Brutal-mode gore: blood drops (BLUD), the pool-of-blood decoration (POL5) and bullet puffs (PUFF).
    SpriteAnim blood, puff;
    std::string bloodPool;
    // The bundled community pack (assets/brutal, see CREDITS.txt there): richer gore. Empty anims when absent.
    std::optional<std::filesystem::path> brutalPackDir;   // set before load(); found with findBrutalPack()
    std::optional<std::filesystem::path> fontPackDir;     // AI-upscaled menu font (assets/font-hd); found with findFontPack()
    int fontHdGlyphs = 0;                                  // how many glyphs came from that pack (0 = xBR only)
    bool brutalPack = false;
    SpriteAnim brChunk[3], brChunkBig[3], brPool[3], brSplat[3], brSpray[3];   // indexed by blood colour: red, green, blue
    SpriteAnim brSmoke, brCasingBullet, brCasingShell, brBlast;
    SpriteAnim brSparks, brPlasmaHit;                 // sparks off metal and blocks; plasma bolt burst
    SpriteAnim brFlare[5];                            // soft light discs: red, yellow, blue, green, white
    SpriteAnim brMuzzleFlare;
    int brGibSounds = 0, brShellSounds = 0, brCasingSounds = 0, brDripSounds = 0;   // "gibdeathN", "shellN", "casingN", "dripN" (1-based)
    int brSparkSounds = 0, brRicochetSounds = 0, brDirtSounds = 0;                   // "sparksN", "ricochetN", "bhitN"
    bool brWeaponSounds = false, brPlayerPain = false;   // "br_shoot" etc. and "br_pain"
    int brExplodeSounds = 0, brBoneSounds = 0, brImpSounds = 0, brZombieSight = 0;   // "br_explodeN", "bonecrN", "impclawN", "zcsitN"
    static std::optional<std::filesystem::path> findBrutalPack(const std::string& baseDir);
    static std::optional<std::filesystem::path> findFontPack(const std::string& baseDir);
    // Doom lump names actually used for wall/floor/ceiling (empty on placeholder art); keys the material maps.
    std::string wallLump, floorLump, ceilingLump;
    // The crypt dresses each room from one of these sets, so a dungeon is not one
    // texture end to end: 0-3 are room themes, 4 is the boss hall, 5 the corridors.
    static constexpr int kCryptThemes = 6;
    std::string cryptWall[kCryptThemes], cryptFloor[kCryptThemes], cryptCeil[kCryptThemes];
    std::string cryptWallLump[kCryptThemes], cryptFloorLump[kCryptThemes], cryptCeilLump[kCryptThemes];
    // Crypt scenery, all optional: whatever the WAD has is what gets placed.
    SpriteAnim column[3], candle, hanging[3], impaled[2], stalagmite, skullPile;
    std::string crosshair = "crosshair";
    std::string white = "__white";
    std::string ring = "__ring";   // soft ring for the line-clear shockwave (procedural, always present)
    EnemyArt enemies[kEnemyKinds];
    SpriteAnim vileFire;      // the arch-vile's flame (FIRE A-H, Doom 2)
    SpriteAnim projectile[kProjectileTypes], projectileHit[kProjectileTypes];
    WeaponArt weapons[kWeaponArt];
    SpriteAnim pickups[kPickupArt];
    SpriteAnim explosion;
    // Decor: animated torches (red/blue/green), candelabra, column lamp, barrel.
    SpriteAnim torchRed, torchBlue, torchGreen, candelabra, lamp, barrel;
    std::string title;                      // optional big title graphic ("" if none)
    std::string skull;                      // M_SKULL1, the menu skull, as the trophy icon ("" if none)

    // Music: raw MUS lumps by name (D_E1M1 ...) and the GENMIDI OPL bank.
    std::map<std::string, std::vector<uint8_t>> music;
    std::vector<uint8_t> genmidi;
    // Streamed Ogg Vorbis music from the rerelease's extras.wad (H_* modern
    // soundtrack by Andrew Hulshult, O_* original score recorded on an SC-55):
    // only the directory is read; the data streams at playback time.
    struct OggLump { std::string name; std::string path; uint64_t offset; uint64_t size; };
    std::vector<OggLump> oggMusic;
    std::string extrasPath;

    // Font: char -> atlas key. Doom's STCFN font is uppercase only.
    std::map<char, std::string> font;
    std::map<char, std::string> fontBig;   // the same glyphs upscaled fontBigScale times with smoothed edges, for large text
    int fontBigScale = 1;
    int fontHeight = 8;
    int textWidth(const std::string& s, float scale) const;

private:
    void loadProcedural(audio::Audio& audio);
    bool loadFromWad(const std::filesystem::path& path, audio::Audio& audio);

    render::Atlas atlas_;
    void loadBrutalPack(audio::Audio& audio);
    std::vector<uint8_t> palette_;   // PLAYPAL (768 bytes) when a WAD is loaded: decodes Doom-format sprites in the gore pack
    bool usingWad_ = false;
    std::string wadName_;
    std::string extrasHint_;
};

}  // namespace rl::game
