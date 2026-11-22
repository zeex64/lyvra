#pragma once
// FSR 3.1 temporal upscaling for a capture feed, using the FidelityFX SDK (MIT).
// A capture feed has no engine motion vectors, so they come from FidelityFX optical flow.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

struct TemporalParams {
    VkImage colorImage;      // RGBA8, render resolution, in SHADER_READ_ONLY_OPTIMAL
    VkImageCreateInfo colorInfo;
    VkImageView colorView = VK_NULL_HANDLE;
    bool jitter = true;      // synthetic sub-pixel jitter (see jitter.comp)
    bool fsr4 = false;       // use FSR 4 (fsr4-vk) instead of FSR 3.1 for the final upscale
    float frameTimeMs = 8.33f;
    float sharpness = 0.25f;  // 0..1
    bool reset = false;
    bool debugMotion = false;
};

class Temporal {
public:
    Temporal();
    ~Temporal();
    bool init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, std::string& err);
    void shutdown();
    bool available() const { return ready_; }

    // (Re)creates contexts/resources when sizes change. Call outside a command buffer (may wait idle).
    bool ensure(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, bool fsr4, std::string& err);
    bool fsr4Available() const;      // fsr4-vk library found and loaded
    std::string fsr4Status;          // where it was loaded from, or why not

    // Records optical flow -> motion vectors -> FSR 3.1 into cmd. Output is left in GENERAL layout,
    // visible to fragment shaders.
    bool record(VkCommandBuffer cmd, const TemporalParams& p);

    VkImageView outputView() const;     // upscaled frame (out size)
    VkImage outputImage() const;
    VkImageCreateInfo outputInfo() const;
    VkImageView debugView() const;      // motion visualisation (in size)
    uint32_t outW() const;
    uint32_t outH() const;

    std::string lastError;
    float lastSceneChange = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool ready_ = false;
};
