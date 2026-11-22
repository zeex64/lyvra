#pragma once
// Frame generation for a capture feed with FidelityFX frame interpolation (MIT).
// Runs at capture resolution, before upscaling: optical flow -> motion vectors -> FI prepare ->
// FI dispatch, producing a frame halfway between the previous and the current real frame.
// Predict mode stops after the motion vectors and leaves a motion field the renderer uses to
// warp the real frame forward (no waiting for the next frame, so no added latency).
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

struct FrameGenParams {
    VkImage color = VK_NULL_HANDLE;       // current real frame (capture resolution)
    VkImageCreateInfo colorInfo{};
    VkImageView colorView = VK_NULL_HANDLE;
    bool colorInGeneral = false;          // GENERAL (true) or SHADER_READ_ONLY_OPTIMAL (false)
    bool lockStatic = true;               // keep unchanged pixels (HUDs) from being dragged
    bool debugLock = false;               // tint locked pixels blue
    float frameTimeMs = 16.67f;
    bool reset = false;
    bool predict = false;                 // only compute the motion field for predictMotionView()
};

class FrameGen {
public:
    FrameGen();
    ~FrameGen();
    bool init(VkPhysicalDevice phys, VkDevice device, std::string& err);
    void shutdown();
    bool available() const { return ready_; }

    // (Re)creates contexts when the size or flow scale changes. Call outside a command buffer.
    // flowPercent: optical flow resolution in % of the frame (0 = auto: 100%, or 50% above 2560 px wide).
    bool ensure(uint32_t w, uint32_t h, int flowPercent, std::string& err);
    // Records the generated frame; output is left in GENERAL, visible to fragment shaders.
    bool record(VkCommandBuffer cmd, const FrameGenParams& p);
    VkImageView outputView() const;
    // Predict mode: per-pixel motion (frame px, current -> previous), readable by compute shaders.
    VkImageView predictMotionView() const;
    uint32_t flowWidth() const;
    uint32_t flowHeight() const;
    uint32_t width() const;
    uint32_t height() const;

    std::string lastError;
    uint64_t skippedFrames = 0;    // generated frames replaced by the real one (fast motion)
    float lastFailedFraction = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool ready_ = false;
};
