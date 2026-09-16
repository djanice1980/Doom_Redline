#include "render/vk_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rl::render {

const char* vkResultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "VkResult(other)";
    }
}

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: " + vkResultName(r));
    }
}

// ---------------------------------------------------------------------------
VkContext::VkContext(SDL_Window* window, bool preferIntegrated) : window_(window) {
    createInstance();
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    }
    pickDevice(preferIntegrated);
    createDevice();
    createSwapchain();
    createSync();
}

VkContext::~VkContext() {
    if (device_) vkDeviceWaitIdle(device_);
    destroySwapchain();
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(device_, inFlight_[i], nullptr);
    }
    if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VkContext::createInstance() {
    Uint32 count = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<const char*> exts(sdlExts, sdlExts + count);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "REDLINE";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "redline";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    if (std::getenv("REDLINE_VALIDATION")) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) std::fprintf(stderr, "[vk] validation requested but VK_LAYER_KHRONOS_validation is not installed\n");
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    vkCheck(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");
}

void VkContext::pickDevice(bool preferIntegrated) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) throw std::runtime_error("no Vulkan devices");
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());

    int forced = -1;
    if (const char* e = std::getenv("REDLINE_GPU")) forced = std::atoi(e);

    int bestScore = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        // Need a queue with graphics + present.
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qp.data());
        int family = -1;
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface_, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = static_cast<int>(q); break; }
        }
        if (family < 0) continue;
        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += preferIntegrated ? 10 : 100;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += preferIntegrated ? 100 : 10;
        if (forced == static_cast<int>(i)) score += 10000;
        std::fprintf(stderr, "[vk] GPU %u: %s (score %d)\n", i, props.deviceName, score);
        if (score > bestScore) {
            bestScore = score;
            physical_ = devs[i];
            queueFamily_ = static_cast<uint32_t>(family);
            gpuName_ = props.deviceName;
        }
    }
    if (!physical_) throw std::runtime_error("no suitable Vulkan 1.3 device with present support");
    vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);
    {
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(physical_, &f);
        bc_ = f.textureCompressionBC == VK_TRUE;
    }

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ep(en);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &en, ep.data());
    bool hasRq = false, hasAs = false, hasDho = false;
    for (auto& e : ep) {
        if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) hasRq = true;
        if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasAs = true;
        if (std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) hasDho = true;
    }
    rayQuery_ = hasRq && hasAs && hasDho;
    if (std::getenv("REDLINE_NO_RT")) rayQuery_ = false;   // force the shadow-map path for comparison
    if (rayQuery_) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &asp;
        vkGetPhysicalDeviceProperties2(physical_, &p2);
        scratchAlignment_ = std::max<VkDeviceSize>(asp.minAccelerationStructureScratchOffsetAlignment, 256);
    }
    std::fprintf(stderr, "[vk] using %s (ray query %s)\n", gpuName_.c_str(), rayQuery_ ? "available" : "not available");
}

void VkContext::createDevice() {
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = rayQuery_ ? VK_TRUE : VK_FALSE;
    f12.pNext = &f13;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    asf.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rqf.rayQuery = VK_TRUE;
    asf.pNext = &rqf;
    rqf.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = rayQuery_ ? static_cast<void*>(&asf) : static_cast<void*>(&f12);
    f2.features.samplerAnisotropy = VK_FALSE;
    f2.features.textureCompressionBC = bc_ ? VK_TRUE : VK_FALSE;

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (rayQuery_) {
        exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    vkCheck(vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (rayQuery_) {
        rt_.getBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        rt_.create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        rt_.destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        rt_.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        rt_.getAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        if (!rt_.getBuildSizes || !rt_.create || !rt_.destroy || !rt_.cmdBuild || !rt_.getAddress) {
            std::fprintf(stderr, "[vk] ray tracing entry points missing; shadow maps only\n");
            rayQuery_ = false;
        }
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    vkCheck(vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFramesInFlight;
    vkCheck(vkAllocateCommandBuffers(device_, &cai, cmds_), "vkAllocateCommandBuffers");
}

void VkContext::createSync() {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vkCheck(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]), "vkCreateSemaphore");
        vkCheck(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]), "vkCreateFence");
    }
}

// ---------------------------------------------------------------------------
void VkContext::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface caps");

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    swapFormat_ = chosen.format;

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::getenv("REDLINE_NOVSYNC"))
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR || m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; break; }

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) { extent_ = {1, 1}; }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());
    swapViews_.resize(n);
    renderDone_.resize(n);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = swapImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device_, &vci, nullptr, &swapViews_[i]), "swapchain image view");
        vkCheck(vkCreateSemaphore(device_, &sci, nullptr, &renderDone_[i]), "vkCreateSemaphore");
    }
    createDepth();
    std::fprintf(stderr, "[vk] swapchain %ux%u, %u images, present mode %d\n", extent_.width, extent_.height, n, static_cast<int>(mode));
}

