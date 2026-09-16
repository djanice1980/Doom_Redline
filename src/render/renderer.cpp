#include "render/renderer.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "core/png.h"
#include "shaders/cube_frag.h"
#include "shaders/cube_vert.h"
#include "shaders/quad_frag.h"
#include "shaders/quad_vert.h"
#include "shaders/shadow_cube_vert.h"
#include "shaders/shadow_quad_vert.h"
#include "shaders/shadow_quad_frag.h"
#include "shaders/mesh_vert.h"
#include "shaders/mesh_frag.h"
#include "shaders/shadow_mesh_vert.h"
#include "shaders/cube_rt_frag.h"
#include "shaders/mesh_rt_frag.h"
#include "shaders/quad_rt_vert.h"
#include "shaders/post_vert.h"
#include "shaders/bright_frag.h"
#include "shaders/blur_frag.h"
#include "shaders/composite_frag.h"

#include <glm/gtc/matrix_transform.hpp>

namespace rl::render {

// Must match FrameUBO in shaders/common.glsl (std140).
struct Renderer::Ubo {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::vec4 cameraPos;
    glm::vec4 sunDir;
    glm::vec4 ambient;
    glm::vec4 fogColor;
    glm::vec4 screen;
    glm::vec4 lightPos[kMaxLights];
    glm::vec4 lightColor[kMaxLights];
    glm::ivec4 counts;
    glm::mat4 lightViewProj;
    glm::vec4 shadow;
    glm::vec4 misc;   // x, y = atlas size in texels; z = colour grade amount
};

namespace {
struct CubeVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec3 tangent;   // face u axis; bitangent = -cross(n, t) (v grows downwards)
};
constexpr size_t kInitialCubes = 8192;
constexpr size_t kInitialQuads = 4096;
}  // namespace

Renderer::Renderer(VkContext& ctx) : ctx_(ctx), rt_(ctx.rayQuerySupported()) {
    // The atlas is sampled bilinearly with mips; the shaders sharpen the bilinear transition to one
    // screen pixel (sampleAtlas in common.glsl), which keeps the chunky Doom pixels without shimmer.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCheck(vkCreateSampler(ctx_.device(), &sci, nullptr, &sampler_), "vkCreateSampler");
    {
        VkSamplerCreateInfo mi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        mi.magFilter = mi.minFilter = VK_FILTER_LINEAR;
        mi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        mi.addressModeU = mi.addressModeV = mi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        mi.maxLod = VK_LOD_CLAMP_NONE;
        vkCheck(vkCreateSampler(ctx_.device(), &mi, nullptr, &matSampler_), "vkCreateSampler(material)");
    }
    // Shadow map: comparison sampler, white outside the light frustum (= lit).
    VkSamplerCreateInfo ssi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ssi.magFilter = ssi.minFilter = VK_FILTER_LINEAR;
    ssi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ssi.addressModeU = ssi.addressModeV = ssi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ssi.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ssi.compareEnable = VK_TRUE;
    ssi.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCheck(vkCreateSampler(ctx_.device(), &ssi, nullptr, &shadowSampler_), "vkCreateSampler(shadow)");
    shadowMap_ = ctx_.createTexture2D(kShadowMapSize, kShadowMapSize, ctx_.depthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ubo_[i] = ctx_.createBuffer(sizeof(Ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        ensureInstanceCapacity(i, kInitialCubes, kInitialQuads);
    }
    // Placeholder 1x1 white atlas until the real one arrives.
    Image white(1, 1);
    white.set(0, 0, 255, 255, 255);
    atlas_ = ctx_.createTexture2D(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    ctx_.uploadTexture(atlas_, white.rgba.data(), white.rgba.size());

    {
        VkSamplerCreateInfo pi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        pi.magFilter = pi.minFilter = VK_FILTER_LINEAR;
        pi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        pi.addressModeU = pi.addressModeV = pi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(ctx_.device(), &pi, nullptr, &postSampler_), "vkCreateSampler(post)");
    }
    createDescriptors();
    createPostDescriptors();   // the post pipeline layout must exist before createPipelines()
    createPipelines();
    createGeometry();
    defNormal_ = createFlatTexture(128, 128, 255);
    defRough_ = createFlatTexture(255, 255, 255);
    createTargets();
}

void Renderer::setMsaa(bool on) {
    const VkSampleCountFlagBits want = on ? ctx_.maxMsaa() : VK_SAMPLE_COUNT_1_BIT;
    if (want == msaaSamples_) return;
    ctx_.waitIdle();
    msaaSamples_ = want;
    destroyPipelines();
    createPipelines();
    createTargets();
}

void Renderer::createPostDescriptors() {
    VkDevice d = ctx_.device();
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 4;
    lci.pBindings = b;
    vkCheck(vkCreateDescriptorSetLayout(d, &lci, nullptr, &postLayout_), "vkCreateDescriptorSetLayout(post)");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSetCount};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kSetCount;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    vkCheck(vkCreateDescriptorPool(d, &pci, nullptr, &postPool_), "vkCreateDescriptorPool(post)");
    VkDescriptorSetLayout layouts[kSetCount];
    for (auto& l : layouts) l = postLayout_;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = postPool_;
    ai.descriptorSetCount = kSetCount;
    ai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(d, &ai, postSets_), "vkAllocateDescriptorSets(post)");
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &postLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    vkCheck(vkCreatePipelineLayout(d, &plci, nullptr, &postPipeLayout_), "vkCreatePipelineLayout(post)");
}

