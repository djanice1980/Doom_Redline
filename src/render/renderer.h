#pragma once
// Instanced forward renderer: lit textured cubes + camera-facing billboards +
// screen-space quads, all sampling one atlas. The instance/material model
// (albedo, roughness, metallic, emissive) mirrors what an RTX Remix backend
// consumes, so a Remix API implementation of this same interface can be
// swapped in on Windows (see docs/remix.md).
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/image.h"
#include "render/ktx2.h"
#include "render/mesh_types.h"
#include "render/vk_context.h"

namespace rl::render {

constexpr int kMaxLights = 32;
constexpr int kMaxMaterials = 8;   // material slot 0 = none; slots 1.. carry normal/roughness maps
constexpr uint32_t kShadowMapSize = 2048;

struct CubeInstance {
    glm::vec4 posScale;   // xyz centre, w uniform scale
    glm::vec4 color;      // rgba
    glm::vec4 emissive;   // rgb, w strength
    glm::vec4 uvRect;     // u0 v0 u1 v1
    glm::vec4 params;     // roughness, metallic, anim phase, flags (bit0 pulse)
    glm::vec4 rot;        // x = rotation about X, y = about Y (radians); applied Y then X
};

struct QuadInstance {
    glm::vec4 pos;        // world xyz (billboard) or screen xy px (w = rotation)
    glm::vec4 size;       // w, h, anchorX, anchorY
    glm::vec4 uvRect;
    glm::vec4 color;
    glm::vec4 params;     // mode (0 world / 1 screen), lit, flipX, flags
};

// Optional PBR maps for a material slot. Either image may be empty (no levels) to
// keep the flat default (normal straight up, roughness 1).
struct MaterialMaps {
    Ktx2Image normal;
    Ktx2Image roughness;
};

// Cube instances are drawn in ranges so a range can carry a material slot
// (push constant) without per-instance dynamic indexing in the shader.
struct CubeRange {
    uint32_t first = 0, count = 0;
    int material = 0;
};

struct PointLight {
    glm::vec3 pos;
    float radius;
    glm::vec3 color;
    float intensity;
};

struct FrameParams {
    glm::mat4 view{1.f};
    glm::mat4 proj{1.f};
    glm::vec3 cameraPos{0.f};
    float time = 0.f;
    glm::vec3 sunDir{0.3f, 1.f, 0.5f};
    float sunIntensity = 1.f;
    glm::vec3 ambient{0.25f};
    float fogDensity = 0.f;
    glm::vec3 fogColor{0.f};
    glm::vec3 clearColor{0.02f, 0.02f, 0.04f};
    std::vector<PointLight> lights;
    // Sun shadow map: an orthographic box of half-size shadowRadius centred on shadowCenter, looking along -sunDir.
    glm::vec3 shadowCenter{0.f, 8.f, 10.f};
    float shadowRadius = 26.f;
    float shadowStrength = 0.85f;   // 0 = no shadows
    // Ray tracing (needs Renderer::rayTracingAvailable()): 0 = shadow map,
    // 1 = ray-traced sun, 2 = ray-traced sun and every point light,
    // 3 = 2 plus ray-traced reflections on cubes flagged reflective (params.w bit 1).
    int rtShadows = 0;
};

class Renderer {
public:
    explicit Renderer(VkContext& ctx);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void setAtlas(const Image& atlas);
    // Material slots 1..maps.size() (at most kMaxMaterials - 1); an empty span clears them all.
    void setMaterialMaps(std::span<const MaterialMaps> maps);

    // Static meshes (voxel models): uploaded once, drawn by id with a model matrix.
    uint32_t createMesh(std::span<const MeshVertex> verts, std::span<const uint32_t> indices);
    void destroyMesh(uint32_t id);

    // Renders one frame. Returns false if the frame was skipped (swapchain rebuild).
    bool render(const FrameParams& params,
                std::span<const CubeInstance> cubes,
                std::span<const QuadInstance> worldQuads,
                std::span<const QuadInstance> screenQuads,
                std::span<const MeshInstance> meshes = {},
                std::span<const CubeRange> cubeRanges = {});   // empty = one range, material 0

    // Writes the most recently presented frame to a PNG (blocks the GPU briefly).
    bool screenshot(const std::string& path);
    VkExtent2D extent() const { return ctx_.extent(); }
    bool rayTracingAvailable() const { return rt_; }

private:
    struct Ubo;
    void createDescriptors();
    void createPipelines();
    void createGeometry();
    void writeDescriptors();
    Texture createFlatTexture(uint8_t r, uint8_t g, uint8_t b);
    void ensureInstanceCapacity(uint32_t frame, size_t cubes, size_t quads);
    void ensureMeshInfoCapacity(uint32_t frame, size_t meshes);

    VkContext& ctx_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kFramesInFlight]{};
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline cubePipe_ = VK_NULL_HANDLE;
    VkPipeline quadPipe_ = VK_NULL_HANDLE;
    VkPipeline shadowCubePipe_ = VK_NULL_HANDLE;
    VkPipeline shadowQuadPipe_ = VK_NULL_HANDLE;
    VkPipeline meshPipe_ = VK_NULL_HANDLE;
    VkPipeline shadowMeshPipe_ = VK_NULL_HANDLE;
    // Ray tracing: one bottom-level structure per mesh (and one for the unit cube), a
    // top-level structure rebuilt every frame from the instance lists.
    struct Blas { VkAccelerationStructureKHR as = VK_NULL_HANDLE; Buffer buf; VkDeviceAddress addr = 0; };
    struct MeshRes { Buffer vb, ib; uint32_t indexCount = 0; bool alive = false; Buffer posf; Blas blas; };
    std::vector<MeshRes> meshes_;
    bool rt_ = false;
    Blas cubeBlas_;
    struct Tlas { VkAccelerationStructureKHR as = VK_NULL_HANDLE; Buffer buf; VkDeviceSize bufSize = 0; Buffer instances; size_t instCap = 0; Buffer scratch; VkDeviceSize scratchSize = 0; bool built = false; };
    Tlas tlas_[kFramesInFlight];
    // What a reflection ray hit: mesh instances are resolved through this table
    // (vertex/index buffer addresses, tint, emissive), cubes through the instance buffer.
    struct MeshInfoGpu { uint64_t vb, ib; glm::vec4 color, emissive; };
    Buffer meshInfo_[kFramesInFlight];
    size_t meshInfoCap_[kFramesInFlight]{};
    Blas buildBlas(VkDeviceAddress vtxAddr, uint32_t vtxCount, VkDeviceSize stride, VkDeviceAddress idxAddr, uint32_t triCount);
    void destroyBlas(Blas& b);
    void destroyTlas(Tlas& t);
    void buildTlas(VkCommandBuffer cmd, uint32_t fi, std::span<const CubeInstance> cubes, std::span<const MeshInstance> meshes);
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkSampler matSampler_ = VK_NULL_HANDLE;   // trilinear, repeat: the material maps are mipmapped
    Texture atlas_;
    Texture defNormal_, defRough_;            // 1x1 fallbacks for empty slots
    Texture matNormal_[kMaxMaterials], matRough_[kMaxMaterials];
    Texture shadowMap_;
    Buffer ubo_[kFramesInFlight];
    Buffer cubeInst_[kFramesInFlight];
    Buffer quadInst_[kFramesInFlight];
    size_t cubeCap_[kFramesInFlight]{};
    size_t quadCap_[kFramesInFlight]{};
    Buffer cubeVB_, cubeIB_;
    uint32_t cubeIndexCount_ = 0;
    VkImage lastImage_ = VK_NULL_HANDLE;
};

}  // namespace rl::render