void VkContext::createDepth() {
    // Prefer D32; fall back to D24S8.
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D32_SFLOAT, &fp);
    depthFormat_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D24_UNORM_S8_UINT;
    depth_ = createTexture2D(extent_.width, extent_.height, depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VkContext::destroySwapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroyTexture(depth_);
    for (auto v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    swapViews_.clear();
    renderDone_.clear();
    swapImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VkContext::waitIdle() { vkDeviceWaitIdle(device_); }

// ---------------------------------------------------------------------------
bool VkContext::beginFrame(FrameCtx& out) {
    if (resizePending_) {
        resizePending_ = false;
        destroySwapchain();
        createSwapchain();
        return false;
    }
    uint32_t fi = frameIndex_;
    vkWaitForFences(device_, 1, &inFlight_[fi], VK_TRUE, UINT64_MAX);
    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { resizePending_ = true; return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vkCheck(r, "vkAcquireNextImageKHR");
    vkResetFences(device_, 1, &inFlight_[fi]);

    VkCommandBuffer cmd = cmds_[fi];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    // Swapchain image: UNDEFINED -> COLOR_ATTACHMENT; depth: UNDEFINED -> DEPTH_ATTACHMENT.
    VkImageMemoryBarrier2 barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].image = swapImages_[imageIndex];
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[1].image = depth_.image;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    out.cmd = cmd;
    out.imageIndex = imageIndex;
    out.frameIndex = fi;
    out.image = swapImages_[imageIndex];
    out.imageView = swapViews_[imageIndex];
    out.depthView = depth_.view;
    return true;
}

void VkContext::endFrame(const FrameCtx& frame) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.image = frame.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(frame.cmd, &dep);
    vkCheck(vkEndCommandBuffer(frame.cmd), "vkEndCommandBuffer");

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = imageAvailable_[frame.frameIndex];
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = renderDone_[frame.imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = frame.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    vkCheck(vkQueueSubmit2(queue_, 1, &si, inFlight_[frame.frameIndex]), "vkQueueSubmit2");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[frame.imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &frame.imageIndex;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resizePending_ = true;
    else if (r != VK_SUCCESS) vkCheck(r, "vkQueuePresentKHR");
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

// ---------------------------------------------------------------------------
uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("no suitable memory type");
}

Buffer VkContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map, bool deviceAddress) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage | (deviceAddress ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0u);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device_, &ci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = deviceAddress ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    vkCheck(vkAllocateMemory(device_, &ai, nullptr, &b.memory), "vkAllocateMemory(buffer)");
    vkCheck(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory");
    if (map) vkCheck(vkMapMemory(device_, b.memory, 0, size, 0, &b.mapped), "vkMapMemory");
    return b;
}

VkDeviceAddress VkContext::bufferAddress(const Buffer& b) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b.buffer;
    return vkGetBufferDeviceAddress(device_, &info);
}

void VkContext::destroyBuffer(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

Texture VkContext::createTexture2D(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkMemoryPropertyFlags props, uint32_t mipLevels) {
    Texture t;
    t.width = w;
    t.height = h;
    t.format = fmt;
    t.mipLevels = mipLevels;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device_, &ci, nullptr, &t.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    vkCheck(vkAllocateMemory(device_, &ai, nullptr, &t.memory), "vkAllocateMemory(image)");
    vkCheck(vkBindImageMemory(device_, t.image, t.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    vkCheck(vkCreateImageView(device_, &vci, nullptr, &t.view), "vkCreateImageView");
    return t;
}

void VkContext::uploadTextureLevels(Texture& t, const std::vector<std::vector<uint8_t>>& levels) {
    size_t total = 0;
    for (const auto& l : levels) total += l.size();
    Buffer staging = createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::vector<VkBufferImageCopy> regions;
    size_t off = 0;
    for (uint32_t i = 0; i < levels.size() && i < t.mipLevels; ++i) {
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + off, levels[i].data(), levels[i].size());
        VkBufferImageCopy r{};
        r.bufferOffset = off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        r.imageExtent = {std::max(1u, t.width >> i), std::max(1u, t.height >> i), 1};
        regions.push_back(r);
        off += levels[i].size();
    }
    oneTimeSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = t.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mipLevels, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    destroyBuffer(staging);
}

void VkContext::destroyTexture(Texture& t) {
    if (t.view) vkDestroyImageView(device_, t.view, nullptr);
    if (t.image) vkDestroyImage(device_, t.image, nullptr);
    if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
    t = {};
}

void VkContext::oneTimeSubmit(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmdPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkCheck(vkAllocateCommandBuffers(device_, &cai, &cmd), "alloc one-time cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkCheck(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), "one-time submit");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
}

void VkContext::uploadBuffer(Buffer& dst, const void* data, size_t bytes) {
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::memcpy(staging.mapped, data, bytes);
    oneTimeSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroyBuffer(staging);
}

void VkContext::uploadTexture(Texture& t, const void* rgba, size_t bytes) {
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::memcpy(staging.mapped, rgba, bytes);
    oneTimeSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = t.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {t.width, t.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    destroyBuffer(staging);
}

VkShaderModule VkContext::createShader(const uint32_t* words, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = words;
    VkShaderModule m;
    vkCheck(vkCreateShaderModule(device_, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

std::vector<uint8_t> VkContext::readbackImage(VkImage image, uint32_t w, uint32_t h, VkFormat fmt) {
    size_t bytes = static_cast<size_t>(w) * h * 4;
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    oneTimeSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    std::vector<uint8_t> out(bytes);
    std::memcpy(out.data(), staging.mapped, bytes);
    destroyBuffer(staging);
    // Convert BGRA -> RGBA if needed.
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB)
        for (size_t i = 0; i < bytes; i += 4) std::swap(out[i], out[i + 2]);
    return out;
}

}  // namespace rl::render
