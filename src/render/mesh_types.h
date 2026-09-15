#pragma once
// Plain-data mesh types shared by the renderer and the asset builders (no Vulkan).
#include <cstdint>

#include <glm/glm.hpp>

namespace rl::render {

// Voxel-mesh vertex: integer corner in model units, a face-normal index
// (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) and an RGBA8 colour. 12 bytes.
struct MeshVertex {
    int16_t x = 0, y = 0, z = 0, n = 0;
    uint32_t rgba = 0xFFFFFFFFu;
};

struct MeshInstance {
    uint32_t mesh = 0;        // id from Renderer::createMesh
    glm::mat4 model{1.f};
    glm::vec4 color{1.f};     // tint (rgb) and alpha
    glm::vec4 emissive{0.f};  // rgb, w strength
};

}  // namespace rl::render
