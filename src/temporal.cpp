#include "temporal.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <dlfcn.h>
#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>

#include <cstdlib>
#include <filesystem>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <vector>

static const uint32_t kMotionSpv[] =
#include "motion.comp.inc"
    ;
static const uint32_t kJitterSpv[] =
#include "jitter.comp.inc"
    ;
static const uint32_t kJitter16Spv[] =
#include "jitter16.comp.inc"
    ;

// Vulkan backend description for the FFX API (FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK = 3).
struct FfxApiBackendVkDesc {
    ffxCreateContextDescHeader header;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
};

static FfxApiResource apiImage(VkImage img, uint32_t format, uint32_t w, uint32_t h, uint32_t state, uint32_t usage) {
    FfxApiResource r{};
    r.resource = (void*)img;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = format;
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = usage;
    r.state = state;
    return r;
}

static void onFsr4Message(uint32_t type, const wchar_t* msg) { fprintf(stderr, "[fsr4] %ls\n", msg); }

static float halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) { f /= float(base); r += f * float(i % base); }
    return r;
}

namespace {

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

void onFfxMessage(FfxMsgType type, const wchar_t* msg) {
    fprintf(stderr, "[ffx %s] %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

}  // namespace

struct Temporal::Impl {
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    std::vector<uint8_t> scratch;
    FfxInterface iface{};
    FfxOpticalflowContext of{};
    FfxFsr3UpscalerContext fsr{};
    bool ofLive = false, fsrLive = false;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    Img flow, scd, motion, depth, output, debug;
    Img dilDepth, dilMotion, prevDepth;  // FSR 3.1 shared resources (always registered)
    Img jittered;
    Img prevColor[2], reactive;
    int prevIdx = 0;
    bool havePrev = false;
    uint32_t jitterIndex = 0;
    VkSampler linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout jSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout jLayout = VK_NULL_HANDLE;
    VkPipeline jPipe = VK_NULL_HANDLE, jPipe16 = VK_NULL_HANDLE;
    // FSR 4 via fsr4-vk (GPL-3.0), loaded at runtime through the FFX API
    void* fsr4Lib = nullptr;
    PfnFfxCreateContext fsr4Create = nullptr;
    PfnFfxDispatch fsr4Dispatch = nullptr;
    PfnFfxDestroyContext fsr4Destroy = nullptr;
    ffxContext fsr4Ctx = nullptr;
    bool fsr4Wanted = false;
    Img jittered16, output16, exposure;  // exposure: 1x1 R32F = 1.0 (SDR capture)
    uint32_t blockSize = 8;

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

    bool create(Img& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
        destroy(img);
        img.info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img.info.imageType = VK_IMAGE_TYPE_2D;
        img.info.format = fmt;
        img.info.extent = {w, h, 1};
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
        if (fsrLive) ffxFsr3UpscalerContextDestroy(&fsr);
        if (ofLive) ffxOpticalflowContextDestroy(&of);
        if (fsr4Ctx && fsr4Destroy) fsr4Destroy(&fsr4Ctx, nullptr);
        fsr4Ctx = nullptr;
        fsrLive = ofLive = false;
        for (Img* i : {&flow, &scd, &motion, &depth, &output, &debug, &dilDepth, &dilMotion, &prevDepth, &jittered, &prevColor[0], &prevColor[1], &reactive, &jittered16, &output16, &exposure}) destroy(*i);
        havePrev = false;
        inW = inH = outW = outH = 0;
    }

    // Layout transition with a full compute/fragment dependency (cheap at this scale).
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

Temporal::Temporal() : d_(std::make_unique<Impl>()) {}
Temporal::~Temporal() { shutdown(); }

bool Temporal::init(VkInstance, VkPhysicalDevice phys, VkDevice device, std::string& err) {
    d_->phys = phys;
    d_->dev = device;
    const size_t maxContexts = 2;  // optical flow + FSR 3 upscaler
    d_->scratch.resize(ffxGetScratchMemorySizeVK(phys, maxContexts));
    VkDeviceContext dc{device, phys, vkGetDeviceProcAddr};
    FfxErrorCode e = ffxGetInterfaceVK(&d_->iface, ffxGetDeviceVK(&dc), d_->scratch.data(), d_->scratch.size(), maxContexts);
    if (e != FFX_OK) { err = "FidelityFX Vulkan backend init failed (" + std::to_string(e) + ")"; return false; }

    d_->push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &sci, nullptr, &d_->nearest);

    VkDescriptorSetLayoutBinding b[7] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = 7;
    lci.pBindings = b;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->setLayout);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d_->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->layout);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof kMotionSpv;
    smi.pCode = kMotionSpv;
    VkShaderModule sm;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpi.layout = d_->layout;
    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->pipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "motion pipeline creation failed"; return false; }

    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &sci, nullptr, &d_->linear);
    VkDescriptorSetLayoutBinding jb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    lci.bindingCount = 2;
    lci.pBindings = jb;
    vkCreateDescriptorSetLayout(device, &lci, nullptr, &d_->jSetLayout);
    VkPushConstantRange jpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    pli.pSetLayouts = &d_->jSetLayout;
    pli.pPushConstantRanges = &jpcr;
    vkCreatePipelineLayout(device, &pli, nullptr, &d_->jLayout);
    smi.codeSize = sizeof kJitterSpv;
    smi.pCode = kJitterSpv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    cpi.layout = d_->jLayout;
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->jPipe);
    vkDestroyShaderModule(device, sm, nullptr);
    if (r != VK_SUCCESS) { err = "jitter pipeline creation failed"; return false; }
    smi.codeSize = sizeof kJitter16Spv;
    smi.pCode = kJitter16Spv;
    vkCreateShaderModule(device, &smi, nullptr, &sm);
    cpi.stage.module = sm;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &d_->jPipe16);
    vkDestroyShaderModule(device, sm, nullptr);

    // fsr4-vk writes a diagnostic log (fsr4.log) into the working directory unless told otherwise.
    if (!getenv("FSR4_VK_LOG_PATH")) {
        std::string dir = getenv("XDG_CACHE_HOME") && *getenv("XDG_CACHE_HOME") ? getenv("XDG_CACHE_HOME") : std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.cache";
        dir += "/lyvra";
        std::error_code mk;
        std::filesystem::create_directories(dir, mk);
        setenv("FSR4_VK_LOG_PATH", dir.c_str(), 0);
    }

    // FSR 4: find fsr4-vk's amd_fidelityfx_upscaler_vk.so (optional; FSR 4 stays unavailable without it).
    std::vector<std::string> candidates;
    if (const char* e = getenv("LYVRA_FSR4_LIB"); e && *e) candidates.push_back(e);
    std::error_code ec;
    auto exeDir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    if (!ec) {
        candidates.push_back((exeDir / "amd_fidelityfx_upscaler_vk.so").string());
        candidates.push_back((exeDir / "../lib/lyvra/amd_fidelityfx_upscaler_vk.so").string());
    }
    if (const char* h = getenv("HOME")) candidates.push_back(std::string(h) + "/.local/share/lyvra/amd_fidelityfx_upscaler_vk.so");
    fsr4Status = "fsr4-vk library (amd_fidelityfx_upscaler_vk.so) not found";
    for (auto& c : candidates) {
        if (!std::filesystem::exists(c, ec)) continue;
        d_->fsr4Lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!d_->fsr4Lib) { fsr4Status = std::string("failed to load ") + c + ": " + dlerror(); continue; }
        d_->fsr4Create = (PfnFfxCreateContext)dlsym(d_->fsr4Lib, "ffxCreateContext");
        d_->fsr4Dispatch = (PfnFfxDispatch)dlsym(d_->fsr4Lib, "ffxDispatch");
        d_->fsr4Destroy = (PfnFfxDestroyContext)dlsym(d_->fsr4Lib, "ffxDestroyContext");
        if (d_->fsr4Create && d_->fsr4Dispatch && d_->fsr4Destroy) { fsr4Status = "loaded " + c; break; }
        fsr4Status = c + " is missing FFX API entry points";
        dlclose(d_->fsr4Lib);
        d_->fsr4Lib = nullptr;
    }
    fprintf(stderr, "FSR 4: %s\n", fsr4Status.c_str());
    ready_ = true;
    return true;
}

