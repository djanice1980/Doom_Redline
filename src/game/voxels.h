#pragma once
// Optional voxel models (Cheello's "Voxel Doom" KVX pack) for monsters, items
// and decor. The pack is not shipped with the game: it is found on disk, its
// VOXELDEF is read for per-model angle offsets and scales, and each frame is
// meshed and uploaded the first time it is drawn.
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "render/renderer.h"

namespace rl::game {

struct VoxelModel {
    uint32_t mesh = 0;           // renderer mesh id
    glm::vec3 pivot{0.f};        // model-space origin (Y-up, voxel units)
    float scale = 1.f;           // VOXELDEF Scale
    float angleOffset = 90.f;    // VOXELDEF AngleOffset (degrees)
    int sizeX = 0, sizeY = 0, sizeZ = 0;
    int quads = 0;
};

class VoxelModels {
public:
    // Searches: explicit dir, $REDLINE_VOXELS, the pack shipped with the game (next to the
    // executable on Windows, share/redline on Linux, assets/ in a source checkout), ./voxels,
    // <pref>/voxels, then the Downloads folder the pack was first extracted to.
    static std::optional<std::filesystem::path> findPack(const std::optional<std::filesystem::path>& explicitDir, const std::string& prefDir, const std::string& baseDir = "");

    bool init(render::Renderer& renderer, const std::filesystem::path& dir);
    bool available() const { return !defs_.empty(); }
    const std::string& dirName() const { return dirName_; }
    int definitions() const { return static_cast<int>(defs_.size()); }
    int loaded() const { return loaded_; }
    size_t bytes() const { return bytes_; }

    // Atlas keys look like "TROO_A" (sprite + frame); returns nullptr when the
    // pack has no voxel for that frame (the caller falls back to the sprite).
    const VoxelModel* get(const std::string& atlasKey);

private:
    struct Def { std::string file; float scale = 1.f; float angleOffset = 90.f; };
    void parseVoxelDef(const std::string& text);

    render::Renderer* renderer_ = nullptr;
    std::filesystem::path dir_;
    std::string dirName_;
    std::map<std::string, Def> defs_;                          // "trooa" -> def
    std::map<std::string, std::unique_ptr<VoxelModel>> cache_; // null entry = known miss
    int loaded_ = 0;
    size_t bytes_ = 0;
};

}  // namespace rl::game
