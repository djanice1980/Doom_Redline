#pragma once
// KVX (Build engine / Voxel Doom) loader: turns the first mip level of a .kvx
// file into a greedy-meshed triangle list of axis-aligned faces, one colour per
// vertex from the file's trailing palette.
//
// Output frame is Y-up with the voxel grid's corner at the origin:
//   X = voxel x            (0 .. sizeX)
//   Y = sizeZ - voxel z    (KVX z points down; the model's feet end up at Y = 0)
//   Z = voxel y            (0 .. sizeY)
// `pivot` is the file's pivot point expressed in that same frame, so a model
// matrix of translate(pos) * rotate * scale * translate(-pivot) puts the
// author's origin at `pos`.
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "render/mesh_types.h"

namespace rl::game {

struct VoxelMeshData {
    std::vector<render::MeshVertex> verts;
    std::vector<uint32_t> idx;
    int sizeX = 0, sizeY = 0, sizeZ = 0;   // KVX dims (x, y, z-down)
    glm::vec3 pivot{0.f};                  // in the Y-up output frame
    int voxels = 0;                        // solid voxels in mip 0
    int quads() const { return static_cast<int>(idx.size() / 6); }
};

// Returns false (with `err` set) on a malformed file.
bool loadKvx(const std::vector<uint8_t>& bytes, VoxelMeshData& out, std::string* err = nullptr);

}  // namespace rl::game
