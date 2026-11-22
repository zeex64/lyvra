#include "anime4k.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

#include "anime4k_passes.h"  // generated

namespace {
struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Pipe {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};
struct Size { float w = 0, h = 0; };
constexpr int kVariants = sizeof(kAnime4KVariants) / sizeof(kAnime4KVariants[0]);
}  // namespace

struct Anime4K::Impl {
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR push = nullptr;
    std::vector<std::vector<Pipe>> pipes;  // [variant][pass]

    int variant = -1;
    uint32_t inW = 0, inH = 0;
    std::map<std::string, Img> textures;  // intermediate textures by mpv name
    Img output[2];                        // final RGBA8 (the pass that saves MAIN): real, generated
    std::vector<Size> passSize;           // output size of each pass of the current variant

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return 0;
    }
    bool create(Img& img, uint32_t w, uint32_t h, VkFormat fmt) {
        destroy(img);
        img.info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img.info.imageType = VK_IMAGE_TYPE_2D;
        img.info.format = fmt;
        img.info.extent = {w, h, 1};
        img.info.mipLevels = img.info.arrayLayers = 1;
        img.info.samples = VK_SAMPLE_COUNT_1_BIT;
        img.info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img.info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(dev, &img.info, nullptr, &img.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &ai, nullptr, &img.mem) != VK_SUCCESS) return false;
        vkBindImageMemory(dev, img.image, img.mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev, &vi, nullptr, &img.view) != VK_SUCCESS) return false;
        img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }
    void destroy(Img& img) {
        if (img.view) vkDestroyImageView(dev, img.view, nullptr);
        if (img.image) vkDestroyImage(dev, img.image, nullptr);
        if (img.mem) vkFreeMemory(dev, img.mem, nullptr);
        img = {};
    }
    void releaseTextures() {
        for (auto& [n, t] : textures) destroy(t);
        textures.clear();
        destroy(output[0]);
        destroy(output[1]);
        variant = -1;
        inW = inH = 0;
    }
    void transition(VkCommandBuffer cmd, Img& img, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout = to == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_UNDEFINED : img.layout;  // outputs are fully overwritten
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        img.layout = to;
    }

    // mpv RPN size expression, e.g. "conv2d_last_tf.w 2 *".
    static float evalRpn(const char* expr, const std::map<std::string, Size>& sizes) {
        std::vector<float> st;
        std::istringstream in(expr);
        std::string tok;
        while (in >> tok) {
            if (tok == "+" || tok == "-" || tok == "*" || tok == "/" || tok == ">" || tok == "<") {
                if (st.size() < 2) return 0;
                float b = st.back(); st.pop_back();
                float a = st.back(); st.pop_back();
                st.push_back(tok == "+" ? a + b : tok == "-" ? a - b : tok == "*" ? a * b : tok == "/" ? (b != 0 ? a / b : 0) : tok == ">" ? float(a > b) : float(a < b));
            } else if (tok.size() > 2 && tok[tok.size() - 2] == '.') {
                auto it = sizes.find(tok.substr(0, tok.size() - 2));
                st.push_back(it == sizes.end() ? 0 : (tok.back() == 'w' ? it->second.w : it->second.h));
            } else {
                st.push_back(std::strtof(tok.c_str(), nullptr));
            }
        }
        return st.empty() ? 0 : st.back();
    }
};

Anime4K::Anime4K() : d_(std::make_unique<Impl>()) {}
Anime4K::~Anime4K() { shutdown(); }

int Anime4K::variantCount() { return kVariants; }
const char* Anime4K::variantName(int v) { return (v >= 0 && v < kVariants) ? kAnime4KVariants[v].name : "?"; }

