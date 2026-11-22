#include "framegen.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static const uint32_t kFgMotionSpv[] =
#include "fgmotion.comp.inc"
    ;
static const uint32_t kFgLockSpv[] =
#include "fglock.comp.inc"
    ;
static const uint32_t kFgPredMvSpv[] =
#include "fgpredmv.comp.inc"
    ;
static const uint32_t kFgCheckSpv[] =
#include "fgcheck.comp.inc"
    ;
static const uint32_t kDownSpv[] =  // jitter.comp with zero offset = 2x box downsample
#include "jitter.comp.inc"
    ;

namespace {
struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
}  // namespace

struct FrameGen::Impl {
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    std::vector<uint8_t> scratch;
    FfxInterface iface{};
    FfxOpticalflowContext of{};
    FfxFrameInterpolationContext fi{};
    bool ofLive = false, fiLive = false;
    uint32_t w = 0, h = 0, blockSize = 8;
    uint32_t ofW = 0, ofH = 0;  // optical flow resolution
    int flowPercent = -1;
    Img small;
    VkPipeline downPipe = VK_NULL_HANDLE;
    VkSampler linearDown = VK_NULL_HANDLE;
    uint64_t frameID = 0;
    Img flow, scd, motion, depth, dilDepth, dilMotion, prevDepth, output, predMv;
    Img mvHist[2];  // predict mode: raw motion of the last frames (ping-pong)
    int histIdx = 0;
    bool haveHist = false;
    VkDescriptorSetLayout pSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pLayout = VK_NULL_HANDLE;
    VkPipeline pPipe = VK_NULL_HANDLE;
    Img prevFinal[2];
    int prevIdx = 0;
    bool havePrev = false;
    VkSampler linearUnused = VK_NULL_HANDLE;
    VkDescriptorSetLayout lSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout lLayout = VK_NULL_HANDLE;
    VkPipeline lPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout cSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout cLayout = VK_NULL_HANDLE;
    VkPipeline cPipe = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    VkBuffer counter = VK_NULL_HANDLE;
    VkDeviceMemory counterMem = VK_NULL_HANDLE;
    uint32_t* counterMap = nullptr;
    bool counterPending = false;

