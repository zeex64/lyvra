#pragma once
// Runs Anime4K CNN upscalers (MIT, bloc97/Anime4K): each mpv pass was converted to a compute
// shader at build time (tools/gen_anime4k.py); this sizes the intermediate textures from the
// passes' WIDTH/HEIGHT expressions and records them in order. Output is 2x the input, RGBA8.
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

class Anime4K {
public:
    Anime4K();
    ~Anime4K();
    bool init(VkPhysicalDevice phys, VkDevice device, std::string& err);
    void shutdown();
    bool available() const { return ready_; }

    static int variantCount();
    static const char* variantName(int v);  // "S", "M", "L"

    // Allocates textures for this variant and input size. Call outside a command buffer.
    bool ensure(int variant, uint32_t inW, uint32_t inH, std::string& err);
    // Leaves the output in SHADER_READ_ONLY_OPTIMAL. Slot 0 is the real frame, slot 1 a generated
    // frame (separate outputs so upscaling a generated frame doesn't overwrite the real one).
    void record(VkCommandBuffer cmd, VkImageView src, VkImageLayout srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, int slot = 0);

    VkImage outputImage(int slot = 0) const;
    VkImageView outputView(int slot = 0) const;
    VkImageCreateInfo outputInfo(int slot = 0) const;
    uint32_t outW() const;
    uint32_t outH() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool ready_ = false;
};
