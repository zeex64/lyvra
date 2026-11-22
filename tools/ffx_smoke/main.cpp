// ffx_smoke: headless smoke test for the ffx_sdk static library.
//
// Creates a Vulkan 1.3 device on the discrete AMD GPU, sets up the FidelityFX VK
// backend and creates/destroys FSR3 upscaler, optical flow and frame interpolation
// contexts. Context creation builds every compute pipeline from the generated
// SPIR-V permutation headers, so this validates the shader toolchain end to end.
// It then (optionally) runs a few real FSR3 upscaler dispatches, comparing the LUT
// and reference reprojection-lanczos shader paths.
//
// Not installed; run from the build tree: ./ffx_smoke

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) {                                                  \
            std::fprintf(stderr, "%s failed: VkResult %d\n", #x, (int)r_);       \
            std::exit(2);                                                        \
        }                                                                        \
    } while (0)

static int g_failures = 0;

static void report(const char* what, FfxErrorCode code)
{
    std::printf("  %-58s -> FfxErrorCode %d (0x%08x)%s\n", what, (int)code, (unsigned)code, code == FFX_OK ? "  OK" : "  FAIL");
    if (code != FFX_OK)
        ++g_failures;
}

static void ffxMessageCb(FfxMsgType type, const wchar_t* msg)
{
    std::printf("    [ffx %s] %ls\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

// ------------------------------------------------------------------------------------
// Optional dispatch test: run a few FSR3 upscaler frames on a synthetic input and read
// the 4K output back. Done twice: once with the device's real capabilities (on RDNA,
// wave32/64 => the upscaler selects the REPROJECT_USE_LANCZOS_TYPE=1 "LUT" shaders)
// and once with capabilities patched to wave32-only (=> reference lanczos, TYPE=0).
// The two outputs should agree closely; a miscompiled LUT path would show up as a
// large difference or NaNs.
// ------------------------------------------------------------------------------------
struct Gpu {
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         qfi;
    VkCommandPool    pool;
};

static FfxGetDeviceCapabilitiesFunc g_realGetCaps = nullptr;
static FfxErrorCode                 capsWave32Only(FfxInterface* iface, FfxDeviceCapabilities* caps)
{
    FfxErrorCode rc        = g_realGetCaps(iface, caps);
    caps->waveLaneCountMin = 32;
    caps->waveLaneCountMax = 32;
    return rc;
}

static uint32_t findMemType(const Gpu& g, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    std::fprintf(stderr, "no memory type\n");
    std::exit(2);
}

struct Img {
    VkImage           image = VK_NULL_HANDLE;
    VkDeviceMemory    mem   = VK_NULL_HANDLE;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

static Img makeImage(const Gpu& g, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    Img i;
    i.ci.imageType     = VK_IMAGE_TYPE_2D;
    i.ci.format        = fmt;
    i.ci.extent        = {w, h, 1};
    i.ci.mipLevels     = 1;
    i.ci.arrayLayers   = 1;
    i.ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    i.ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    i.ci.usage         = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    i.ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(g.device, &i.ci, nullptr, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, i.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g.device, &ai, nullptr, &i.mem));
    VK_CHECK(vkBindImageMemory(g.device, i.image, i.mem, 0));
    return i;
}

static void destroyImage(const Gpu& g, Img& i)
{
    vkDestroyImage(g.device, i.image, nullptr);
    vkFreeMemory(g.device, i.mem, nullptr);
}

struct Buf {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          ptr = nullptr;
};

static Buf makeHostBuffer(const Gpu& g, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buf                b;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size  = size;
    bi.usage = usage;
    VK_CHECK(vkCreateBuffer(g.device, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = findMemType(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(g.device, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(g.device, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(g.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.ptr));
    return b;
}

static void destroyBuffer(const Gpu& g, Buf& b)
{
    vkDestroyBuffer(g.device, b.buf, nullptr);
    vkFreeMemory(g.device, b.mem, nullptr);
}

static VkCommandBuffer beginCmd(const Gpu& g)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = g.pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(g.device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void submitWait(const Gpu& g, VkCommandBuffer cb)
{
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(g.queue));
    vkFreeCommandBuffers(g.device, g.pool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static float halfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0)
        f = m ? 0 : (s << 31);  // flush denormals, fine for stats
    else if (e == 31)
        f = (s << 31) | 0x7f800000u | (m << 13);
    else
        f = (s << 31) | ((e + 112) << 23) | (m << 13);
    if (e == 0 && m) {
        float v = std::ldexp((float)m, -24);
        return s ? -v : v;
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

// Returns false on failure. Output: RGBA float pixels of the 3840x2160 result.
static bool runFsr3Frames(const Gpu& g, FfxInterface iface, bool forceReferenceLanczos, std::vector<float>& out)
{
    const uint32_t RW = 1920, RH = 1080, DW = 3840, DH = 2160, FRAMES = 8;
    if (forceReferenceLanczos) {
        g_realGetCaps                = iface.fpGetDeviceCapabilities;
        iface.fpGetDeviceCapabilities = capsWave32Only;
    }

    auto                              ctx = std::make_unique<FfxFsr3UpscalerContext>();
    FfxFsr3UpscalerContextDescription d{};
    d.flags            = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    d.maxRenderSize    = {RW, RH};
    d.maxUpscaleSize   = {DW, DH};
    d.fpMessage        = ffxMessageCb;
    d.backendInterface = iface;
    if (ffxFsr3UpscalerContextCreate(ctx.get(), &d) != FFX_OK)
        return false;

    const VkImageUsageFlags ro = VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    Img color  = makeImage(g, VK_FORMAT_R8G8B8A8_UNORM, RW, RH, ro);
    Img depth  = makeImage(g, VK_FORMAT_R32_SFLOAT, RW, RH, ro);
    Img mvec   = makeImage(g, VK_FORMAT_R16G16_SFLOAT, RW, RH, ro);
    Img dDepth = makeImage(g, VK_FORMAT_R32_SFLOAT, RW, RH, rw);
    Img dMv    = makeImage(g, VK_FORMAT_R16G16_SFLOAT, RW, RH, rw);
    Img rDepth = makeImage(g, VK_FORMAT_R32_UINT, RW, RH, rw);
    Img output = makeImage(g, VK_FORMAT_R16G16B16A16_SFLOAT, DW, DH, rw);

    // Synthetic, high-frequency color input (checkerboard + rings + gradients).
    Buf staging = makeHostBuffer(g, (VkDeviceSize)RW * RH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* px    = static_cast<uint8_t*>(staging.ptr);
    for (uint32_t y = 0; y < RH; ++y)
        for (uint32_t x = 0; x < RW; ++x) {
            uint8_t* p   = px + 4 * ((size_t)y * RW + x);
            float    dx  = (float)x - RW * 0.5f, dy = (float)y - RH * 0.5f;
            float    rng = 0.5f + 0.5f * std::sin(std::sqrt(dx * dx + dy * dy) * 0.15f);
            bool     chk = ((x / 7) ^ (y / 5)) & 1;
            p[0] = (uint8_t)(255.f * rng);
            p[1] = chk ? 230 : 20;
            p[2] = (uint8_t)(255.f * x / RW);
            p[3] = 255;
        }

    VkCommandBuffer cb = beginCmd(g);
    for (Img* i : {&color, &depth, &mvec})
        barrier(cb, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (Img* i : {&dDepth, &dMv, &rDepth, &output})
        barrier(cb, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {RW, RH, 1};
    vkCmdCopyBufferToImage(cb, staging.buf, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Constant sub-pixel motion so history reprojection actually resamples at
    // fractional positions (exercises the reproject lanczos kernel under test).
    VkClearColorValue       half{{0.5f, 0, 0, 0}}, motion{{0.37f / RW, -0.21f / RH, 0, 0}};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &half, 1, &all);
    vkCmdClearColorImage(cb, mvec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &motion, 1, &all);
    for (Img* i : {&color, &depth, &mvec})
        barrier(cb, i->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submitWait(g, cb);

    auto res = [](Img& i, FfxResourceUsage usage, const wchar_t* name, FfxResourceStates st) {
        return ffxGetResourceVK((void*)i.image, ffxGetImageResourceDescriptionVK(i.image, i.ci, usage), name, st);
    };

    bool        ok    = true;
    const int   phases = ffxFsr3UpscalerGetJitterPhaseCount(RW, DW);
    for (uint32_t f = 0; f < FRAMES && ok; ++f) {
        cb = beginCmd(g);
        FfxFsr3UpscalerDispatchDescription dd{};
        dd.commandList                   = ffxGetCommandListVK(cb);
        dd.color                         = res(color, FFX_RESOURCE_USAGE_READ_ONLY, L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.depth                         = res(depth, FFX_RESOURCE_USAGE_READ_ONLY, L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.motionVectors                 = res(mvec, FFX_RESOURCE_USAGE_READ_ONLY, L"mv", FFX_RESOURCE_STATE_COMPUTE_READ);
        dd.dilatedDepth                  = res(dDepth, FFX_RESOURCE_USAGE_UAV, L"dilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.dilatedMotionVectors          = res(dMv, FFX_RESOURCE_USAGE_UAV, L"dilatedMv", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.reconstructedPrevNearestDepth = res(rDepth, FFX_RESOURCE_USAGE_UAV, L"prevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.output                        = res(output, FFX_RESOURCE_USAGE_UAV, L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        ffxFsr3UpscalerGetJitterOffset(&dd.jitterOffset.x, &dd.jitterOffset.y, (int32_t)f, phases);
        dd.motionVectorScale       = {(float)RW, (float)RH};
        dd.renderSize              = {RW, RH};
        dd.upscaleSize             = {DW, DH};
        dd.enableSharpening        = false;
        dd.frameTimeDelta          = 16.7f;
        dd.preExposure             = 1.0f;
        dd.reset                   = (f == 0);
        dd.cameraNear              = 0.1f;
        dd.cameraFar               = 1000.0f;
        dd.cameraFovAngleVertical  = 1.0f;
        dd.viewSpaceToMetersFactor = 1.0f;
        FfxErrorCode rc            = ffxFsr3UpscalerContextDispatch(ctx.get(), &dd);
        if (rc != FFX_OK) {
            std::printf("    dispatch frame %u failed: %d\n", f, (int)rc);
            ok = false;
        }
        submitWait(g, cb);
    }

    if (ok) {
        Buf rb = makeHostBuffer(g, (VkDeviceSize)DW * DH * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        cb     = beginCmd(g);
        barrier(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent      = {DW, DH, 1};
        vkCmdCopyImageToBuffer(cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
        submitWait(g, cb);
        const auto* h = static_cast<const uint16_t*>(rb.ptr);
        out.resize((size_t)DW * DH * 4);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = halfToFloat(h[i]);
        destroyBuffer(g, rb);
    }

    VK_CHECK(vkDeviceWaitIdle(g.device));
    if (ffxFsr3UpscalerContextDestroy(ctx.get()) != FFX_OK)
        ok = false;
    for (Img* i : {&color, &depth, &mvec, &dDepth, &dMv, &rDepth, &output})
        destroyImage(g, *i);
    destroyBuffer(g, staging);
    return ok;
}

static void dispatchTest(const Gpu& g, const FfxInterface& iface)
{
    std::printf("FSR3 upscaler dispatch test (8 frames, 1920x1080 -> 3840x2160):\n");
    std::vector<float> lut, ref;
    bool               okLut = runFsr3Frames(g, iface, false, lut);
    bool               okRef = runFsr3Frames(g, iface, true, ref);
    report("dispatch, native caps (wave32/64 -> LUT reproject lanczos)", okLut ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR);
    report("dispatch, wave32-only caps (reference reproject lanczos)", okRef ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR);
    if (!okLut || !okRef)
        return;
    auto stats = [](const std::vector<float>& v, const char* name) {
        size_t nans = 0;
        double sum = 0, mn = 1e30, mx = -1e30;
        for (size_t i = 0; i < v.size(); i += 4)
            for (int c = 0; c < 3; ++c) {
                float x = v[i + c];
                if (!std::isfinite(x)) { ++nans; continue; }
                sum += x;
                mn = x < mn ? x : mn;
                mx = x > mx ? x : mx;
            }
        std::printf("    %-10s mean %.5f  min %.5f  max %.5f  non-finite %zu\n", name, sum / (v.size() / 4 * 3), mn, mx, nans);
        return nans;
    };
    size_t nanLut = stats(lut, "LUT");
    size_t nanRef = stats(ref, "reference");
    double sad = 0, maxd = 0;
    size_t over = 0;
    for (size_t i = 0; i < lut.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            double dlt = std::fabs((double)lut[i + c] - (double)ref[i + c]);
            if (!std::isfinite(dlt)) continue;
            sad += dlt;
            maxd = dlt > maxd ? dlt : maxd;
            if (dlt > 0.05) ++over;
        }
    double mad = sad / (lut.size() / 4 * 3);
    std::printf("    LUT vs reference: mean |diff| %.6f, max |diff| %.5f, channels with |diff|>0.05: %zu (%.4f%%)\n", mad, maxd, over,
                100.0 * over / (lut.size() / 4 * 3));
    bool sane = nanLut == 0 && nanRef == 0 && mad < 0.01;
    report("LUT vs reference output agree (no NaN, mean |diff| < 0.01)", sane ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR);
}

int main()
{
    // ---- Instance ---------------------------------------------------------------
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ffx_smoke";
    app.apiVersion       = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance  = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    // ---- Physical device: discrete AMD preferred --------------------------------
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    int              best = -1;
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 0) + (p.vendorID == 0x1002 ? 1 : 0);
        if (p.apiVersion < VK_API_VERSION_1_3)
            score = -1;
        if (score > best) {
            best = score;
            phys = pd;
        }
    }
    if (!phys) {
        std::fprintf(stderr, "no Vulkan 1.3 device\n");
        return 2;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("GPU: %s (api %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    // ---- Queue family with graphics+compute -------------------------------------
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(n);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qfs.data());
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i)
        if ((qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            qfi = i;
            break;
        }
    if (qfi == UINT32_MAX) {
        std::fprintf(stderr, "no graphics+compute queue\n");
        return 2;
    }

    // ---- Features the FFX VK backend / generated SPIR-V rely on ------------------
    // The backend picks permutations from what the device *supports* (it queries
    // extension/feature support, not what was enabled), so enable these:
    //   core:  shaderInt16, shaderStorageImageReadWithoutFormat/WriteWithoutFormat
    //   1.1:   storageBuffer16BitAccess, uniformAndStorageBuffer16BitAccess
    //   1.2:   shaderFloat16 (fp16 permutations), descriptorIndexing subset
    //   1.3:   subgroupSizeControl, computeFullSubgroups, synchronization2
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2        f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext  = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2        e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.pNext  = &e11;
    e11.pNext = &e12;
    e12.pNext = &e13;
    e2.features.shaderInt16                         = f2.features.shaderInt16;
    e2.features.shaderStorageImageReadWithoutFormat  = f2.features.shaderStorageImageReadWithoutFormat;
    e2.features.shaderStorageImageWriteWithoutFormat = f2.features.shaderStorageImageWriteWithoutFormat;
    e2.features.shaderImageGatherExtended            = f2.features.shaderImageGatherExtended;
    e11.storageBuffer16BitAccess                    = f11.storageBuffer16BitAccess;
    e11.uniformAndStorageBuffer16BitAccess          = f11.uniformAndStorageBuffer16BitAccess;
    e12.shaderFloat16                               = f12.shaderFloat16;
    e12.shaderStorageBufferArrayNonUniformIndexing  = f12.shaderStorageBufferArrayNonUniformIndexing;
    e12.shaderSampledImageArrayNonUniformIndexing   = f12.shaderSampledImageArrayNonUniformIndexing;
    e12.shaderStorageImageArrayNonUniformIndexing   = f12.shaderStorageImageArrayNonUniformIndexing;
    e12.runtimeDescriptorArray                      = f12.runtimeDescriptorArray;
    e13.subgroupSizeControl                         = f13.subgroupSizeControl;
    e13.computeFullSubgroups                        = f13.computeFullSubgroups;
    e13.synchronization2                            = f13.synchronization2;
    std::printf("features: shaderFloat16=%u shaderInt16=%u sb16=%u imgReadNoFmt=%u imgWriteNoFmt=%u subgroupSizeControl=%u\n",
                e12.shaderFloat16, e2.features.shaderInt16, e11.storageBuffer16BitAccess,
                e2.features.shaderStorageImageReadWithoutFormat, e2.features.shaderStorageImageWriteWithoutFormat,
                e13.subgroupSizeControl);

    // ---- Device extensions ---------------------------------------------------------
    // REQUIRED even on a 1.3 device: ffx_vk.cpp loads vkGetBufferMemoryRequirements2KHR
    // by its KHR name and calls it unconditionally on first context creation whenever
    // VK_KHR_dedicated_allocation is *advertised*; the KHR alias is only returned by
    // vkGetDeviceProcAddr when VK_KHR_get_memory_requirements2 is *enabled*, otherwise
    // it is NULL and the backend crashes. Enable both (they are promoted, so harmless).
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> availExts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, availExts.data());
    auto hasExt = [&](const char* name) {
        for (const auto& e : availExts)
            if (std::strcmp(e.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char*> exts;
    for (const char* e : {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME})
        if (hasExt(e))
            exts.push_back(e);
    for (const char* e : exts)
        std::printf("enabling device extension %s\n", e);

    float                   prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext                   = &e2;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.data();
    VkDevice device          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));

    // ---- FFX backend interface -------------------------------------------------
    const size_t    maxContexts = 4;  // upscaler + optical flow + frame interpolation (+1 spare)
    VkDeviceContext vkdc{device, phys, vkGetDeviceProcAddr};
    size_t          scratchSize = ffxGetScratchMemorySizeVK(phys, maxContexts);
    void*           scratch     = std::calloc(1, scratchSize);
    FfxInterface    iface{};
    FfxErrorCode    rc = ffxGetInterfaceVK(&iface, ffxGetDeviceVK(&vkdc), scratch, scratchSize, maxContexts);
    std::printf("ffxGetScratchMemorySizeVK(maxContexts=%zu) = %zu bytes\n", maxContexts, scratchSize);
    report("ffxGetInterfaceVK", rc);
    if (rc != FFX_OK)
        return 1;

    // ---- FSR3 upscaler: default flags + a sweep over permutation-relevant flags ----
    {
        static const struct { uint32_t flags; const char* name; } cases[] = {
            {FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE, "AUTO_EXPOSURE"},
            {FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE | FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE, "AUTO_EXPOSURE|HDR"},
            {FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE, "DEPTH_INVERTED|INFINITE"},
            {FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION,
             "DISPLAY_RES_MV|JITTER_CANCELLATION"},
            {FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
                 FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
                 FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION,
             "HDR|DEPTH_INVERTED|DISPLAY_RES_MV|JITTER|DYNAMIC_RES"},
        };
        auto ctx = std::make_unique<FfxFsr3UpscalerContext>();
        for (const auto& c : cases) {
            FfxFsr3UpscalerContextDescription d{};
            d.flags            = c.flags;
            d.maxRenderSize    = {1920, 1080};
            d.maxUpscaleSize   = {3840, 2160};
            d.fpMessage        = ffxMessageCb;
            d.backendInterface = iface;
            char label[160];
            std::snprintf(label, sizeof(label), "ffxFsr3UpscalerContextCreate [%s]", c.name);
            rc = ffxFsr3UpscalerContextCreate(ctx.get(), &d);
            report(label, rc);
            if (rc == FFX_OK)
                report("ffxFsr3UpscalerContextDestroy", ffxFsr3UpscalerContextDestroy(ctx.get()));
        }
    }

    // ---- Optical flow -----------------------------------------------------------
    {
        auto                            ctx = std::make_unique<FfxOpticalflowContext>();
        FfxOpticalflowContextDescription d{};
        d.backendInterface = iface;
        d.flags            = 0;
        d.resolution       = {1920, 1080};
        rc                 = ffxOpticalflowContextCreate(ctx.get(), &d);
        report("ffxOpticalflowContextCreate [1920x1080]", rc);
        if (rc == FFX_OK)
            report("ffxOpticalflowContextDestroy", ffxOpticalflowContextDestroy(ctx.get()));
    }

    // ---- Frame interpolation ------------------------------------------------------
    {
        static const struct { uint32_t flags; const char* name; } cases[] = {
            {0, "default"},
            {FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEINTERPOLATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                 FFX_FRAMEINTERPOLATION_ENABLE_JITTER_MOTION_VECTORS | FFX_FRAMEINTERPOLATION_ENABLE_HDR_COLOR_INPUT,
             "DEPTH_INVERTED|DISPLAY_RES_MV|JITTER|HDR"},
        };
        auto ctx = std::make_unique<FfxFrameInterpolationContext>();
        for (const auto& c : cases) {
            FfxFrameInterpolationContextDescription d{};
            d.flags                             = c.flags;
            d.maxRenderSize                     = {1920, 1080};
            d.displaySize                       = {3840, 2160};
            d.backBufferFormat                  = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
            d.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
            d.backendInterface                  = iface;
            char label[160];
            std::snprintf(label, sizeof(label), "ffxFrameInterpolationContextCreate [%s]", c.name);
            rc = ffxFrameInterpolationContextCreate(ctx.get(), &d);
            report(label, rc);
            if (rc == FFX_OK)
                report("ffxFrameInterpolationContextDestroy", ffxFrameInterpolationContextDestroy(ctx.get()));
        }
    }

    // ---- Optional: record real dispatches (skip with FFX_SMOKE_NO_DISPATCH=1) -------
    if (!std::getenv("FFX_SMOKE_NO_DISPATCH")) {
        Gpu g{phys, device, VK_NULL_HANDLE, qfi, VK_NULL_HANDLE};
        vkGetDeviceQueue(device, qfi, 0, &g.queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfi;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &g.pool));
        dispatchTest(g, iface);
        vkDestroyCommandPool(device, g.pool, nullptr);
    }

    vkDeviceWaitIdle(device);
    std::free(scratch);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
