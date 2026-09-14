#include "render/renderer.h"

#include <array>
#include <cstring>

#include "core/png.h"
#include "shaders/cube_frag.h"
#include "shaders/cube_vert.h"
#include "shaders/quad_frag.h"
#include "shaders/quad_vert.h"

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
};

namespace {
struct CubeVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};
constexpr size_t kInitialCubes = 8192;
constexpr size_t kInitialQuads = 4096;
}  // namespace

Renderer::Renderer(VkContext& ctx) : ctx_(ctx) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;   // chunky Doom pixels
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    vkCheck(vkCreateSampler(ctx_.device(), &sci, nullptr, &sampler_), "vkCreateSampler");

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

    createDescriptors();
    createPipelines();
    createGeometry();
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
    }
    ctx_.destroyTexture(atlas_);
    if (cubePipe_) vkDestroyPipeline(d, cubePipe_, nullptr);
    if (quadPipe_) vkDestroyPipeline(d, quadPipe_, nullptr);
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
        cubeInst_[frame] = ctx_.createBuffer(cap * sizeof(CubeInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, flags, true);
        cubeCap_[frame] = cap;
    }
    if (quads > quadCap_[frame]) {
        size_t cap = std::max(quads, quadCap_[frame] * 2);
        if (quadInst_[frame].buffer) { ctx_.waitIdle(); ctx_.destroyBuffer(quadInst_[frame]); }
        quadInst_[frame] = ctx_.createBuffer(cap * sizeof(QuadInstance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, flags, true);
        quadCap_[frame] = cap;
    }
}

void Renderer::createDescriptors() {
    VkDevice d = ctx_.device();
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2;
    lci.pBindings = bindings;
    vkCheck(vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kFramesInFlight;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    vkCheck(vkCreateDescriptorPool(d, &pci, nullptr, &pool_), "vkCreateDescriptorPool");

    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (auto& l : layouts) l = setLayout_;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(d, &ai, sets_), "vkAllocateDescriptorSets");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    vkCheck(vkCreatePipelineLayout(d, &plci, nullptr, &pipeLayout_), "vkCreatePipelineLayout");
}

void Renderer::setAtlas(const Image& img) {
    ctx_.waitIdle();
    ctx_.destroyTexture(atlas_);
    atlas_ = ctx_.createTexture2D(static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    ctx_.uploadTexture(atlas_, img.rgba.data(), img.rgba.size());
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo bi{ubo_[i].buffer, 0, sizeof(Ubo)};
        VkDescriptorImageInfo ii{sampler_, atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
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
        vkUpdateDescriptorSets(ctx_.device(), 2, w, 0, nullptr);
    }
}

void Renderer::createPipelines() {
    VkDevice d = ctx_.device();
    VkShaderModule cubeVS = ctx_.createShader(shaders::cube_vert, shaders::cube_vert_size);
    VkShaderModule cubeFS = ctx_.createShader(shaders::cube_frag, shaders::cube_frag_size);
    VkShaderModule quadVS = ctx_.createShader(shaders::quad_vert, shaders::quad_vert_size);
    VkShaderModule quadFS = ctx_.createShader(shaders::quad_frag, shaders::quad_frag_size);

    VkFormat colorFmt = ctx_.swapchainFormat();
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &colorFmt;
    rci.depthAttachmentFormat = ctx_.depthFormat();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    auto makePipeline = [&](VkShaderModule vs, VkShaderModule fs, const VkPipelineVertexInputStateCreateInfo& vi, bool cull, bool blend) {
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
        ci.pNext = &rci;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &dss;
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
        VkVertexInputAttributeDescription a[9] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CubeVertex, uv)},
            {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, posScale)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, color)},
            {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, emissive)},
            {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, uvRect)},
            {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, params)},
            {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, rot)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = b;
        vi.vertexAttributeDescriptionCount = 9;
        vi.pVertexAttributeDescriptions = a;
        cubePipe_ = makePipeline(cubeVS, cubeFS, vi, true, false);
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
    }
    vkDestroyShaderModule(d, cubeVS, nullptr);
    vkDestroyShaderModule(d, cubeFS, nullptr);
    vkDestroyShaderModule(d, quadVS, nullptr);
    vkDestroyShaderModule(d, quadFS, nullptr);
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
            verts.push_back({p, f.n, uvs[i]});
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    cubeIndexCount_ = static_cast<uint32_t>(idx.size());
    cubeVB_ = ctx_.createBuffer(verts.size() * sizeof(CubeVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    cubeIB_ = ctx_.createBuffer(idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    ctx_.uploadBuffer(cubeVB_, verts.data(), verts.size() * sizeof(CubeVertex));
    ctx_.uploadBuffer(cubeIB_, idx.data(), idx.size() * sizeof(uint32_t));
}

bool Renderer::render(const FrameParams& params, std::span<const CubeInstance> cubes,
                      std::span<const QuadInstance> worldQuads, std::span<const QuadInstance> screenQuads) {
    VkContext::FrameCtx frame;
    if (!ctx_.beginFrame(frame)) return false;
    const uint32_t fi = frame.frameIndex;
    const VkExtent2D ext = ctx_.extent();

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
    u.counts = glm::ivec4(nl, 0, 0, 0);
    std::memcpy(ubo_[fi].mapped, &u, sizeof(u));

    // Instances (world + screen quads share one buffer, drawn as two ranges).
    size_t quadTotal = worldQuads.size() + screenQuads.size();
    ensureInstanceCapacity(fi, cubes.size(), quadTotal);
    if (!cubes.empty()) std::memcpy(cubeInst_[fi].mapped, cubes.data(), cubes.size() * sizeof(CubeInstance));
    auto* qdst = static_cast<QuadInstance*>(quadInst_[fi].mapped);
    if (!worldQuads.empty()) std::memcpy(qdst, worldQuads.data(), worldQuads.size() * sizeof(QuadInstance));
    if (!screenQuads.empty()) std::memcpy(qdst + worldQuads.size(), screenQuads.data(), screenQuads.size() * sizeof(QuadInstance));

    VkCommandBuffer cmd = frame.cmd;
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = frame.imageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{params.clearColor.r, params.clearColor.g, params.clearColor.b, 1.f}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = frame.depthView;
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &sets_[fi], 0, nullptr);

    if (!cubes.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cubePipe_);
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        VkBuffer vbs[2] = {cubeVB_.buffer, cubeInst_[fi].buffer};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, cubeIB_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, cubeIndexCount_, static_cast<uint32_t>(cubes.size()), 0, 0, 0);
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
        if (!screenQuads.empty()) {
            vkCmdSetDepthTestEnable(cmd, VK_FALSE);
            vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
            vkCmdDraw(cmd, 6, static_cast<uint32_t>(screenQuads.size()), 0, static_cast<uint32_t>(worldQuads.size()));
        }
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
