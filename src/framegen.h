#pragma once
// Frame generation for a capture feed with FidelityFX frame interpolation (MIT).
// Runs at display resolution on the final (upscaled) frame: optical flow -> motion
// vectors -> FI prepare -> FI dispatch, producing a frame halfway between the previous
// and the current real frame.
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

struct FrameGenParams {
    VkImage color = VK_NULL_HANDLE;       // final frame (display resolution)
    VkImageCreateInfo colorInfo{};
    VkImageView colorView = VK_NULL_HANDLE;
    bool colorInGeneral = false;          // GENERAL (true) or SHADER_READ_ONLY_OPTIMAL (false)
    bool lockStatic = true;               // keep unchanged pixels (HUDs) from being dragged
    bool debugLock = false;               // tint locked pixels blue
    float frameTimeMs = 16.67f;
    bool reset = false;
};

class FrameGen {
public:
    FrameGen();
    ~FrameGen();
    bool init(VkPhysicalDevice phys, VkDevice device, std::string& err);
    void shutdown();
    bool available() const { return ready_; }

    // (Re)creates contexts when the display size changes. Call outside a command buffer.
    bool ensure(uint32_t w, uint32_t h, std::string& err);
    // Records the generated frame; output is left in GENERAL, visible to fragment shaders.
    bool record(VkCommandBuffer cmd, const FrameGenParams& p);
    VkImageView outputView() const;
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