void Temporal::shutdown() {
    if (!d_->dev) return;
    vkDeviceWaitIdle(d_->dev);
    d_->destroyContexts();
    if (d_->pipe) vkDestroyPipeline(d_->dev, d_->pipe, nullptr);
    if (d_->layout) vkDestroyPipelineLayout(d_->dev, d_->layout, nullptr);
    if (d_->setLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->setLayout, nullptr);
    if (d_->nearest) vkDestroySampler(d_->dev, d_->nearest, nullptr);
    if (d_->linear) vkDestroySampler(d_->dev, d_->linear, nullptr);
    if (d_->jPipe) vkDestroyPipeline(d_->dev, d_->jPipe, nullptr);
    if (d_->jPipe16) vkDestroyPipeline(d_->dev, d_->jPipe16, nullptr);
    if (d_->jLayout) vkDestroyPipelineLayout(d_->dev, d_->jLayout, nullptr);
    if (d_->jSetLayout) vkDestroyDescriptorSetLayout(d_->dev, d_->jSetLayout, nullptr);
    d_->dev = VK_NULL_HANDLE;
    ready_ = false;
}

bool Temporal::fsr4Available() const { return d_->fsr4Lib != nullptr; }

bool Temporal::ensure(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, bool fsr4, std::string& err) {
    if (!ready_) { err = "FidelityFX not initialised"; return false; }
    Impl& d = *d_;
    if (fsr4 && !d.fsr4Lib) { err = fsr4Status; return false; }
    bool fsr4Ready = !fsr4 || d.fsr4Ctx;
    if (d.ofLive && d.fsrLive && fsr4Ready && d.inW == inW && d.inH == inH && d.outW == outW && d.outH == outH) return true;
    vkDeviceWaitIdle(d.dev);
    d.destroyContexts();

    FfxOpticalflowContextDescription od{};
    od.backendInterface = d.iface;
    od.resolution = {inW, inH};
    FfxErrorCode e = ffxOpticalflowContextCreate(&d.of, &od);
    if (e != FFX_OK) { err = "Optical flow context creation failed (" + std::to_string(e) + ")"; return false; }
    d.ofLive = true;

    FfxOpticalflowSharedResourceDescriptions shared{};
    ffxOpticalflowGetSharedResourceDescriptions(&d.of, &shared);
    auto& fv = shared.opticalFlowVector.resourceDescription;
    auto& sc = shared.opticalFlowSCD.resourceDescription;
    d.blockSize = fv.width ? (inW + fv.width - 1) / fv.width : 8;

    FfxFsr3UpscalerContextDescription fd{};
    fd.flags = 0;  // LDR input, standard depth, render-resolution motion vectors
    fd.maxRenderSize = {inW, inH};
    fd.maxUpscaleSize = {outW, outH};
    fd.fpMessage = onFfxMessage;
    fd.backendInterface = d.iface;
    e = ffxFsr3UpscalerContextCreate(&d.fsr, &fd);
    if (e != FFX_OK) { err = "FSR 3 upscaler context creation failed (" + std::to_string(e) + ")"; return false; }
    d.fsrLive = true;

    const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = d.create(d.flow, fv.width, fv.height, VK_FORMAT_R16G16_SINT, rw) &&
              d.create(d.scd, sc.width ? sc.width : 3, sc.height ? sc.height : 1, VK_FORMAT_R32_UINT, rw) &&
              d.create(d.motion, inW, inH, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.depth, inW, inH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
              d.create(d.output, outW, outH, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.debug, inW, inH, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.dilDepth, inW, inH, VK_FORMAT_R32_SFLOAT, rw) &&
              d.create(d.dilMotion, inW, inH, VK_FORMAT_R16G16_SFLOAT, rw) &&
              d.create(d.prevDepth, inW, inH, VK_FORMAT_R32_UINT, rw) &&
              d.create(d.jittered, inW, inH, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.prevColor[0], inW, inH, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.prevColor[1], inW, inH, VK_FORMAT_R8G8B8A8_UNORM, rw) &&
              d.create(d.reactive, inW, inH, VK_FORMAT_R8_UNORM, rw);
    if (!ok) { err = "Temporal resource allocation failed"; d.destroyContexts(); return false; }
    d.inW = inW;
    d.inH = inH;
    d.outW = outW;
    d.outH = outH;

    if (fsr4) {
        if (!d.create(d.jittered16, inW, inH, VK_FORMAT_R16G16B16A16_SFLOAT, rw) ||
            !d.create(d.output16, outW, outH, VK_FORMAT_R16G16B16A16_SFLOAT, rw) ||
            !d.create(d.exposure, 1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            err = "FSR 4 resource allocation failed";
            return false;
        }
        FfxApiBackendVkDesc be{};
        be.header.type = 3;  // FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK
        be.device = d.dev;
        be.physicalDevice = d.phys;
        be.getDeviceProcAddr = vkGetDeviceProcAddr;
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.maxRenderSize = {inW, inH};
        cd.maxUpscaleSize = {outW, outH};
        cd.fpMessage = onFsr4Message;
        ffxReturnCode_t rc = d.fsr4Create(&d.fsr4Ctx, &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            d.fsr4Ctx = nullptr;
            err = "FSR 4 context creation failed (" + std::to_string(rc) + ")";
            return false;
        }
    }
    return true;
}

bool Temporal::record(VkCommandBuffer cmd, const TemporalParams& p) {
    Impl& d = *d_;
    if (!d.ofLive || !d.fsrLive) return false;
    bool reset = p.reset;

    // One-time setup of freshly created images.
    if (d.depth.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        d.transition(cmd, d.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        VkClearColorValue far{{1.0f, 0, 0, 0}};
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, d.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &rr);
        d.transition(cmd, d.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (Img* i : {&d.flow, &d.scd, &d.output, &d.debug, &d.dilDepth, &d.dilMotion, &d.prevDepth, &d.prevColor[0], &d.prevColor[1]}) d.transition(cmd, *i, VK_IMAGE_LAYOUT_GENERAL);
        if (d.output16.image) d.transition(cmd, d.output16, VK_IMAGE_LAYOUT_GENERAL);
        if (d.exposure.image) {
            d.transition(cmd, d.exposure, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            VkClearColorValue one{{1.0f, 0, 0, 0}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, d.exposure.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &rr);
            d.transition(cmd, d.exposure, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        reset = true;
    }

    // 1) Optical flow on the converted frame
    FfxOpticalflowDispatchDescription od{};
    od.commandList = ffxGetCommandListVK(cmd);
    od.color = ffxGetResourceVK((void*)p.colorImage, ffxGetImageResourceDescriptionVK(p.colorImage, p.colorInfo), L"PG_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    od.opticalFlowVector = d.res(d.flow, L"PG_OpticalFlow", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    od.opticalFlowSCD = d.res(d.scd, L"PG_OpticalFlowSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    od.reset = reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.minMaxLuminance = {0.0f, 1.0f};
    FfxErrorCode e = ffxOpticalflowContextDispatch(&d.of, &od);
    if (e != FFX_OK) { lastError = "optical flow dispatch failed (" + std::to_string(e) + ")"; return false; }

    // 2) Block vectors -> per-pixel motion vectors (+ debug view)
    d.transition(cmd, d.flow, VK_IMAGE_LAYOUT_GENERAL);  // makes the FFX writes visible
    d.transition(cmd, d.motion, VK_IMAGE_LAYOUT_GENERAL);
    if (p.debugMotion) d.transition(cmd, d.debug, VK_IMAGE_LAYOUT_GENERAL);
    Img& prevRead = d.prevColor[d.prevIdx];
    Img& prevWrite = d.prevColor[1 - d.prevIdx];
    d.transition(cmd, prevRead, VK_IMAGE_LAYOUT_GENERAL);
    d.transition(cmd, prevWrite, VK_IMAGE_LAYOUT_GENERAL);
    d.transition(cmd, d.reactive, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
    VkDescriptorImageInfo flowInfo{d.nearest, d.flow.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo motionInfo{VK_NULL_HANDLE, d.motion.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo debugInfo{VK_NULL_HANDLE, d.debug.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo curInfo{d.linear, p.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo prevInfo{d.linear, prevRead.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo copyInfo{VK_NULL_HANDLE, prevWrite.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo reactInfo{VK_NULL_HANDLE, d.reactive.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[7];
    VkDescriptorImageInfo* infos[7] = {&flowInfo, &motionInfo, &debugInfo, &curInfo, &prevInfo, &copyInfo, &reactInfo};
    for (int i = 0; i < 7; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = uint32_t(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = (i == 0 || i == 3 || i == 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = infos[i];
    }
    d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 7, w);
    struct { uint32_t w, h, block, debug; float scale; uint32_t havePrev; } pc{d.inW, d.inH, d.blockSize, p.debugMotion ? 1u : 0u, 24.0f, d.havePrev && !reset ? 1u : 0u};
    vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (d.inW + 7) / 8, (d.inH + 7) / 8, 1);
    d.transition(cmd, d.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    d.transition(cmd, d.reactive, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    d.prevIdx = 1 - d.prevIdx;
    d.havePrev = true;
    if (p.debugMotion) d.transition(cmd, d.debug, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    // 3) Synthetic jitter: FSR's recommended Halton(2,3) phase count for this scale factor
    float jx = 0, jy = 0;
    FfxResource color = od.color;
    if (p.fsr4 && d.fsr4Ctx && p.colorView) {
        // FSR 4: float input (RGBA16F), always written (jittered or not)
        if (p.jitter) {
            float ratio = float(d.outW) / float(d.inW);
            uint32_t phases = uint32_t(std::max(8.0f, 8.0f * ratio * ratio));
            uint32_t i = (d.jitterIndex++ % phases) + 1;
            jx = halton(i, 2) - 0.5f;
            jy = halton(i, 3) - 0.5f;
        }
        d.transition(cmd, d.jittered16, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jPipe16);
        VkDescriptorImageInfo si{d.linear, p.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo di{VK_NULL_HANDLE, d.jittered16.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet jw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        jw[0].dstBinding = 0; jw[0].descriptorCount = 1; jw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; jw[0].pImageInfo = &si;
        jw[1].dstBinding = 1; jw[1].descriptorCount = 1; jw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; jw[1].pImageInfo = &di;
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jLayout, 0, 2, jw);
        struct { uint32_t w, h; float jx, jy; } jpc{d.inW, d.inH, jx, jy};
        vkCmdPushConstants(cmd, d.jLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof jpc, &jpc);
        vkCmdDispatch(cmd, (d.inW + 7) / 8, (d.inH + 7) / 8, 1);
        d.transition(cmd, d.jittered16, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        d.transition(cmd, d.output16, VK_IMAGE_LAYOUT_GENERAL);

        ffxDispatchDescUpscale ud{};
        ud.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        ud.commandList = cmd;
        ud.color = apiImage(d.jittered16.image, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, d.inW, d.inH, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
        ud.depth = apiImage(d.depth.image, FFX_API_SURFACE_FORMAT_R32_FLOAT, d.inW, d.inH, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
        ud.motionVectors = apiImage(d.motion.image, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, d.inW, d.inH, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
        ud.exposure = apiImage(d.exposure.image, FFX_API_SURFACE_FORMAT_R32_FLOAT, 1, 1, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
        ud.reactive = apiImage(d.reactive.image, FFX_API_SURFACE_FORMAT_R8_UNORM, d.inW, d.inH, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
        ud.output = apiImage(d.output16.image, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, d.outW, d.outH, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        ud.jitterOffset = {jx, jy};
        ud.motionVectorScale = {1.0f, 1.0f};
        ud.renderSize = {d.inW, d.inH};
        ud.upscaleSize = {d.outW, d.outH};
        ud.enableSharpening = p.sharpness > 0.0f;
        ud.sharpness = p.sharpness;
        ud.frameTimeDelta = p.frameTimeMs;
        ud.preExposure = 1.0f;
        ud.reset = reset;
        ud.cameraNear = 0.1f;
        ud.cameraFar = 100.0f;
        ud.cameraFovAngleVertical = 1.0f;
        ud.viewSpaceToMetersFactor = 1.0f;
        ffxReturnCode_t rc = d.fsr4Dispatch(&d.fsr4Ctx, &ud.header);
        if (rc != FFX_API_RETURN_OK) { lastError = "FSR 4 dispatch failed (" + std::to_string(rc) + ")"; return false; }

        // FSR 4 output (RGBA16F) -> the RGBA8 output the rest of the pipeline uses
        d.transition(cmd, d.output16, VK_IMAGE_LAYOUT_GENERAL);
        d.transition(cmd, d.output, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jPipe);
        VkDescriptorImageInfo s2{d.nearest, d.output16.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo d2{VK_NULL_HANDLE, d.output.view, VK_IMAGE_LAYOUT_GENERAL};
        jw[0].pImageInfo = &s2;
        jw[1].pImageInfo = &d2;
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jLayout, 0, 2, jw);
        struct { uint32_t w, h; float jx, jy; } cpc{d.outW, d.outH, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, d.jLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cpc, &cpc);
        vkCmdDispatch(cmd, (d.outW + 7) / 8, (d.outH + 7) / 8, 1);
        d.transition(cmd, d.output, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        return true;
    }
    if (p.jitter && p.colorView) {
        float ratio = float(d.outW) / float(d.inW);
        uint32_t phases = uint32_t(std::max(8.0f, 8.0f * ratio * ratio));
        uint32_t i = (d.jitterIndex++ % phases) + 1;
        jx = halton(i, 2) - 0.5f;
        jy = halton(i, 3) - 0.5f;
        d.transition(cmd, d.jittered, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jPipe);
        VkDescriptorImageInfo si{d.linear, p.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo di{VK_NULL_HANDLE, d.jittered.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet jw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        jw[0].dstBinding = 0; jw[0].descriptorCount = 1; jw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; jw[0].pImageInfo = &si;
        jw[1].dstBinding = 1; jw[1].descriptorCount = 1; jw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; jw[1].pImageInfo = &di;
        d.push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.jLayout, 0, 2, jw);
        struct { uint32_t w, h; float jx, jy; } jpc{d.inW, d.inH, jx, jy};
        vkCmdPushConstants(cmd, d.jLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof jpc, &jpc);
        vkCmdDispatch(cmd, (d.inW + 7) / 8, (d.inH + 7) / 8, 1);
        d.transition(cmd, d.jittered, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        color = d.res(d.jittered, L"PG_JitteredColor", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    }

    // 4) FSR 3.1 temporal upscale
    FfxFsr3UpscalerDispatchDescription fd{};
    fd.commandList = ffxGetCommandListVK(cmd);
    fd.color = color;
    fd.depth = d.res(d.depth, L"PG_Depth", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    fd.motionVectors = d.res(d.motion, L"PG_MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    fd.reactive = d.res(d.reactive, L"PG_Reactive", FFX_RESOURCE_STATE_COMPUTE_READ, FFX_RESOURCE_USAGE_READ_ONLY);
    fd.output = d.res(d.output, L"PG_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    fd.dilatedDepth = d.res(d.dilDepth, L"PG_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    fd.dilatedMotionVectors = d.res(d.dilMotion, L"PG_DilatedMotion", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    fd.reconstructedPrevNearestDepth = d.res(d.prevDepth, L"PG_PrevNearestDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS, FFX_RESOURCE_USAGE_UAV);
    fd.jitterOffset = {jx, jy};             // synthetic, see step 3
    fd.motionVectorScale = {1.0f, 1.0f};   // vectors are in render-resolution pixels
    fd.renderSize = {d.inW, d.inH};
    fd.upscaleSize = {d.outW, d.outH};
    fd.enableSharpening = p.sharpness > 0.0f;
    fd.sharpness = p.sharpness;
    fd.frameTimeDelta = p.frameTimeMs;
    fd.preExposure = 1.0f;
    fd.reset = reset;
    fd.cameraNear = 0.1f;
    fd.cameraFar = 100.0f;
    fd.cameraFovAngleVertical = 1.0f;
    fd.viewSpaceToMetersFactor = 1.0f;
    e = ffxFsr3UpscalerContextDispatch(&d.fsr, &fd);
    if (e != FFX_OK) { lastError = "FSR 3 dispatch failed (" + std::to_string(e) + ")"; return false; }
    d.transition(cmd, d.output, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    return true;
}

VkImageView Temporal::outputView() const { return d_->output.view; }
VkImage Temporal::outputImage() const { return d_->output.image; }
VkImageCreateInfo Temporal::outputInfo() const { return d_->output.info; }
VkImageView Temporal::debugView() const { return d_->debug.view; }
uint32_t Temporal::outW() const { return d_->outW; }
uint32_t Temporal::outH() const { return d_->outH; }