    VkSampler nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR push = nullptr;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return 0;
    }
    bool create(Img& img, uint32_t iw, uint32_t ih, VkFormat fmt, VkImageUsageFlags usage) {
        destroy(img);
        img.info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img.info.imageType = VK_IMAGE_TYPE_2D;
        img.info.format = fmt;
        img.info.extent = {iw, ih, 1};
        img.info.mipLevels = img.info.arrayLayers = 1;
        img.info.samples = VK_SAMPLE_COUNT_1_BIT;
        img.info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img.info.usage = usage;
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
    void destroyContexts() {
        if (fiLive) ffxFrameInterpolationContextDestroy(&fi);
        if (ofLive) ffxOpticalflowContextDestroy(&of);
        fiLive = ofLive = false;
        for (Img* i : {&flow, &scd, &motion, &depth, &dilDepth, &dilMotion, &prevDepth, &output, &prevFinal[0], &prevFinal[1], &small, &predMv, &mvHist[0], &mvHist[1]}) destroy(*i);
        w = h = 0;
        havePrev = false;
    }
    void transition(VkCommandBuffer cmd, Img& img, VkImageLayout to, VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = dstStage;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = img.layout;
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
    FfxResource res(Img& img, const wchar_t* name, FfxResourceStates state, FfxResourceUsage usage) {
        return ffxGetResourceVK((void*)img.image, ffxGetImageResourceDescriptionVK(img.image, img.info, usage), name, state);
    }
};

FrameGen::FrameGen() : d_(std::make_unique<Impl>()) {}
FrameGen::~FrameGen() { shutdown(); }

bool FrameGen::init(VkPhysicalDevice phys, VkDevice device, std::string& err) {
    d_->phys = phys;
    d_->dev = device;
    const size_t maxContexts = 2;  // optical flow + frame interpolation
    d_->scratch.resize(ffxGetScratchMemorySizeVK(phys, maxContexts));
    VkDeviceContext dc{device, phys, vkGetDeviceProcAddr};
    FfxErrorCode e = ffxGetInterfaceVK(&d_->iface, ffxGetDeviceVK(&dc), d_->scratch.data(), d_->scratch.size(), maxContexts);
    if (e != FFX_OK) { err = "FidelityFX backend init failed (" + std::to_string(e) + ")"; return false; }

    d_->push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sci, nullptr, &d_->nearest);
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = 2;
    lci.pBindings = b;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->setLayout);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d_->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->layout);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof kFgMotionSpv;
    smi.pCode = kFgMotionSpv;
    VkShaderModule sm;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpi.layout = d_->layout;
    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->pipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "frame generation pipeline creation failed"; return false; }
    smi.codeSize = sizeof kDownSpv;
    smi.pCode = kDownSpv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->downPipe);  // same layout as fgmotion
    vkDestroyShaderModule(device, sm, nullptr);
    {
        VkSamplerCreateInfo ls = sci;
        ls.magFilter = ls.minFilter = VK_FILTER_LINEAR;
        vkCreateSampler(device, &ls, nullptr, &d_->linearDown);
    }

    VkDescriptorSetLayoutBinding lb[5] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    lci.bindingCount = 5;
    lci.pBindings = lb;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->lSetLayout);
    VkPushConstantRange lpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    pli.pSetLayouts = &d_->lSetLayout;
    pli.pPushConstantRanges = &lpcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->lLayout);
    smi.codeSize = sizeof kFgLockSpv;
    smi.pCode = kFgLockSpv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    cpi.layout = d_->lLayout;
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->lPipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "frame generation lock pipeline creation failed"; return false; }

    // Predict-mode motion pipeline: cur, prev, motion -> predicted motion + stored frame
    VkDescriptorSetLayoutBinding pb[7] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    lci.bindingCount = 7;
    lci.pBindings = pb;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->pSetLayout);
    pli.pSetLayouts = &d_->pSetLayout;
    VkPushConstantRange ppcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    pli.pPushConstantRanges = &ppcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->pLayout);
    smi.codeSize = sizeof kFgPredMvSpv;
    smi.pCode = kFgPredMvSpv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    cpi.layout = d_->pLayout;
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->pPipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "frame generation predict pipeline creation failed"; return false; }

    // Fast-motion guard pipeline
    VkDescriptorSetLayoutBinding cb[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    lci.bindingCount = 4;
    lci.pBindings = cb;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->cSetLayout);
    VkPushConstantRange cpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    pli.pSetLayouts = &d_->cSetLayout;
    pli.pPushConstantRanges = &cpcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->cLayout);
    smi.codeSize = sizeof kFgCheckSpv;
    smi.pCode = kFgCheckSpv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    cpi.layout = d_->cLayout;
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->cPipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "frame generation guard pipeline creation failed"; return false; }
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &sci, nullptr, &d_->linear);

    // Counter buffer: host-visible so the result can be read back for stats after the fence.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 16;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device, &bci, nullptr, &d_->counter);
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device, d_->counter, &breq);
    VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bai.allocationSize = breq.size;
    bai.memoryTypeIndex = d_->memType(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &bai, nullptr, &d_->counterMem);
    vkBindBufferMemory(device, d_->counter, d_->counterMem, 0);
    vkMapMemory(device, d_->counterMem, 0, VK_WHOLE_SIZE, 0, (void**)&d_->counterMap);
    ready_ = true;
    return true;
}

void FrameGen::shutdown() {
    if (!d_->dev) return;
    vkDeviceWaitIdle(d_->dev);
    d_->destroyContexts();
    if (d_->pipe) vkDestroyPipeline(d_->dev, d_->pipe, nullptr);
    if (d_->downPipe) vkDestroyPipeline(d_->dev, d_->downPipe, nullptr);
    if (d_->linearDown) vkDestroySampler(d_->dev, d_->linearDown, nullptr);
    if (d_->layout) vkDestroyPipelineLayout(d_->dev, d_->layout, nullptr);
    if (d_->setLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->setLayout, nullptr);
    if (d_->nearest) vkDestroySampler(d_->dev, d_->nearest, nullptr);
    if (d_->lPipe) vkDestroyPipeline(d_->dev, d_->lPipe, nullptr);
    if (d_->pPipe) vkDestroyPipeline(d_->dev, d_->pPipe, nullptr);
    if (d_->pLayout) vkDestroyPipelineLayout(d_->dev, d_->pLayout, nullptr);
    if (d_->pSetLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->pSetLayout, nullptr);
    if (d_->cPipe) vkDestroyPipeline(d_->dev, d_->cPipe, nullptr);
    if (d_->cLayout) vkDestroyPipelineLayout(d_->dev, d_->cLayout, nullptr);
    if (d_->cSetLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->cSetLayout, nullptr);
    if (d_->linear) vkDestroySampler(d_->dev, d_->linear, nullptr);
    if (d_->counterMap) vkUnmapMemory(d_->dev, d_->counterMem);
    if (d_->counter) vkDestroyBuffer(d_->dev, d_->counter, nullptr);
    if (d_->counterMem) vkFreeMemory(d_->dev, d_->counterMem, nullptr);
    if (d_->lLayout) vkDestroyPipelineLayout(d_->dev, d_->lLayout, nullptr);
    if (d_->lSetLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->lSetLayout, nullptr);
    d_->dev = VK_NULL_HANDLE;
    ready_ = false;
}

