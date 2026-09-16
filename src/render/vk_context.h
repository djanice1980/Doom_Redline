#pragma once
// Vulkan 1.3 device/swapchain bootstrap. Dynamic rendering, synchronization2,
// two frames in flight. No third-party allocator: a tiny findMemoryType +
// vkAllocateMemory per resource is enough for this project's handful of
// buffers and one atlas texture.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace rl::render {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

constexpr uint32_t kFramesInFlight = 2;

class VkContext {
public:
    VkContext(SDL_Window* window, bool preferIntegrated);
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    VkDevice device() const { return device_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkFormat swapchainFormat() const { return swapFormat_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkExtent2D extent() const { return extent_; }
    const std::string& gpuName() const { return gpuName_; }
    bool rayQuerySupported() const { return rayQuery_; }
    // Ray tracing (VK_KHR_acceleration_structure + ray_query), loaded when supported.
    struct RtFuncs {
        PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
        PFN_vkCreateAccelerationStructureKHR create = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR getAddress = nullptr;
    };
    const RtFuncs& rt() const { return rt_; }
    VkDeviceSize scratchAlignment() const { return scratchAlignment_; }

    // Frame lifecycle -----------------------------------------------------
    struct FrameCtx {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint32_t imageIndex = 0;
        uint32_t frameIndex = 0;   // 0..kFramesInFlight-1
        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
    };
    // Returns false if the swapchain had to be rebuilt (skip this frame).
    bool beginFrame(FrameCtx& out);
    void endFrame(const FrameCtx& frame);
    void waitIdle();
    void requestResize() { resizePending_ = true; }

    // Resource helpers ----------------------------------------------------
    // `deviceAddress` allocates with the device-address flag (acceleration-structure inputs, scratch, AS storage).
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map, bool deviceAddress = false);
    VkDeviceAddress bufferAddress(const Buffer& b) const;
    void destroyBuffer(Buffer& b);
    Texture createTexture2D(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    void destroyTexture(Texture& t);
    void uploadTexture(Texture& t, const void* rgba, size_t bytes);   // -> SHADER_READ_ONLY_OPTIMAL
    void uploadBuffer(Buffer& dst, const void* data, size_t bytes);   // via staging
    void oneTimeSubmit(const std::function<void(VkCommandBuffer)>& fn);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkShaderModule createShader(const uint32_t* words, size_t bytes);

    // Reads back the last presented swapchain image (call after endFrame + waitIdle).
    std::vector<uint8_t> readbackImage(VkImage image, uint32_t w, uint32_t h, VkFormat fmt);

private:
    void createInstance();
    void pickDevice(bool preferIntegrated);
    void createDevice();
    void createSwapchain();
    void destroySwapchain();
    void createDepth();
    void createSync();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    std::string gpuName_;
    bool rayQuery_ = false;
    RtFuncs rt_;
    VkDeviceSize scratchAlignment_ = 256;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkExtent2D extent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_;   // per swapchain image
    Texture depth_;
    bool resizePending_ = false;

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmds_[kFramesInFlight]{};
    VkSemaphore imageAvailable_[kFramesInFlight]{};
    VkFence inFlight_[kFramesInFlight]{};
    uint32_t frameIndex_ = 0;
};

const char* vkResultName(VkResult r);
void vkCheck(VkResult r, const char* what);

}  // namespace rl::render
