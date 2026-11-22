#pragma once
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "settings.h"
#include "temporal.h"
#include "framegen.h"

struct ImDrawData;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

struct FrameInput {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t width = 0, height = 0, fourcc = 0, stride = 0;
    bool bt601 = false, fullRange = false;
};

struct RenderParams {
    Rect videoArea;  // in pixels
    ScaleMode scale = ScaleMode::Fit;
    Upscaler upscaler = Upscaler::Off;
    float sharpness = 0.25f;
    float frameTimeMs = 8.33f;  // capture frame interval, for FSR 3
    bool debugMotion = false;   // show the optical-flow motion field instead of the video
    bool compare = false;       // split view: FSR 1 left, FSR 3 right (same frame)
    bool jitter = true;         // synthetic sub-pixel jitter for FSR 3
    bool frameGen = false;      // on a new frame, present a generated in-between frame instead
    bool fgLock = true;         // static-pixel lock for frame generation
    int fsrQuality = 0;         // FSR 3/4 output: 0 Auto (window), 1 Native AA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance
};

struct GpuTimes { float convert = 0, upscale = 0, present = 0, total = 0; bool valid = false; };

class Renderer {
public:
    bool init(SDL_Window* window, PresentMode pm, std::string& err);
    void initImGui();
    void shutdown();

    void setPresentMode(PresentMode pm);
    void onResize() { swapchainDirty_ = true; }
    void clearInput() { inputValid_ = false; }
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; }
    std::string savedScreenshot;  // set after a screenshot is written; caller clears it

    // Uploads `in` (if non-null), converts, upscales and presents with the UI on top.
    bool render(const FrameInput* in, const RenderParams& p, ImDrawData* dd);

    static Rect fitRect(Rect area, uint32_t srcW, uint32_t srcH, ScaleMode m);
    const char* presentModeName() const;

    // Info for the UI
    std::string gpuName;
    Upscaler effectiveUpscaler = Upscaler::Off;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    uint32_t swapW = 0, swapH = 0;
    GpuTimes gpu;
    uint64_t lastRenderStartNs = 0, lastSubmitNs = 0, lastPresentReturnNs = 0;
    bool hasInput() const { return inputValid_; }
    bool temporalAvailable() const { return temporal_.available(); }
    bool fsr4Available() const { return temporal_.fsr4Available(); }
    const std::string& fsr4Status() const { return temporal_.fsr4Status; }
    std::string temporalStatus;  // why FSR 3 is unavailable or fell back, empty when fine
    bool showingMotion = false;
    bool comparing = false;  // split view active this frame
    bool frameGenAvailable() const { return fg_.available(); }
    uint64_t frameGenSkipped() const { return fg_.skippedFrames; }
    float frameGenFailedFraction() const { return fg_.lastFailedFraction; }
    bool generatedShown = false;  // this render presented a generated frame; the real one is still to come
    std::string frameGenStatus;
    Rect lastDst;            // where the video was drawn, in pixels

private:
    struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; uint32_t w = 0, h = 0; VkImageCreateInfo info{}; };
    struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; VkDeviceSize size = 0; };
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        Buffer staging;
        bool queried = false;
    };
    static constexpr int kFrames = 2;

    bool createSwapchain();
    void destroySwapchainViews();
    bool createPipelines();
    VkShaderModule shader(const uint32_t* code, size_t bytes);
    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback);
    bool createImage(Image& img, uint32_t w, uint32_t h);
    void destroyImage(Image& img);
    bool createBuffer(Buffer& b, VkDeviceSize size);
    void destroyBuffer(Buffer& b);
    void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    void pushImageSampler(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout,
                          VkImageView sampled, VkSampler sampler, VkImageView storage,
                          VkImageLayout sampledLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t qfam_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    float tsPeriod_ = 1.0f;
    bool tsSupported_ = false;
    PFN_vkCmdPushDescriptorSetKHR pushDesc_ = nullptr;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_;
    uint32_t minImageCount_ = 2;
    PresentMode presentMode_ = PresentMode::Mailbox;
    VkPresentModeKHR vkPresentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool swapchainDirty_ = false;

    Frame frames_[kFrames];
    int cur_ = 0;

    Image src_, easu_, rcas_, easuG_, rcasG_;  // *G_: FSR 1 on generated frames
    uint32_t inFourcc_ = 0;
    size_t stagingSize_ = 0;
    bool inputValid_ = false, upscaleDirty_ = true;
    float lastSharpness_ = -1;

    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout yuvSetLayout_ = VK_NULL_HANDLE, fsrSetLayout_ = VK_NULL_HANDLE, blitSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout yuvLayout_ = VK_NULL_HANDLE, fsrLayout_ = VK_NULL_HANDLE, blitLayout_ = VK_NULL_HANDLE;
    VkPipeline yuvPipe_ = VK_NULL_HANDLE, easuPipe_ = VK_NULL_HANDLE, rcasPipe_ = VK_NULL_HANDLE, blitPipe_ = VK_NULL_HANDLE;

    bool imguiReady_ = false;
    Temporal temporal_;
    FrameGen fg_;
    bool temporalInputFresh_ = false;
    std::string screenshotPath_;
    Buffer readback_;
    bool writeScreenshot();
};