bool Anime4K::init(VkPhysicalDevice phys, VkDevice device, std::string& err) {
    Impl& d = *d_;
    d.phys = phys;
    d.dev = device;
    d.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;  // mpv samples bound textures with linear filtering
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sci, nullptr, &d.linear);

    d.pipes.resize(kVariants);
    for (int v = 0; v < kVariants; v++) {
        const Anime4KVariant& var = kAnime4KVariants[v];
        for (uint32_t i = 0; i < var.count; i++) {
            const Anime4KPassDesc& p = var.passes[i];
            Pipe pp;
            std::vector<VkDescriptorSetLayoutBinding> b;
            for (uint32_t k = 0; k < p.numBinds; k++) b.push_back({k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            b.push_back({p.numBinds, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            lci.bindingCount = uint32_t(b.size());
            lci.pBindings = b.data();
            vkCreateDescriptorSetLayout(device, &lci, nullptr, &pp.set);
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 + 8 * 8};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &pp.set;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pcr;
            vkCreatePipelineLayout(device, &pli, nullptr, &pp.layout);
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = p.spvBytes;
            smi.pCode = p.spv;
            VkShaderModule sm;
            if (vkCreateShaderModule(device, &smi, nullptr, &sm) != VK_SUCCESS) { err = "Anime4K shader module failed"; return false; }
            VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
            cpi.layout = pp.layout;
            VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pp.pipe);
            vkDestroyShaderModule(device, sm, nullptr);
            if (r != VK_SUCCESS) { err = "Anime4K pipeline creation failed"; return false; }
            d.pipes[v].push_back(pp);
        }
    }
    ready_ = true;
    return true;
}

void Anime4K::shutdown() {
    Impl& d = *d_;
    if (!d.dev) return;
    vkDeviceWaitIdle(d.dev);
    d.releaseTextures();
    for (auto& v : d.pipes)
        for (auto& p : v) {
            vkDestroyPipeline(d.dev, p.pipe, nullptr);
            vkDestroyPipelineLayout(d.dev, p.layout, nullptr);
            vkDestroyDescriptorSetLayout(d.dev, p.set, nullptr);
        }
    d.pipes.clear();
    if (d.linear) vkDestroySampler(d.dev, d.linear, nullptr);
    d.dev = VK_NULL_HANDLE;
    ready_ = false;
}

bool Anime4K::ensure(int variant, uint32_t inW, uint32_t inH, std::string& err) {
    Impl& d = *d_;
    if (!ready_) { err = "Anime4K not initialised"; return false; }
    variant = variant < 0 || variant >= kVariants ? 0 : variant;
    if (d.variant == variant && d.inW == inW && d.inH == inH) return true;
    vkDeviceWaitIdle(d.dev);
    d.releaseTextures();

    const Anime4KVariant& var = kAnime4KVariants[variant];
    std::map<std::string, Size> sizes{{"MAIN", {float(inW), float(inH)}}};
    d.passSize.clear();
    for (uint32_t i = 0; i < var.count; i++) {
        const Anime4KPassDesc& p = var.passes[i];
        Size s{std::round(Impl::evalRpn(p.width, sizes)), std::round(Impl::evalRpn(p.height, sizes))};
        if (s.w < 1 || s.h < 1 || s.w > 16384 || s.h > 16384) { err = "Anime4K: bad size for pass " + std::to_string(i); return false; }
        d.passSize.push_back(s);
        if (p.final) {
            for (Img& o : d.output)
                if (!d.create(o, uint32_t(s.w), uint32_t(s.h), VK_FORMAT_R8G8B8A8_UNORM)) { err = "Anime4K allocation failed"; return false; }
        } else if (!d.textures.count(p.save)) {
            if (!d.create(d.textures[p.save], uint32_t(s.w), uint32_t(s.h), VK_FORMAT_R16G16B16A16_SFLOAT)) { err = "Anime4K allocation failed"; return false; }
        }
        sizes[p.save] = s;  // later passes see the new size (including the final MAIN)
    }
    d.variant = variant;
    d.inW = inW;
    d.inH = inH;
    return true;
}

void Anime4K::record(VkCommandBuffer cmd, VkImageView src, VkImageLayout srcLayout, int slot) {
    Impl& d = *d_;
    if (d.variant < 0) return;
    const Anime4KVariant& var = kAnime4KVariants[d.variant];
    std::map<std::string, Size> sizes{{"MAIN", {float(d.inW), float(d.inH)}}};
    for (uint32_t i = 0; i < var.count; i++) {
        const Anime4KPassDesc& p = var.passes[i];
        Img& out = p.final ? d.output[slot ? 1 : 0] : d.textures[p.save];
        d.transition(cmd, out, VK_IMAGE_LAYOUT_GENERAL);
        const Pipe& pp = d.pipes[d.variant][i];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp.pipe);

        std::vector<VkDescriptorImageInfo> infos(p.numBinds + 1);
        std::vector<VkWriteDescriptorSet> w(p.numBinds + 1, VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        struct { float outW, outH; float sizes[16]; } pc{d.passSize[i].w, d.passSize[i].h, {}};
        for (uint32_t k = 0; k < p.numBinds; k++) {
            std::string name = p.binds[k];
            VkImageView view = name == "MAIN" ? src : d.textures[name].view;
            infos[k] = {d.linear, view, name == "MAIN" ? srcLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &infos[k];
            pc.sizes[k * 2] = sizes[name].w;
            pc.sizes[k * 2 + 1] = sizes[name].h;
        }
        infos[p.numBinds] = {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL};
        w[p.numBinds].dstBinding = p.numBinds;
        w[p.numBinds].descriptorCount = 1;
        w[p.numBinds].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[p.numBinds].pImageInfo = &infos[p.numBinds];
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp.layout, 0, uint32_t(w.size()), w.data());
        vkCmdPushConstants(cmd, pp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 + 8 * 8, &pc);
        vkCmdDispatch(cmd, (uint32_t(d.passSize[i].w) + 7) / 8, (uint32_t(d.passSize[i].h) + 7) / 8, 1);
        d.transition(cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sizes[p.save] = d.passSize[i];
    }
}

VkImage Anime4K::outputImage(int slot) const { return d_->output[slot ? 1 : 0].image; }
VkImageView Anime4K::outputView(int slot) const { return d_->output[slot ? 1 : 0].view; }
VkImageCreateInfo Anime4K::outputInfo(int slot) const { return d_->output[slot ? 1 : 0].info; }
uint32_t Anime4K::outW() const { return d_->output[0].info.extent.width; }
uint32_t Anime4K::outH() const { return d_->output[0].info.extent.height; }
