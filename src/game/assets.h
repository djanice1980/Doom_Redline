#pragma once
// Builds the texture atlas and sound bank from a Doom IWAD when one is
// available, otherwise from procedural art. Game code refers to art by the
// logical names below and never touches WAD lump names directly.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "render/atlas.h"

namespace rl::game {

struct SpriteAnim {
    std::vector<std::string> frames;   // atlas keys
    std::vector<bool> mirrored;        // per frame
    float fps = 8.f;
    bool empty() const { return frames.empty(); }
};

class Assets {
public:
    // Searches: explicit path, $REDLINE_WAD, ./wads/*.wad, the Steam Doom folders.
    static std::optional<std::filesystem::path> findWad(const std::optional<std::filesystem::path>& explicitPath);

    bool load(const std::optional<std::filesystem::path>& wadPath, audio::Audio& audio);

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
    std::string crosshair = "crosshair";
    std::string white = "__white";
    SpriteAnim enemyIdle, enemyAttack, enemyPain, enemyDeath;
    SpriteAnim gunIdle, gunFire, gunFlash;
    SpriteAnim explosion, fireball, fireballHit;
    std::string title;                      // optional big title graphic ("" if none)

    // Font: char -> atlas key. Doom's STCFN font is uppercase only.
    std::map<char, std::string> font;
    int fontHeight = 8;
    float fontScale = 1.f;   // suggested on-screen scale
    int textWidth(const std::string& s, float scale) const;

    // Sound names are logical too: "shoot", "explode", "hit", "move", "rotate",
    // "lock", "clear", "redline", "pain", "enemy_die", "levelup", "fireball",
    // "fireball_hit", "gameover", "enemy_sight", "enemy_pain".

private:
    void loadProcedural(audio::Audio& audio);
    bool loadFromWad(const std::filesystem::path& path, audio::Audio& audio);

    render::Atlas atlas_;
    bool usingWad_ = false;
    std::string wadName_;
};

}  // namespace rl::game
