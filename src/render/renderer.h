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
#include "render/vk_context.h"

namespace rl::render {

constexpr int kMaxLights = 16;

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
};

class Renderer {
public:
    explicit Renderer(VkContext& ctx);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void setAtlas(const Image& atlas);

    // Renders one frame. Returns false if the frame was skipped (swapchain rebuild).
    bool render(const FrameParams& params,
                std::span<const CubeInstance> cubes,
                std::span<const QuadInstance> worldQuads,
                std::span<const QuadInstance> screenQuads);

    // Writes the most recently presented frame to a PNG (blocks the GPU briefly).
    bool screenshot(const std::string& path);
    VkExtent2D extent() const { return ctx_.extent(); }

private:
    struct Ubo;
    void createDescriptors();
    void createPipelines();
    void createGeometry();
    void ensureInstanceCapacity(uint32_t frame, size_t cubes, size_t quads);

    VkContext& ctx_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[kFramesInFlight]{};
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline cubePipe_ = VK_NULL_HANDLE;
    VkPipeline quadPipe_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    Texture atlas_;
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