void Renderer::createTargets() {
    destroyTargets();
    const VkExtent2D ext = ctx_.extent();
    tg_.extent = ext;
    const VkFormat hdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    tg_.hdr = ctx_.createTexture2D(ext.width, ext.height, hdrFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (msaaSamples_ != VK_SAMPLE_COUNT_1_BIT) {
        tg_.msaaColor = ctx_.createTexture2D(ext.width, ext.height, hdrFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, msaaSamples_);
        tg_.msaaDepth = ctx_.createTexture2D(ext.width, ext.height, ctx_.depthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, msaaSamples_);
    }
    for (int i = 0; i < 6; ++i) {
        const uint32_t div = 2u << (i / 2);   // 2, 2, 4, 4, 8, 8
        tg_.bloom[i] = ctx_.createTexture2D(std::max(1u, ext.width / div), std::max(1u, ext.height / div), hdrFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    writePostDescriptors();
}

void Renderer::destroyTargets() {
    if (!tg_.hdr.image) return;
    ctx_.waitIdle();
    ctx_.destroyTexture(tg_.hdr);
    ctx_.destroyTexture(tg_.msaaColor);
    ctx_.destroyTexture(tg_.msaaDepth);
    for (Texture& t : tg_.bloom) ctx_.destroyTexture(t);
    tg_ = Targets{};
}

void Renderer::writePostDescriptors() {
    // One set per input: the HDR image, each bloom buffer, and the composite's four.
    auto write = [&](VkDescriptorSet set, const Texture* const* tex) {
        VkDescriptorImageInfo ii[4];
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            ii[i] = {postSampler_, tex[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(ctx_.device(), 4, w, 0, nullptr);
    };
    const Texture* h4[4] = {&tg_.hdr, &tg_.hdr, &tg_.hdr, &tg_.hdr};
    write(postSets_[kSetHdr], h4);
    for (int i = 0; i < 6; ++i) {
        const Texture* b4[4] = {&tg_.bloom[i], &tg_.bloom[i], &tg_.bloom[i], &tg_.bloom[i]};
        write(postSets_[kSetB0 + i], b4);
    }
    const Texture* c4[4] = {&tg_.hdr, &tg_.bloom[0], &tg_.bloom[2], &tg_.bloom[4]};
    write(postSets_[kSetComposite], c4);
}

Texture Renderer::createFlatTexture(uint8_t r, uint8_t g, uint8_t b) {
    Texture t = ctx_.createTexture2D(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    const uint8_t px[4] = {r, g, b, 255};
    ctx_.uploadTexture(t, px, sizeof(px));
    return t;
}

Renderer::~Renderer() {
    ctx_.waitIdle();
    VkDevice d = ctx_.device();
    ctx_.destroyBuffer(cubeVB_);
    ctx_.destroyBuffer(cubeIB_);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        ctx_.destroyBuffer(ubo_[i]);
        ctx_.destroyBuffer(cubeInst_[i]);
        ctx_.destroyBuffer(quadInst_[i]);
        ctx_.destroyBuffer(meshInfo_[i]);
    }
    for (MeshRes& m : meshes_) if (m.alive) { ctx_.destroyBuffer(m.vb); ctx_.destroyBuffer(m.ib); if (rt_) { ctx_.destroyBuffer(m.posf); destroyBlas(m.blas); } }
    if (rt_) { destroyBlas(cubeBlas_); for (Tlas& t : tlas_) destroyTlas(t); }
    ctx_.destroyTexture(atlas_);
    ctx_.destroyTexture(shadowMap_);
    ctx_.destroyTexture(defNormal_);
    ctx_.destroyTexture(defRough_);
    for (int i = 0; i < kMaxMaterials; ++i) { ctx_.destroyTexture(matNormal_[i]); ctx_.destroyTexture(matRough_[i]); }
    if (matSampler_) vkDestroySampler(d, matSampler_, nullptr);
    destroyTargets();
    destroyPipelines();
    if (postPipeLayout_) vkDestroyPipelineLayout(d, postPipeLayout_, nullptr);
    if (postPool_) vkDestroyDescriptorPool(d, postPool_, nullptr);
    if (postLayout_) vkDestroyDescriptorSetLayout(d, postLayout_, nullptr);
    if (postSampler_) vkDestroySampler(d, postSampler_, nullptr);
    if (shadowSampler_) vkDestroySampler(d, shadowSampler_, nullptr);
    if (pipeLayout_) vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
}

void Renderer::ensureInstanceCapacity(uint32_t frame, size_t cubes, size_t quads) {
    auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (cubes > cubeCap_[frame]) {
        size_t cap = std::max(cubes, cubeCap_[frame] * 2);
        if (cubeInst_[frame].buffer) { ctx_.waitIdle(); ctx_.destroyBuffer(cubeInst_[frame]); }
        cubeInst_[frame] = ctx_.createBuffer(cap * sizeof(CubeInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flags, true);
        cubeCap_[frame] = cap;
    }
    if (quads > quadCap_[frame]) {
        size_t cap = std::max(quads, quadCap_[frame] * 2);
        if (quadInst_[frame].buffer) { ctx_.waitIdle(); ctx_.destroyBuffer(quadInst_[frame]); }
        quadInst_[frame] = ctx_.createBuffer(cap * sizeof(QuadInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, flags, true);
        quadCap_[frame] = cap;
    }
}

void Renderer::ensureMeshInfoCapacity(uint32_t frame, size_t meshes) {
    const size_t need = std::max<size_t>(meshes, 1);
    if (need <= meshInfoCap_[frame]) return;
    if (meshInfo_[frame].buffer) { ctx_.waitIdle(); ctx_.destroyBuffer(meshInfo_[frame]); }
    meshInfoCap_[frame] = std::max(need, meshInfoCap_[frame] * 2);
    meshInfo_[frame] = ctx_.createBuffer(meshInfoCap_[frame] * sizeof(MeshInfoGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
}

void Renderer::createDescriptors() {
    VkDevice d = ctx_.device();
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding tlasBinding{};
    tlasBinding.binding = 3;
    tlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    tlasBinding.descriptorCount = 1;
    tlasBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // Material maps: normal (4) and roughness (5), one array element per material slot.
    VkDescriptorSetLayoutBinding matBindings[2]{};
    for (int i = 0; i < 2; ++i) {
        matBindings[i].binding = 4 + static_cast<uint32_t>(i);
        matBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        matBindings[i].descriptorCount = kMaxMaterials;
        matBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // Reflection hit lookup: cube instances (6) and mesh instance info (7), ray-tracing GPUs only.
    VkDescriptorSetLayoutBinding ssbo[2]{};
    for (int i = 0; i < 2; ++i) {
        ssbo[i].binding = 6 + static_cast<uint32_t>(i);
        ssbo[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssbo[i].descriptorCount = 1;
        ssbo[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    std::vector<VkDescriptorSetLayoutBinding> all = {bindings[0], bindings[1], bindings[2]};
    if (rt_) all.push_back(tlasBinding);
    all.push_back(matBindings[0]);
    all.push_back(matBindings[1]);
    if (rt_) { all.push_back(ssbo[0]); all.push_back(ssbo[1]); }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = static_cast<uint32_t>(all.size());
    lci.pBindings = all.data();
    vkCheck(vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * (2 + 2 * kMaxMaterials)}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight * 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kFramesInFlight;
    pci.poolSizeCount = rt_ ? 4 : 2;
    pci.pPoolSizes = sizes;
    vkCheck(vkCreateDescriptorPool(d, &pci, nullptr, &pool_), "vkCreateDescriptorPool");

    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (auto& l : layouts) l = setLayout_;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(d, &ai, sets_), "vkAllocateDescriptorSets");

    // Push constants carry a mesh instance's model matrix, tint and emissive (96 bytes);
    // cube draws reuse the first int as the material slot of the range being drawn.
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::mat4) + 2 * sizeof(glm::vec4)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    vkCheck(vkCreatePipelineLayout(d, &plci, nullptr, &pipeLayout_), "vkCreatePipelineLayout");
}

void Renderer::setAtlas(const Image& img) {
    ctx_.waitIdle();
    ctx_.destroyTexture(atlas_);
    // Three mip levels (down to a quarter): enough to stop far walls shimmering, few enough that
    // neighbouring atlas regions do not bleed into each other much. Transparent texels are
    // excluded from the average so sprite edges keep their colour.
    std::vector<std::vector<uint8_t>> levels;
    levels.push_back(img.rgba);
    int w = img.width, h = img.height;
    for (int lvl = 1; lvl < 3 && w > 1 && h > 1; ++lvl) {
        const std::vector<uint8_t>& src = levels.back();
        const int nw = w / 2, nh = h / 2;
        std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                int sum[4] = {0, 0, 0, 0}, opaque = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint8_t* p = &src[(static_cast<size_t>(y * 2 + dy) * w + (x * 2 + dx)) * 4];
                        if (p[3] > 0) { sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++opaque; }
                        sum[3] += p[3];
                    }
                uint8_t* o = &dst[(static_cast<size_t>(y) * nw + x) * 4];
                o[0] = static_cast<uint8_t>(opaque ? sum[0] / opaque : 0);
                o[1] = static_cast<uint8_t>(opaque ? sum[1] / opaque : 0);
                o[2] = static_cast<uint8_t>(opaque ? sum[2] / opaque : 0);
                o[3] = static_cast<uint8_t>(sum[3] / 4);
            }
        levels.push_back(std::move(dst));
        w = nw; h = nh;
    }
    atlas_ = ctx_.createTexture2D(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, static_cast<uint32_t>(levels.size()));
    ctx_.uploadTextureLevels(atlas_, levels);
    writeDescriptors();
}

void Renderer::setMaterialMaps(std::span<const MaterialMaps> maps) {
    ctx_.waitIdle();
    for (int i = 0; i < kMaxMaterials; ++i) { ctx_.destroyTexture(matNormal_[i]); ctx_.destroyTexture(matRough_[i]); }
    auto upload = [&](const Ktx2Image& img, Texture& out, const char* what) {
        if (img.levels.empty()) return;
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        switch (img.vkFormat) {
            case 37: fmt = VK_FORMAT_R8G8B8A8_UNORM; break;
            case 43: fmt = VK_FORMAT_R8G8B8A8_SRGB; break;
            case 141: fmt = VK_FORMAT_BC5_UNORM_BLOCK; break;
            case 145: fmt = VK_FORMAT_BC7_UNORM_BLOCK; break;
            default: break;
        }
        if (fmt == VK_FORMAT_UNDEFINED || (img.blockCompressed() && !ctx_.bcTexturesSupported())) {
            std::fprintf(stderr, "[materials] %s: unsupported KTX2 format %u, using the flat default\n", what, img.vkFormat);
            return;
        }
        out = ctx_.createTexture2D(img.width, img.height, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, static_cast<uint32_t>(img.levels.size()));
        ctx_.uploadTextureLevels(out, img.levels);
    };
    for (size_t i = 0; i < maps.size() && i + 1 < static_cast<size_t>(kMaxMaterials); ++i) {
        upload(maps[i].normal, matNormal_[i + 1], "normal");
        upload(maps[i].roughness, matRough_[i + 1], "roughness");
    }
    if (atlas_.view) writeDescriptors();
}

void Renderer::writeDescriptors() {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo bi{ubo_[i].buffer, 0, sizeof(Ubo)};
        VkDescriptorImageInfo ii{sampler_, atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo si{shadowSampler_, shadowMap_.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo ni[kMaxMaterials], ri[kMaxMaterials];
        for (int m = 0; m < kMaxMaterials; ++m) {
            ni[m] = {matSampler_, matNormal_[m].view ? matNormal_[m].view : defNormal_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            ri[m] = {matSampler_, matRough_[m].view ? matRough_[m].view : defRough_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        VkWriteDescriptorSet w[5]{};
        w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3].dstSet = sets_[i];
        w[3].dstBinding = 4;
        w[3].descriptorCount = kMaxMaterials;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = ni;
        w[4] = w[3];
        w[4].dstBinding = 5;
        w[4].pImageInfo = ri;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet = sets_[i];
        w[2].dstBinding = 2;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &si;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = sets_[i];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = sets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(ctx_.device(), 5, w, 0, nullptr);
    }
}

void Renderer::destroyPipelines() {
    VkDevice d = ctx_.device();
    for (VkPipeline* p : {&cubePipe_, &quadPipe_, &shadowCubePipe_, &shadowQuadPipe_, &meshPipe_, &meshBlendPipe_, &shadowMeshPipe_, &quadLdrPipe_, &brightPipe_, &blurPipe_, &compositePipe_}) {
        if (*p) vkDestroyPipeline(d, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
}

void Renderer::createPipelines() {
    VkDevice d = ctx_.device();
    VkShaderModule cubeVS = ctx_.createShader(shaders::cube_vert, shaders::cube_vert_size);
    VkShaderModule cubeFS = rt_ ? ctx_.createShader(shaders::cube_rt_frag, shaders::cube_rt_frag_size) : ctx_.createShader(shaders::cube_frag, shaders::cube_frag_size);
    VkShaderModule quadVS = rt_ ? ctx_.createShader(shaders::quad_rt_vert, shaders::quad_rt_vert_size) : ctx_.createShader(shaders::quad_vert, shaders::quad_vert_size);
    VkShaderModule quadFS = ctx_.createShader(shaders::quad_frag, shaders::quad_frag_size);

    // World pass: HDR colour (multisampled when MSAA is on); HUD pass: the swapchain image.
    const VkFormat hdrFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormat swapFmt = ctx_.swapchainFormat();
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &hdrFmt;
    rci.depthAttachmentFormat = ctx_.depthFormat();
    VkPipelineRenderingCreateInfo ldrRci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ldrRci.colorAttachmentCount = 1;
    ldrRci.pColorAttachmentFormats = &swapFmt;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineRenderingCreateInfo shadowRci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    shadowRci.colorAttachmentCount = 0;
    shadowRci.depthAttachmentFormat = ctx_.depthFormat();

    // kind: 0 world pass (HDR, MSAA), 1 shadow map (depth only), 2 HUD on the swapchain (no depth).
    auto makePipeline = [&](VkShaderModule vs, VkShaderModule fs, const VkPipelineVertexInputStateCreateInfo& vi, bool cull, bool blend, bool depthOnly = false, int kind = 0) {
        if (depthOnly) kind = 1;
        VkPipelineMultisampleStateCreateInfo msk{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msk.rasterizationSamples = kind == 0 ? msaaSamples_ : VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dsk = dss;
        if (kind == 2) { dsk.depthTestEnable = VK_FALSE; dsk.depthWriteEnable = VK_FALSE; }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.f;
        if (depthOnly) {
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.depthBiasEnable = VK_TRUE;
            rs.depthBiasConstantFactor = 1.5f;
            rs.depthBiasSlopeFactor = 2.5f;
        }
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        cba.blendEnable = blend ? VK_TRUE : VK_FALSE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = kind == 1 ? &shadowRci : kind == 2 ? &ldrRci : &rci;
        ci.stageCount = fs ? 2 : 1;
        ci.pStages = stages;
        if (depthOnly) cb.attachmentCount = 0;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &msk;
        ci.pDepthStencilState = kind == 2 ? &dsk : &dss;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = pipeLayout_;
        VkPipeline p;
        vkCheck(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
        return p;
    };

    // Cube: binding 0 per-vertex, binding 1 per-instance.
    {
        VkVertexInputBindingDescription b[2] = {
            {0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(CubeInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
        VkVertexInputAttributeDescription a[10] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CubeVertex, uv)},
            {9, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, tangent)},
            {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, posScale)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, color)},
            {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, emissive)},
            {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, uvRect)},
            {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, params)},
            {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, rot)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = b;
        vi.vertexAttributeDescriptionCount = 10;
        vi.pVertexAttributeDescriptions = a;
        cubePipe_ = makePipeline(cubeVS, cubeFS, vi, true, false);
        VkShaderModule shVS = ctx_.createShader(shaders::shadow_cube_vert, shaders::shadow_cube_vert_size);
        shadowCubePipe_ = makePipeline(shVS, VK_NULL_HANDLE, vi, false, false, true);
        vkDestroyShaderModule(d, shVS, nullptr);
    }
    // Quad: binding 0 per-instance only.
    {
        VkVertexInputBindingDescription b[1] = {{0, sizeof(QuadInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
        VkVertexInputAttributeDescription a[5] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, size)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, uvRect)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, color)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(QuadInstance, params)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = b;
        vi.vertexAttributeDescriptionCount = 5;
        vi.pVertexAttributeDescriptions = a;
        quadPipe_ = makePipeline(quadVS, quadFS, vi, false, true);
        quadLdrPipe_ = makePipeline(quadVS, quadFS, vi, false, true, false, 2);
        VkShaderModule shVS = ctx_.createShader(shaders::shadow_quad_vert, shaders::shadow_quad_vert_size);
        VkShaderModule shFS = ctx_.createShader(shaders::shadow_quad_frag, shaders::shadow_quad_frag_size);
        shadowQuadPipe_ = makePipeline(shVS, shFS, vi, false, false, true);
        vkDestroyShaderModule(d, shVS, nullptr);
        vkDestroyShaderModule(d, shFS, nullptr);
    }
    // Mesh: binding 0 per-vertex (int16 position + normal index, RGBA8 colour).
    {
        VkShaderModule meshVS = ctx_.createShader(shaders::mesh_vert, shaders::mesh_vert_size);
        VkShaderModule meshFS = rt_ ? ctx_.createShader(shaders::mesh_rt_frag, shaders::mesh_rt_frag_size) : ctx_.createShader(shaders::mesh_frag, shaders::mesh_frag_size);
        VkShaderModule shVS = ctx_.createShader(shaders::shadow_mesh_vert, shaders::shadow_mesh_vert_size);
        VkVertexInputBindingDescription b[1] = {{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
        VkVertexInputAttributeDescription a[2] = {
            {0, 0, VK_FORMAT_R16G16B16A16_SINT, offsetof(MeshVertex, x)},
            {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(MeshVertex, rgba)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = b;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = a;
        meshPipe_ = makePipeline(meshVS, meshFS, vi, true, false);
        meshBlendPipe_ = makePipeline(meshVS, meshFS, vi, true, true);
        shadowMeshPipe_ = makePipeline(shVS, VK_NULL_HANDLE, vi, false, false, true);
        vkDestroyShaderModule(d, meshVS, nullptr);
        vkDestroyShaderModule(d, meshFS, nullptr);
        vkDestroyShaderModule(d, shVS, nullptr);
    }
    vkDestroyShaderModule(d, cubeVS, nullptr);
    vkDestroyShaderModule(d, cubeFS, nullptr);
    vkDestroyShaderModule(d, quadVS, nullptr);
    vkDestroyShaderModule(d, quadFS, nullptr);

    // Post-processing pipelines: fullscreen triangle, no vertex input, no depth, no blending.
    {
        VkShaderModule postVS = ctx_.createShader(shaders::post_vert, shaders::post_vert_size);
        VkShaderModule brightFS = ctx_.createShader(shaders::bright_frag, shaders::bright_frag_size);
        VkShaderModule blurFS = ctx_.createShader(shaders::blur_frag, shaders::blur_frag_size);
        VkShaderModule compFS = ctx_.createShader(shaders::composite_frag, shaders::composite_frag_size);
        auto makePost = [&](VkShaderModule fs, VkFormat fmt) {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = postVS;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fs;
            stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo pms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            pms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState cba{};
            cba.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &cba;
            VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            prci.colorAttachmentCount = 1;
            prci.pColorAttachmentFormats = &fmt;
            VkDynamicState pdyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo pds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            pds.dynamicStateCount = 2;
            pds.pDynamicStates = pdyn;
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            ci.pNext = &prci;
            ci.stageCount = 2;
            ci.pStages = stages;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &pms;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &pds;
            ci.layout = postPipeLayout_;
            VkPipeline p;
            vkCheck(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines(post)");
            return p;
        };
        brightPipe_ = makePost(brightFS, hdrFmt);
        blurPipe_ = makePost(blurFS, hdrFmt);
        compositePipe_ = makePost(compFS, swapFmt);
        vkDestroyShaderModule(d, postVS, nullptr);
        vkDestroyShaderModule(d, brightFS, nullptr);
        vkDestroyShaderModule(d, blurFS, nullptr);
        vkDestroyShaderModule(d, compFS, nullptr);
    }
}

uint32_t Renderer::createMesh(std::span<const MeshVertex> verts, std::span<const uint32_t> indices) {
    MeshRes m;
    const VkBufferUsageFlags asIn = rt_ ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) : 0u;
    // With ray tracing the fragment shader reads vertices/indices of reflected meshes by device address.
    const VkBufferUsageFlags refl = rt_ ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0u;
    m.vb = ctx_.createBuffer(verts.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | refl, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, rt_);
    m.ib = ctx_.createBuffer(indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | asIn | refl, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, rt_);
    ctx_.uploadBuffer(m.vb, verts.data(), verts.size_bytes());
    ctx_.uploadBuffer(m.ib, indices.data(), indices.size_bytes());
    m.indexCount = static_cast<uint32_t>(indices.size());
    m.alive = true;
    if (rt_) {
        // Acceleration structures want float positions; the draw path keeps its 12-byte vertices.
        std::vector<glm::vec3> pos(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) pos[i] = glm::vec3(verts[i].x, verts[i].y, verts[i].z);
        m.posf = ctx_.createBuffer(pos.size() * sizeof(glm::vec3), VK_BUFFER_USAGE_TRANSFER_DST_BIT | asIn, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
        ctx_.uploadBuffer(m.posf, pos.data(), pos.size() * sizeof(glm::vec3));
        m.blas = buildBlas(ctx_.bufferAddress(m.posf), static_cast<uint32_t>(pos.size()), sizeof(glm::vec3), ctx_.bufferAddress(m.ib), static_cast<uint32_t>(indices.size() / 3));
    }
    for (size_t i = 0; i < meshes_.size(); ++i)
        if (!meshes_[i].alive) { meshes_[i] = m; return static_cast<uint32_t>(i); }
    meshes_.push_back(m);
    return static_cast<uint32_t>(meshes_.size() - 1);
}

void Renderer::destroyMesh(uint32_t id) {
    if (id >= meshes_.size() || !meshes_[id].alive) return;
    ctx_.waitIdle();
    ctx_.destroyBuffer(meshes_[id].vb);
    ctx_.destroyBuffer(meshes_[id].ib);
    if (rt_) { ctx_.destroyBuffer(meshes_[id].posf); destroyBlas(meshes_[id].blas); }
    meshes_[id] = MeshRes{};
}

// --- acceleration structures ---------------------------------------------------
Renderer::Blas Renderer::buildBlas(VkDeviceAddress vtxAddr, uint32_t vtxCount, VkDeviceSize stride, VkDeviceAddress idxAddr, uint32_t triCount) {
    const VkContext::RtFuncs& rt = ctx_.rt();
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = vtxAddr;
    geom.geometry.triangles.vertexStride = stride;
    geom.geometry.triangles.maxVertex = vtxCount - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = idxAddr;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.getBuildSizes(ctx_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &triCount, &sizes);
    Blas b;
    b.buf = ctx_.createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = b.buf.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    vkCheck(rt.create(ctx_.device(), &ci, nullptr, &b.as), "vkCreateAccelerationStructureKHR");
    const VkDeviceSize align = ctx_.scratchAlignment();
    Buffer scratch = ctx_.createBuffer(sizes.buildScratchSize + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
    VkDeviceAddress sa = ctx_.bufferAddress(scratch);
    sa = (sa + align - 1) & ~(align - 1);
    bi.dstAccelerationStructure = b.as;
    bi.scratchData.deviceAddress = sa;
    VkAccelerationStructureBuildRangeInfoKHR range{triCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    ctx_.oneTimeSubmit([&](VkCommandBuffer cmd) { rt.cmdBuild(cmd, 1, &bi, &pRange); });
    ctx_.destroyBuffer(scratch);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = b.as;
    b.addr = rt.getAddress(ctx_.device(), &ai);
    return b;
}

void Renderer::destroyBlas(Blas& b) {
    if (b.as) ctx_.rt().destroy(ctx_.device(), b.as, nullptr);
    if (b.buf.buffer) ctx_.destroyBuffer(b.buf);
    b = Blas{};
}

void Renderer::destroyTlas(Tlas& t) {
    if (t.as) ctx_.rt().destroy(ctx_.device(), t.as, nullptr);
    if (t.buf.buffer) ctx_.destroyBuffer(t.buf);
    if (t.instances.buffer) ctx_.destroyBuffer(t.instances);
    if (t.scratch.buffer) ctx_.destroyBuffer(t.scratch);
    t = Tlas{};
}

// Rebuilds this frame's top-level structure from the cube and mesh instance lists.
void Renderer::buildTlas(VkCommandBuffer cmd, uint32_t fi, std::span<const CubeInstance> cubes, std::span<const MeshInstance> meshes) {
    const VkContext::RtFuncs& rt = ctx_.rt();
    Tlas& t = tlas_[fi];
    std::vector<VkAccelerationStructureInstanceKHR> inst;
    inst.reserve(cubes.size() + meshes.size());
    // Custom index tells a reflection ray what it hit: cube i, or mesh entry j | 0x800000
    // (j indexes the mesh info table filled in render(), same alive filter and order).
    auto put = [&](const glm::mat4& M, VkDeviceAddress blas, uint32_t custom) {
        VkAccelerationStructureInstanceKHR in{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) in.transform.matrix[r][c] = M[c][r];
        in.instanceCustomIndex = custom & 0xFFFFFFu;
        in.mask = 0xFF;
        in.instanceShaderBindingTableRecordOffset = 0;
        in.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        in.accelerationStructureReference = blas;
        inst.push_back(in);
    };
    for (size_t i = 0; i < cubes.size(); ++i) {
        const CubeInstance& c = cubes[i];
        glm::mat4 M = glm::translate(glm::mat4(1.f), glm::vec3(c.posScale));
        M = glm::rotate(M, c.rot.x, glm::vec3(1.f, 0.f, 0.f));
        M = glm::rotate(M, c.rot.y, glm::vec3(0.f, 1.f, 0.f));
        M = glm::scale(M, glm::vec3(c.posScale.w));
        put(M, cubeBlas_.addr, static_cast<uint32_t>(i));
    }
    uint32_t j = 0;
    for (const MeshInstance& mi : meshes)
        if (mi.mesh < meshes_.size() && meshes_[mi.mesh].alive && mi.color.a >= 0.999f) put(mi.model, meshes_[mi.mesh].blas.addr, 0x800000u | j++);
    const uint32_t n = static_cast<uint32_t>(inst.size());

    if (n > t.instCap) {
        ctx_.waitIdle();
        if (t.instances.buffer) ctx_.destroyBuffer(t.instances);
        t.instCap = std::max<size_t>(n, t.instCap * 2);
        t.instances = ctx_.createBuffer(t.instCap * sizeof(VkAccelerationStructureInstanceKHR),
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, true);
    }
    if (n) std::memcpy(t.instances.mapped, inst.data(), n * sizeof(VkAccelerationStructureInstanceKHR));

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress = ctx_.bufferAddress(t.instances);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rt.getBuildSizes(ctx_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sizes);
    if (sizes.accelerationStructureSize > t.bufSize) {
        ctx_.waitIdle();
        if (t.as) rt.destroy(ctx_.device(), t.as, nullptr);
        if (t.buf.buffer) ctx_.destroyBuffer(t.buf);
        t.bufSize = sizes.accelerationStructureSize * 3 / 2;
        t.buf = ctx_.createBuffer(t.bufSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = t.buf.buffer;
        ci.size = t.bufSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        vkCheck(rt.create(ctx_.device(), &ci, nullptr, &t.as), "vkCreateAccelerationStructureKHR(tlas)");
    }
    const VkDeviceSize align = ctx_.scratchAlignment();
    if (sizes.buildScratchSize + align > t.scratchSize) {
        ctx_.waitIdle();
        if (t.scratch.buffer) ctx_.destroyBuffer(t.scratch);
        t.scratchSize = (sizes.buildScratchSize + align) * 3 / 2;
        t.scratch = ctx_.createBuffer(t.scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
    }
    VkDeviceAddress sa = ctx_.bufferAddress(t.scratch);
    sa = (sa + align - 1) & ~(align - 1);
    bi.dstAccelerationStructure = t.as;
    bi.scratchData.deviceAddress = sa;
    VkAccelerationStructureBuildRangeInfoKHR range{n, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    rt.cmdBuild(cmd, 1, &bi, &pRange);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    t.built = true;
}

void Renderer::createGeometry() {
    // Unit cube centred at the origin, 24 vertices (per-face normals/uvs), CCW winding.
    struct Face { glm::vec3 n, u, v; };
    const Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},    // +Z (front)
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},  // -Z
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},   // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -X
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},   // +Y (top)
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},   // -Y
    };
    std::vector<CubeVertex> verts;
    std::vector<uint32_t> idx;
    for (const Face& f : faces) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const glm::vec2 corners[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (int i = 0; i < 4; ++i) {
            glm::vec3 p = f.n * 0.5f + f.u * corners[i].x + f.v * corners[i].y;
            verts.push_back({p, f.n, uvs[i], f.u});
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    cubeIndexCount_ = static_cast<uint32_t>(idx.size());
    const VkBufferUsageFlags asIn = rt_ ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) : 0u;
    cubeVB_ = ctx_.createBuffer(verts.size() * sizeof(CubeVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | asIn, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, rt_);
    cubeIB_ = ctx_.createBuffer(idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | asIn, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, rt_);
    ctx_.uploadBuffer(cubeVB_, verts.data(), verts.size() * sizeof(CubeVertex));
    ctx_.uploadBuffer(cubeIB_, idx.data(), idx.size() * sizeof(uint32_t));
    if (rt_) cubeBlas_ = buildBlas(ctx_.bufferAddress(cubeVB_), static_cast<uint32_t>(verts.size()), sizeof(CubeVertex), ctx_.bufferAddress(cubeIB_), cubeIndexCount_ / 3);
}

bool Renderer::render(const FrameParams& params, std::span<const CubeInstance> cubes,
                      std::span<const QuadInstance> worldQuads, std::span<const QuadInstance> screenQuads,
                      std::span<const MeshInstance> meshes, std::span<const CubeRange> cubeRanges) {
    VkContext::FrameCtx frame;
    if (!ctx_.beginFrame(frame)) return false;
    const uint32_t fi = frame.frameIndex;
    const VkExtent2D ext = ctx_.extent();
    if (tg_.extent.width != ext.width || tg_.extent.height != ext.height) createTargets();

    // Screen quads flagged HDR (params.w bit 1: the weapon and its flash) are drawn in the
    // world pass so they bloom; the rest go on top of the tone-mapped image.
    quadScratch_.clear();
    quadScratch_.reserve(screenQuads.size());
    for (const QuadInstance& q : screenQuads) if ((static_cast<int>(q.params.w) & 2) != 0) quadScratch_.push_back(q);
    const size_t hdrQuads = quadScratch_.size();
    for (const QuadInstance& q : screenQuads) if ((static_cast<int>(q.params.w) & 2) == 0) quadScratch_.push_back(q);

    // Uniforms
    Ubo u{};
    u.view = params.view;
    u.proj = params.proj;
    u.viewProj = params.proj * params.view;
    u.cameraPos = glm::vec4(params.cameraPos, params.time);
    u.sunDir = glm::vec4(glm::normalize(params.sunDir), params.sunIntensity);
    u.ambient = glm::vec4(params.ambient, params.fogDensity);
    u.fogColor = glm::vec4(params.fogColor, 0.f);
    u.screen = glm::vec4(static_cast<float>(ext.width), static_cast<float>(ext.height), 1.f / ext.width, 1.f / ext.height);
    int nl = static_cast<int>(std::min<size_t>(params.lights.size(), kMaxLights));
    for (int i = 0; i < nl; ++i) {
        u.lightPos[i] = glm::vec4(params.lights[i].pos, params.lights[i].radius);
        u.lightColor[i] = glm::vec4(params.lights[i].color, params.lights[i].intensity);
    }
    u.counts = glm::ivec4(nl, rt_ ? params.rtShadows : 0, 0, 0);
    u.misc = glm::vec4(static_cast<float>(atlas_.width), static_cast<float>(atlas_.height), params.grade, 0.f);
    {
        glm::vec3 dir = glm::normalize(params.sunDir);
        glm::vec3 up = std::fabs(dir.y) > 0.95f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
        float r = params.shadowRadius;
        glm::mat4 lv = glm::lookAt(params.shadowCenter + dir * r, params.shadowCenter, up);
        glm::mat4 lp = glm::ortho(-r, r, -r, r, 0.f, 2.f * r);
        u.lightViewProj = lp * lv;
        u.shadow = glm::vec4(1.f / static_cast<float>(kShadowMapSize), 0.0015f, params.shadowStrength, 0.08f);
    }
    std::memcpy(ubo_[fi].mapped, &u, sizeof(u));

    // Instances (world + screen quads share one buffer, drawn as two ranges).
    size_t quadTotal = worldQuads.size() + screenQuads.size();
    ensureInstanceCapacity(fi, cubes.size(), quadTotal);
    if (!cubes.empty()) std::memcpy(cubeInst_[fi].mapped, cubes.data(), cubes.size() * sizeof(CubeInstance));
    auto* qdst = static_cast<QuadInstance*>(quadInst_[fi].mapped);
    if (!worldQuads.empty()) std::memcpy(qdst, worldQuads.data(), worldQuads.size() * sizeof(QuadInstance));
    if (!quadScratch_.empty()) std::memcpy(qdst + worldQuads.size(), quadScratch_.data(), quadScratch_.size() * sizeof(QuadInstance));

    VkCommandBuffer cmd = frame.cmd;
    if (rt_) {
        // The top-level structure is rebuilt whenever ray-traced shadows are on (and once
        // regardless, so the descriptor always points at a valid structure).
        if (params.rtShadows > 0 || !tlas_[fi].built) buildTlas(cmd, fi, cubes, meshes);
        // Mesh info for reflection hits, in TLAS order.
        ensureMeshInfoCapacity(fi, meshes.size());
        {
            auto* dst = static_cast<MeshInfoGpu*>(meshInfo_[fi].mapped);
            size_t j = 0;
            for (const MeshInstance& mi : meshes) {
                if (mi.mesh >= meshes_.size() || !meshes_[mi.mesh].alive || mi.color.a < 0.999f) continue;
                const MeshRes& m = meshes_[mi.mesh];
                dst[j++] = {ctx_.bufferAddress(m.vb), ctx_.bufferAddress(m.ib), mi.color, mi.emissive};
            }
            if (j == 0) dst[0] = {};
        }
        VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        wa.accelerationStructureCount = 1;
        wa.pAccelerationStructures = &tlas_[fi].as;
        VkDescriptorBufferInfo cb{cubeInst_[fi].buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo mb{meshInfo_[fi].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].pNext = &wa;
        w[0].dstSet = sets_[fi];
        w[0].dstBinding = 3;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = sets_[fi];
        w[1].dstBinding = 6;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &cb;
        w[2] = w[1];
        w[2].dstBinding = 7;
        w[2].pBufferInfo = &mb;
        vkUpdateDescriptorSets(ctx_.device(), 3, w, 0, nullptr);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &sets_[fi], 0, nullptr);
    struct MeshPush { glm::mat4 model; glm::vec4 color; glm::vec4 emissive; };
    // translucent: draw only instances with colour alpha < 1 (else only the opaque ones).
    auto drawMeshes = [&](VkPipeline pipe, bool translucent = false) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        uint32_t bound = UINT32_MAX;
        for (const MeshInstance& mi : meshes) {
            if (mi.mesh >= meshes_.size() || !meshes_[mi.mesh].alive) continue;
            if ((mi.color.a < 0.999f) != translucent) continue;
            const MeshRes& m = meshes_[mi.mesh];
            if (bound != mi.mesh) {
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vb.buffer, &off);
                vkCmdBindIndexBuffer(cmd, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound = mi.mesh;
            }
            MeshPush pc{mi.model, mi.color, mi.emissive};
            vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
        }
    };

    // --- Shadow pass: depth from the sun into the shadow map --------------------
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.image = shadowMap_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo sdepth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sdepth.imageView = shadowMap_.view;
        sdepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sdepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sdepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sdepth.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sri.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
        sri.layerCount = 1;
        sri.pDepthAttachment = &sdepth;
        vkCmdBeginRendering(cmd, &sri);
        VkViewport svp{0.f, 0.f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.f, 1.f};
        VkRect2D ssc{{0, 0}, {kShadowMapSize, kShadowMapSize}};
        vkCmdSetViewport(cmd, 0, 1, &svp);
        vkCmdSetScissor(cmd, 0, 1, &ssc);
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        if (!cubes.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowCubePipe_);
            VkBuffer vbs[2] = {cubeVB_.buffer, cubeInst_[fi].buffer};
            VkDeviceSize offs[2] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(cmd, cubeIB_.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, cubeIndexCount_, static_cast<uint32_t>(cubes.size()), 0, 0, 0);
        }
        if (!worldQuads.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowQuadPipe_);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &quadInst_[fi].buffer, &off);
            vkCmdDraw(cmd, 6, static_cast<uint32_t>(worldQuads.size()), 0, 0);
        }
        if (!meshes.empty()) drawMeshes(shadowMeshPipe_);
        vkCmdEndRendering(cmd);

        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // --- World pass into the HDR target (via the MSAA image when multisampling) ------
    const bool msaa = msaaSamples_ != VK_SAMPLE_COUNT_1_BIT;
    auto imageBarrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 kFrag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    imageBarrier(tg_.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag | kColorOut, 0, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (msaa) {
        imageBarrier(tg_.msaaColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColorOut, 0, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(tg_.msaaDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? tg_.msaaColor.view : tg_.hdr.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{params.clearColor.r, params.clearColor.g, params.clearColor.b, 1.f}};
    if (msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = tg_.hdr.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = msaa ? tg_.msaaDepth.view : frame.depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    // Negative-height viewport (core since 1.1): keeps GL-style +Y up so glm
    // projections and CCW winding work unchanged.
    VkViewport viewport{0.f, static_cast<float>(ext.height), static_cast<float>(ext.width), -static_cast<float>(ext.height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (!cubes.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cubePipe_);
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        VkBuffer vbs[2] = {cubeVB_.buffer, cubeInst_[fi].buffer};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, cubeIB_.buffer, 0, VK_INDEX_TYPE_UINT32);
        const CubeRange whole{0, static_cast<uint32_t>(cubes.size()), 0};
        std::span<const CubeRange> ranges = cubeRanges.empty() ? std::span<const CubeRange>(&whole, 1) : cubeRanges;
        for (const CubeRange& r : ranges) {
            if (r.count == 0 || r.first + r.count > cubes.size()) continue;
            const int mat = (r.material > 0 && r.material < kMaxMaterials) ? r.material : 0;
            vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int), &mat);
            vkCmdDrawIndexed(cmd, cubeIndexCount_, r.count, 0, 0, r.first);
        }
    }
    if (!meshes.empty()) {
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        drawMeshes(meshPipe_);
    }
    if (quadTotal > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quadPipe_);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &quadInst_[fi].buffer, &off);
        if (!worldQuads.empty()) {
            vkCmdSetDepthTestEnable(cmd, VK_TRUE);
            vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
            vkCmdDraw(cmd, 6, static_cast<uint32_t>(worldQuads.size()), 0, 0);
        }
    }
    if (!meshes.empty()) {   // translucent voxel models (explosions) over the opaque world and sprites
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        drawMeshes(meshBlendPipe_, true);
    }
    if (hdrQuads > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quadPipe_);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &quadInst_[fi].buffer, &off);
        vkCmdSetDepthTestEnable(cmd, VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        vkCmdDraw(cmd, 6, static_cast<uint32_t>(hdrQuads), 0, static_cast<uint32_t>(worldQuads.size()));
    }
    vkCmdEndRendering(cmd);
    imageBarrier(tg_.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag, VK_ACCESS_2_SHADER_READ_BIT);

    // --- Post: bloom chain, then composite + HUD into the swapchain image -------------
    auto fullscreen = [&](VkImageView view, VkExtent2D size, VkPipeline pipe, VkDescriptorSet set, glm::vec4 pc) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo pri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        pri.renderArea = {{0, 0}, size};
        pri.layerCount = 1;
        pri.colorAttachmentCount = 1;
        pri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &pri);
        VkViewport pvp{0.f, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 1.f};
        VkRect2D psc{{0, 0}, size};
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        vkCmdSetScissor(cmd, 0, 1, &psc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, postPipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    auto bloomPass = [&](int dst, VkPipeline pipe, VkDescriptorSet set, glm::vec4 pc) {
        Texture& t = tg_.bloom[dst];
        imageBarrier(t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kFrag, 0, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        fullscreen(t.view, {t.width, t.height}, pipe, set, pc);
        vkCmdEndRendering(cmd);
        imageBarrier(t.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kFrag, VK_ACCESS_2_SHADER_READ_BIT);
    };
    const bool bloom = params.bloom > 0.f;
    if (bloom) {
        auto step = [&](int i) { return glm::vec4(1.f / tg_.bloom[i].width, 1.f / tg_.bloom[i].height, 0.f, 0.f); };
        bloomPass(0, brightPipe_, postSets_[kSetHdr], glm::vec4(1.0f, 0.5f, 0.f, 0.f));
        for (int lvl = 0; lvl < 3; ++lvl) {
            const int a = lvl * 2, b = a + 1;
            if (lvl > 0) bloomPass(a, blurPipe_, postSets_[kSetB0 + a - 2], glm::vec4(0.f));   // downsample from the level above
            bloomPass(b, blurPipe_, postSets_[kSetB0 + a], glm::vec4(step(a).x, 0.f, 0.f, 0.f));
            bloomPass(a, blurPipe_, postSets_[kSetB0 + b], glm::vec4(0.f, step(a).y, 0.f, 0.f));
        }
    }
    fullscreen(frame.imageView, ext, compositePipe_, postSets_[bloom ? kSetComposite : kSetHdr], glm::vec4(params.exposure, bloom ? params.bloom : 0.f, 0.75f, params.grade));
    // HUD and text on top, untouched by the tone curve.
    if (quadScratch_.size() > hdrQuads) {
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quadLdrPipe_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &sets_[fi], 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &quadInst_[fi].buffer, &off);
        vkCmdDraw(cmd, 6, static_cast<uint32_t>(quadScratch_.size() - hdrQuads), 0, static_cast<uint32_t>(worldQuads.size() + hdrQuads));
    }
    vkCmdEndRendering(cmd);
    lastImage_ = frame.image;
    ctx_.endFrame(frame);
    return true;
}

bool Renderer::screenshot(const std::string& path) {
    if (!lastImage_) return false;
    ctx_.waitIdle();
    VkExtent2D ext = ctx_.extent();
    std::vector<uint8_t> px = ctx_.readbackImage(lastImage_, ext.width, ext.height, ctx_.swapchainFormat());
    Image img(static_cast<int>(ext.width), static_cast<int>(ext.height));
    img.rgba = std::move(px);
    for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    return writePng(path, img);
}

}  // namespace rl::render
