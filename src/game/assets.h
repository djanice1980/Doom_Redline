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
    SpriteAnim walk, attack, pain, death;
    SpriteAnim xdeath;        // gib death frames (zombies and imps have them); empty otherwise
    float metresPerPixel = 0.031f;
    glm::vec4 tint{1.f};
    std::string sightSound, painSound, deathSound, attackSound;   // logical sound names
};

constexpr int kEnemyTiers = 7;
constexpr int kProjectileTypes = 5;   // imp / cacodemon / baron fireballs, rocket, plasma
constexpr int kWeaponArt = 4;         // shotgun, chaingun, rocket launcher, plasma rifle
constexpr int kPickupArt = 8;         // stim, medikit, bullets, rockets, cells, chaingun, launcher, plasma gun

struct WeaponArt {
    std::string name;
    SpriteAnim idle, fire, flash;
    SpriteAnim cooldown;      // shown briefly after the trigger is released (plasma rifle's vent frame); may be empty
    float flashFor = 0.12f;   // seconds the muzzle flash is drawn after a shot (matches the frame it was drawn for)
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
    bool brutalPack = false;
    SpriteAnim brChunk, brChunkBig, brPool, brSplat, brSpray, brSmoke, brCasingBullet, brCasingShell, brBlast;
    int brGibSounds = 0, brShellSounds = 0, brCasingSounds = 0, brDripSounds = 0;   // "gibdeathN", "shellN", "casingN", "dripN" (1-based)
    static std::optional<std::filesystem::path> findBrutalPack(const std::string& baseDir);
    // Doom lump names actually used for wall/floor/ceiling (empty on placeholder art); keys the material maps.
    std::string wallLump, floorLump, ceilingLump;
    std::string crosshair = "crosshair";
    std::string white = "__white";
    EnemyArt enemies[kEnemyTiers];
    SpriteAnim projectile[kProjectileTypes], projectileHit[kProjectileTypes];
    WeaponArt weapons[kWeaponArt];
    SpriteAnim pickups[kPickupArt];
    SpriteAnim explosion;
    // Decor: animated torches (red/blue/green), candelabra, column lamp, barrel.
    SpriteAnim torchRed, torchBlue, torchGreen, candelabra, lamp, barrel;
    std::string title;                      // optional big title graphic ("" if none)

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