bool FrameGen::ensure(uint32_t w, uint32_t h, int flowPercent, std::string& err) {
    if (!ready_) { err = "FidelityFX not initialised"; return false; }
    Impl& d = *d_;
    if (d.ofLive && d.fiLive && d.w == w && d.h == h && d.flowPercent == flowPercent) return true;
    vkDeviceWaitIdle(d.dev);
    d.destroyContexts();

    // Optical flow can run on a smaller copy of the frame ("flow scale"); interpolation stays full-res.
    const int pct = flowPercent > 0 ? std::clamp(flowPercent, 10, 100) : (w > 2560 ? 50 : 100);
    d.ofW = pct == 100 ? w : std::max(64u, (uint32_t(std::lround(w * pct / 100.0)) + 1) & ~1u);
    d.ofH = pct == 100 ? h : std::max(36u, (uint32_t(std::lround(h * pct / 100.0)) + 1) & ~1u);
    FfxOpticalflowContextDescription od{};
    od.backendInterface = d.iface;
    od.resolution = {d.ofW, d.ofH};
    FfxErrorCode e = ffxOpticalflowContextCreate(&d.of, &od);
    if (e != FFX_OK) { err = "frame generation optical flow failed (" + std::to_string(e) + ")"; return false; }
    d.ofLive = true;
    FfxOpticalflowSharedResourceDescriptions ofs{};
    ffxOpticalflowGetSharedResourceDescriptions(&d.of, &ofs);
    auto& fv = ofs.opticalFlowVector.resourceDescription;
    auto& sc = ofs.opticalFlowSCD.resourceDescription;
    d.blockSize = fv.width ? (d.ofW + fv.width - 1) / fv.width : 8;

    FfxFrameInterpolationContextDescription fd{};
    fd.flags = FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    fd.maxRenderSize = {w, h};
    fd.displaySize = {w, h};
    fd.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fd.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fd.backendInterface = d.iface;
    e = ffxFrameInterpolationContextCreate(&d.fi, &fd);
    if (e != FFX_OK) { err = "frame interpolation context creation failed (" + std::to_string(e) + ")"; return false; }
    d.fiLive = true;
    FfxFrameInterpolationSharedResourceDescriptions fs{};
    ffxFrameInterpolationGetSharedResourceDescriptions(&d.fi, &fs);
    auto dim = [](const FfxCreateResourceDescription& c, uint32_t dw, uint32_t dh, uint32_t& ow, uint32_t& oh) {
        ow = c.resourceDescription.width ? c.resourceDescription.width : dw;
        oh = c.resourceDescription.height ? c.resourceDescription.height : dh;
    };
    uint32_t ddw, ddh, dmw, dmh, pdw, pdh;
    dim(fs.dilatedDepth, w, h, ddw, ddh);
    dim(fs.dilatedMotionVectors, w, h, dmw, dmh);
    dim(fs.reconstructedPrevNearestDepth, w, h, pdw, pdh);

    const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = d.create(d.flow, fv.width, fv.height, VK_FORMAT_R16G16_SINT, rw) &&
              d.create(d.scd, sc.width ? sc.width : 3, sc.height ? sc.height : 1, VK_FORMAT_R32_UINT, rw) &&
              d.create(d.motion, w, h, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.depth, w, h, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
              d.create(d.dilDepth, ddw, ddh, VK_FORMAT_R32_SFLOAT, rw) &&
              d.create(d.dilMotion, dmw, dmh, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.prevDepth, pdw, pdh, VK_FORMAT_R32_UINT, rw) &&
              d.create(d.output, w, h, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              (d.ofW == w || d.create(d.small, d.ofW, d.ofH, VK_FORMAT_R8G8B8A8_UNORM, rw)) &&
              d.create(d.predMv, w, h, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.mvHist[0], w, h, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.mvHist[1], w, h, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.prevFinal[0], w, h, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.prevFinal[1], w, h, VK_FORMAT_R8G8B8A8_UNORM, rw);
    if (!ok) { err = "frame generation resource allocation failed"; d.destroyContexts(); return false; }
    d.w = w;
    d.h = h;
    d.flowPercent = flowPercent;
    d.frameID = 0;
    d.havePrev = false;
    d.haveHist = false;
    return true;
}

bool FrameGen::record(VkCommandBuffer cmd, const FrameGenParams& p) {
    Impl& d = *d_;
    if (!d.ofLive || !d.fiLive) return false;
    bool reset = p.reset;
    if (d.depth.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        d.transition(cmd, d.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        VkClearColorValue far{{1.0f, 0, 0, 0}};
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, d.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &rr);
        d.transition(cmd, d.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (Img* i : {&d.flow, &d.scd, &d.dilDepth, &d.dilMotion, &d.prevDepth, &d.output, &d.prevFinal[0], &d.prevFinal[1]}) d.transition(cmd, *i, VK_IMAGE_LAYOUT_GENERAL);
        reset = true;
    }
    const uint64_t id = ++d.frameID;
    FfxCommandList cl = ffxGetCommandListVK(cmd);
    FfxResource color = ffxGetResourceVK((void*)p.color, ffxGetImageResourceDescriptionVK(p.color, p.colorInfo), L"PG_FgColor",
                                         p.colorInGeneral ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);

    // 1) Optical flow (on a downsampled copy when the flow scale is below 100%)
    FfxResource ofColor = color;
    if (d.ofW != d.w && p.colorView) {
        d.transition(cmd, d.small, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.downPipe);
        VkDescriptorImageInfo si{d.linearDown, p.colorView, p.colorInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo di{VK_NULL_HANDLE, d.small.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet dw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        dw[0].dstBinding = 0; dw[0].descriptorCount = 1; dw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; dw[0].pImageInfo = &si;
        dw[1].dstBinding = 1; dw[1].descriptorCount = 1; dw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; dw[1].pImageInfo = &di;
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 2, dw);
        struct { uint32_t w, h; float jx, jy; } dpc{d.ofW, d.ofH, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof dpc, &dpc);
        vkCmdDispatch(cmd, (d.ofW + 7) / 8, (d.ofH + 7) / 8, 1);
        d.transition(cmd, d.small, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ofColor = d.res(d.small, L"PG_FgSmall", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    }
    FfxOpticalflowDispatchDescription od{};
    od.commandList = cl;
    od.color = ofColor;
    od.opticalFlowVector = d.res(d.flow, L"PG_FgFlow", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    od.opticalFlowSCD = d.res(d.scd, L"PG_FgSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    od.reset = reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.minMaxLuminance = {0.0f, 1.0f};
    FfxErrorCode e = ffxOpticalflowContextDispatch(&d.of, &od);
    if (e != FFX_OK) { lastError = "frame generation optical flow failed (" + std::to_string(e) + ")"; return false; }

    // 2) Block flow -> per-pixel motion vectors
    d.transition(cmd, d.flow, VK_IMAGE_LAYOUT_GENERAL);
    d.transition(cmd, d.motion, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
    VkDescriptorImageInfo fi{d.nearest, d.flow.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi{VK_NULL_HANDLE, d.motion.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &fi;
    w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &mi;
    d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 2, w);
    struct { uint32_t w, h, block; float sx, sy; } pc{d.w, d.h, d.blockSize, float(d.w) / float(d.ofW), float(d.h) / float(d.ofH)};
    vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (d.w + 7) / 8, (d.h + 7) / 8, 1);
    d.transition(cmd, d.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Predict mode: clean up the motion field for the renderer's forward warp and stop here.
    if (p.predict) {
        if (!p.colorView) { lastError = "predict mode needs a colour view"; return false; }
        Img& prevRead = d.prevFinal[d.prevIdx];
        Img& prevWrite = d.prevFinal[1 - d.prevIdx];
        d.transition(cmd, prevRead, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, prevWrite, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, d.predMv, VK_IMAGE_LAYOUT_GENERAL);
        Img& histRead = d.mvHist[d.histIdx];
        Img& histWrite = d.mvHist[1 - d.histIdx];
        d.transition(cmd, histRead, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, histWrite, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pPipe);
        VkDescriptorImageInfo c0{d.nearest, p.colorView, p.colorInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo c1{d.linear, prevRead.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo c2{d.nearest, d.motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo s0{VK_NULL_HANDLE, d.predMv.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo s1{VK_NULL_HANDLE, prevWrite.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo h0{d.linear, histRead.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo h1{VK_NULL_HANDLE, histWrite.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo* pinfo[7] = {&c0, &c1, &c2, &s0, &s1, &h0, &h1};
        VkWriteDescriptorSet pw[7];
        for (int i = 0; i < 7; i++) {
            pw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            pw[i].dstBinding = uint32_t(i);
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = (i < 3 || i == 5) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            pw[i].pImageInfo = pinfo[i];
        }
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pLayout, 0, 7, pw);
        const bool hp = d.havePrev && !reset;
        struct { uint32_t w, h, havePrev, check; float maxPx; uint32_t haveHist; } ppc{d.w, d.h, hp ? 1u : 0u, p.lockStatic ? 1u : 0u, 0.08f * float(d.w),
                                                                                       (hp && d.haveHist) ? 1u : 0u};
        vkCmdPushConstants(cmd, d.pLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ppc, &ppc);
        vkCmdDispatch(cmd, (d.w + 7) / 8, (d.h + 7) / 8, 1);
        d.transition(cmd, d.predMv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        d.prevIdx = 1 - d.prevIdx;
        d.histIdx = 1 - d.histIdx;
        d.haveHist = hp;  // the history just written holds real motion only if there was a previous frame
        d.havePrev = true;
        d.counterPending = false;
        return true;
    }

    FfxResource dilDepth = d.res(d.dilDepth, L"PG_FgDilDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    FfxResource dilMotion = d.res(d.dilMotion, L"PG_FgDilMotion", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    FfxResource prevDepth = d.res(d.prevDepth, L"PG_FgPrevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);

    // 3) FI prepare: dilate motion / reconstruct previous depth (depth is a constant far plane)
    FfxFrameInterpolationPrepareDescription pd{};
    pd.commandList = cl;
    pd.renderSize = {d.w, d.h};
    pd.jitterOffset = {0.0f, 0.0f};
    pd.motionVectorScale = {1.0f, 1.0f};
    pd.frameTimeDelta = p.frameTimeMs;
    pd.cameraNear = 0.1f;
    pd.cameraFar = 100.0f;
    pd.viewSpaceToMetersFactor = 1.0f;
    pd.cameraFovAngleVertical = 1.0f;
    pd.depth = d.res(d.depth, L"PG_FgDepth", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    pd.motionVectors = d.res(d.motion, L"PG_FgMotion", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    pd.frameID = id;
    pd.dilatedDepth = dilDepth;
    pd.dilatedMotionVectors = dilMotion;
    pd.reconstructedPrevDepth = prevDepth;
    e = ffxFrameInterpolationPrepare(&d.fi, &pd);
    if (e != FFX_OK) { lastError = "frame interpolation prepare failed (" + std::to_string(e) + ")"; return false; }

    // 4) FI dispatch: frame halfway between the previous and this frame
    FfxFrameInterpolationDispatchDescription dd{};
    dd.commandList = cl;
    dd.displaySize = {d.w, d.h};
    dd.renderSize = {d.w, d.h};
    dd.currentBackBuffer = color;
    dd.output = d.res(d.output, L"PG_FgOutput", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    dd.interpolationRect = {0, 0, int32_t(d.w), int32_t(d.h)};
    dd.opticalFlowVector = od.opticalFlowVector;
    dd.opticalFlowSceneChangeDetection = od.opticalFlowSCD;
    dd.opticalFlowBufferSize = {d.flow.info.extent.width, d.flow.info.extent.height};
    dd.opticalFlowScale = {1.0f / float(d.ofW), 1.0f / float(d.ofH)};
    dd.opticalFlowBlockSize = int(d.blockSize);
    dd.cameraNear = 0.1f;
    dd.cameraFar = 100.0f;
    dd.cameraFovAngleVertical = 1.0f;
    dd.viewSpaceToMetersFactor = 1.0f;
    dd.frameTimeDelta = p.frameTimeMs;
    dd.reset = reset;
    dd.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dd.minMaxLuminance[0] = 0.0f;
    dd.minMaxLuminance[1] = 1.0f;
    dd.frameID = id;
    dd.dilatedDepth = dilDepth;
    dd.dilatedMotionVectors = dilMotion;
    dd.reconstructedPrevDepth = prevDepth;
    e = ffxFrameInterpolationDispatch(&d.fi, &dd);
    if (e != FFX_OK) { lastError = "frame interpolation dispatch failed (" + std::to_string(e) + ")"; return false; }

    // 5) Fast-motion guard + static-pixel lock + keep this frame for next time
    if (p.colorView) {
        Img& prevRead = d.prevFinal[d.prevIdx];
        Img& prevWrite = d.prevFinal[1 - d.prevIdx];
        const float maxFailed = 0.10f;
        // The last generated frame's command buffer has completed (the renderer waited on its fence).
        if (d.counterPending && d.counterMap[0] > 0) {
            lastFailedFraction = float(d.counterMap[1]) / float(d.counterMap[0]);
            float fastFraction = float(d.counterMap[2]) / float(d.counterMap[0]);
            if (lastFailedFraction > maxFailed || fastFraction > 0.25f) skippedFrames++;
        }
        vkCmdFillBuffer(cmd, d.counter, 0, 16, 0);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo mdi{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        mdi.memoryBarrierCount = 1;
        mdi.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &mdi);
        d.transition(cmd, d.output, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, prevRead, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, prevWrite, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorBufferInfo cbuf{d.counter, 0, 16};
        if (d.havePrev && !reset) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.cPipe);
            VkDescriptorImageInfo c0{d.linear, p.colorView, p.colorInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo c1{d.linear, prevRead.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo c2{d.linear, d.motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet cw[4];
            VkDescriptorImageInfo* ci3[3] = {&c0, &c1, &c2};
            for (int i = 0; i < 4; i++) {
                cw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                cw[i].dstBinding = uint32_t(i);
                cw[i].descriptorCount = 1;
                cw[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if (i < 3) cw[i].pImageInfo = ci3[i]; else cw[i].pBufferInfo = &cbuf;
            }
            d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.cLayout, 0, 4, cw);
            const uint32_t step = 4;
            struct { uint32_t w, h, step; float fastPx; } cpc{d.w, d.h, step, 0.03f * float(d.w)};
            vkCmdPushConstants(cmd, d.cLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cpc, &cpc);
            vkCmdDispatch(cmd, (d.w / step + 7) / 8, (d.h / step + 7) / 8, 1);
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            vkCmdPipelineBarrier2(cmd, &mdi);
            d.counterPending = true;
        } else {
            d.counterPending = false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.lPipe);
        VkDescriptorImageInfo ci{d.nearest, p.colorView, p.colorInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo pi{d.nearest, prevRead.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo gi{VK_NULL_HANDLE, d.output.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo si{VK_NULL_HANDLE, prevWrite.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo* infos[4] = {&ci, &pi, &gi, &si};
        VkWriteDescriptorSet lw[5];
        for (int i = 0; i < 5; i++) {
            lw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            lw[i].dstBinding = uint32_t(i);
            lw[i].descriptorCount = 1;
            lw[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i < 4) lw[i].pImageInfo = infos[i]; else lw[i].pBufferInfo = &cbuf;
        }
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.lLayout, 0, 5, lw);
        struct { uint32_t w, h, havePrev, debug; float maxFailed; } lpc{d.w, d.h, (d.havePrev && p.lockStatic && !reset) ? 1u : 0u, p.debugLock ? 1u : 0u,
                                                                       p.lockStatic ? maxFailed : 2.0f};
        vkCmdPushConstants(cmd, d.lLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof lpc, &lpc);
        vkCmdDispatch(cmd, (d.w + 7) / 8, (d.h + 7) / 8, 1);
        d.prevIdx = 1 - d.prevIdx;
        d.havePrev = true;
    }
    d.transition(cmd, d.output, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    return true;
}

VkImageView FrameGen::outputView() const { return d_->output.view; }
VkImageView FrameGen::predictMotionView() const { return d_->predMv.view; }
uint32_t FrameGen::flowWidth() const { return d_->ofW; }
uint32_t FrameGen::flowHeight() const { return d_->ofH; }
uint32_t FrameGen::width() const { return d_->w; }
uint32_t FrameGen::height() const { return d_->h; }
