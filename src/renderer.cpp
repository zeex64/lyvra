#include "renderer.h"

#include <SDL3/SDL_vulkan.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "capture.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"

void fsrEasuConstants(uint32_t con[16], float inW, float inH, float outW, float outH);
void fsrRcasConstants(uint32_t con[4], float sharpnessStops);
void casConstants(uint32_t con[8], float sharpness, float w, float h);
void nisScalerConfig(void* out112, float sharpness, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
const float* nisCoefScale();
const float* nisCoefUsm();

static const uint32_t kYuvSpv[] =
#include "yuv.comp.inc"
    ;
static const uint32_t kEasuSpv[] =
#include "easu.comp.inc"
    ;
static const uint32_t kRcasSpv[] =
#include "rcas.comp.inc"
    ;
static const uint32_t kBlitVertSpv[] =
#include "blit.vert.inc"
    ;
static const uint32_t kCasSpv[] =
#include "cas.comp.inc"
    ;
static const uint32_t kNisSpv[] =
#include "nis.comp.inc"
    ;
static const uint32_t kPredictSpv[] =
#include "predict.comp.inc"
    ;
static const uint32_t kBlitFragSpv[] =
#include "blit.frag.inc"
    ;

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) {                                                  \
            fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", r_, __FILE__, __LINE__, #x); \
            return false;                                                        \
        }                                                                        \
    } while (0)

static void imguiCheck(VkResult r) {
    if (r != VK_SUCCESS) fprintf(stderr, "ImGui Vulkan error %d\n", r);
}

// ---------------------------------------------------------------------------
bool Renderer::init(SDL_Window* window, PresentMode pm, std::string& err) {
    window_ = window;
    presentMode_ = pm;

    Uint32 extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    std::vector<const char*> exts(sdlExts, sdlExts + extCount);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = APP_NAME;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    const char* validation = "VK_LAYER_KHRONOS_validation";
    if (const char* v = getenv("LYVRA_VALIDATION"); v && *v == '1') {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &validation;
    }
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) { err = "Vulkan 1.3 instance creation failed"; return false; }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) { err = std::string("Surface: ") + SDL_GetError(); return false; }

    // Pick a GPU: Vulkan 1.3, push descriptors, a graphics+compute+present queue. Prefer discrete.
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    int bestScore = -1;
    for (auto pd : pds) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t ec = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> eps(ec);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, eps.data());
        auto has = [&](const char* e) { return std::any_of(eps.begin(), eps.end(), [&](auto& p) { return !strcmp(p.extensionName, e); }); };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qps(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qps.data());
        for (uint32_t i = 0; i < qc; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if (!present || !(qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                phys_ = pd;
                qfam_ = i;
                gpuName = props.deviceName;
                tsPeriod_ = props.limits.timestampPeriod;
                tsSupported_ = qps[i].timestampValidBits > 0 && props.limits.timestampComputeAndGraphics;
            }
            break;
        }
    }
    if (!phys_) { err = "No Vulkan 1.3 GPU with push descriptors and presentation support"; return false; }
    // Shorten "AMD Radeon RX 9070 XT (RADV GFX1201)" -> "AMD Radeon RX 9070 XT"
    if (auto p = gpuName.find(" ("); p != std::string::npos) gpuName = gpuName.substr(0, p);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Enable every feature the GPU supports (except the costly robustness ones). The FidelityFX
    // backend picks shader variants from *supported* features, and fsr4-vk (FSR 4) needs
    // descriptor buffers, buffer device addresses, 8/16-bit types, FP8 and cooperative matrices.
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    uint32_t dec = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &dec, nullptr);
    std::vector<VkExtensionProperties> deps(dec);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &dec, deps.data());
    auto hasExt = [&](const char* e) { return std::any_of(deps.begin(), deps.end(), [&](auto& p) { return !strcmp(p.extensionName, e); }); };
    for (const char* e : {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                          VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME})
        if (hasExt(e)) dexts.push_back(e);

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT ff8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    void** tail = &f13.pNext;
    auto chain = [&](void* s, void** next, const char* ext) { if (hasExt(ext)) { *tail = s; tail = next; } };
    chain(&fdb, &fdb.pNext, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    chain(&ff8, &ff8.pNext, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    chain(&fcm, &fcm.pNext, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    vkGetPhysicalDeviceFeatures2(phys_, &f2);  // query everything supported, then enable it as-is
    f2.features.robustBufferAccess = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    if (!f13.dynamicRendering || !f13.synchronization2) { err = "GPU lacks dynamic rendering / synchronization2"; return false; }
    fdb.descriptorBufferCaptureReplay = VK_FALSE;  // debugging-only features
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    if (vkCreateDevice(phys_, &dci, nullptr, &device_) != VK_SUCCESS) { err = "vkCreateDevice failed"; return false; }
    vkGetDeviceQueue(device_, qfam_, 0, &queue_);
    pushDesc_ = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device_, "vkCmdPushDescriptorSetKHR");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qfam_;
    vkCreateCommandPool(device_, &cpi, nullptr, &pool_);
    for (auto& f : frames_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &ai, &f.cmd);
        vkAllocateCommandBuffers(device_, &ai, &f.cmdAfter);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &si, nullptr, &f.acquired);
    }
    if (tsSupported_) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 4 * kFrames;
        vkCreateQueryPool(device_, &qpi, nullptr, &queries_);
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &sci, nullptr, &linear_);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(device_, &sci, nullptr, &nearest_);

    if (!createSwapchain()) { err = "Swapchain creation failed"; return false; }
    if (!createPipelines()) { err = "Pipeline creation failed"; return false; }
    std::string terr;
    if (!temporal_.init(instance_, phys_, device_, terr)) { temporalStatus = terr; fprintf(stderr, "FSR 3 unavailable: %s\n", terr.c_str()); }
    if (!fg_.init(phys_, device_, terr)) { frameGenStatus = terr; fprintf(stderr, "Frame generation unavailable: %s\n", terr.c_str()); }
    if (!a4k_.init(phys_, device_, terr)) fprintf(stderr, "Anime4K unavailable: %s\n", terr.c_str());
    if (!uploadNisCoefficients()) fprintf(stderr, "NIS unavailable: coefficient upload failed\n");
    return true;
}

void Renderer::initImGui() {
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = instance_;
    ii.PhysicalDevice = phys_;
    ii.Device = device_;
    ii.QueueFamily = qfam_;
    ii.Queue = queue_;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = minImageCount_;
    ii.ImageCount = uint32_t(swapImages_.size());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat_;
    ii.CheckVkResultFn = imguiCheck;
    ImGui_ImplVulkan_Init(&ii);
    imguiReady_ = true;
}

void Renderer::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    if (imguiReady_) { ImGui_ImplVulkan_Shutdown(); imguiReady_ = false; }
    temporal_.shutdown();
    fg_.shutdown();
    a4k_.shutdown();
    for (Image* i : {&cas_, &casG_, &nis_, &nisG_, &pred_, &coefScale_, &coefUsm_}) destroyImage(*i);
    destroyImage(src_);
    destroyImage(easu_);
    destroyImage(rcas_);
    destroyImage(easuG_);
    destroyImage(rcasG_);
    destroyBuffer(readback_);
    for (auto& f : frames_) {
        destroyBuffer(f.staging);
        vkDestroyFence(device_, f.fence, nullptr);
        vkDestroySemaphore(device_, f.acquired, nullptr);
    }
    for (VkPipeline p : {yuvPipe_, easuPipe_, rcasPipe_, blitPipe_, casPipe_, nisPipe_, predPipe_}) vkDestroyPipeline(device_, p, nullptr);
    vkDestroyPipelineLayout(device_, nisLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, nisSetLayout_, nullptr);
    vkDestroyPipelineLayout(device_, predLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, predSetLayout_, nullptr);
    for (VkPipelineLayout l : {yuvLayout_, fsrLayout_, blitLayout_}) vkDestroyPipelineLayout(device_, l, nullptr);
    for (VkDescriptorSetLayout l : {yuvSetLayout_, fsrSetLayout_, blitSetLayout_}) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    vkDestroySampler(device_, linear_, nullptr);
    vkDestroySampler(device_, nearest_, nullptr);
    destroySwapchainViews();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (queries_) vkDestroyQueryPool(device_, queries_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

// ---------------------------------------------------------------------------
static VkPresentModeKHR toVk(PresentMode m) {
    switch (m) {
        case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
        default: return VK_PRESENT_MODE_FIFO_KHR;
    }
}

const char* Renderer::presentModeName() const {
    switch (vkPresentMode_) {
        case VK_PRESENT_MODE_MAILBOX_KHR: return "Mailbox";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "Immediate";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
        default: return "VSync (FIFO)";
    }
}

void Renderer::setPresentMode(PresentMode pm) {
    if (pm == presentMode_) return;
    presentMode_ = pm;
    swapchainDirty_ = true;
}

void Renderer::destroySwapchainViews() {
    for (auto v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    swapViews_.clear();
    renderDone_.clear();
}

bool Renderer::createSwapchain() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    vkDeviceWaitIdle(device_);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    uint32_t pc = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pc, nullptr);
    std::vector<VkPresentModeKHR> modes(pc);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pc, modes.data());
    VkPresentModeKHR want = toVk(presentMode_);
    vkPresentMode_ = std::find(modes.begin(), modes.end(), want) != modes.end() ? want : VK_PRESENT_MODE_FIFO_KHR;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) {
        ext.width = std::clamp(uint32_t(pw), caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(uint32_t(ph), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    // Fewer images = less queued latency. Mailbox needs 3 to be non-blocking.
    uint32_t count = vkPresentMode_ == VK_PRESENT_MODE_MAILBOX_KHR ? 3 : 2;
    count = std::max(count, caps.minImageCount);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = vkPresentMode_;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &sc));
    destroySwapchainViews();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = sc;
    swapFormat_ = chosen.format;
    swapW = ext.width;
    swapH = ext.height;
    minImageCount_ = count;

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &ic, nullptr);
    swapImages_.resize(ic);
    vkGetSwapchainImagesKHR(device_, swapchain_, &ic, swapImages_.data());
    for (auto img : swapImages_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &v));
        swapViews_.push_back(v);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(device_, &si, nullptr, &s);
        renderDone_.push_back(s);
    }
    if (imguiReady_) ImGui_ImplVulkan_SetMinImageCount(minImageCount_);
    swapchainDirty_ = false;
    return true;
}

// ---------------------------------------------------------------------------
VkShaderModule Renderer::shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &ci, nullptr, &m);
    return m;
}

bool Renderer::createPipelines() {
    auto setLayout = [&](std::initializer_list<VkDescriptorType> types, VkShaderStageFlags stages) {
        std::vector<VkDescriptorSetLayoutBinding> b;
        uint32_t i = 0;
        for (auto t : types) b.push_back({i++, t, 1, stages, nullptr});
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ci.bindingCount = uint32_t(b.size());
        ci.pBindings = b.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &l);
        return l;
    };
    auto pipeLayout = [&](VkDescriptorSetLayout sl, uint32_t pcBytes, VkShaderStageFlags stages) {
        VkPushConstantRange r{stages, 0, pcBytes};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &sl;
        ci.pushConstantRangeCount = pcBytes ? 1 : 0;
        ci.pPushConstantRanges = &r;
        VkPipelineLayout l = VK_NULL_HANDLE;
        vkCreatePipelineLayout(device_, &ci, nullptr, &l);
        return l;
    };
    auto compute = [&](const uint32_t* code, size_t bytes, VkPipelineLayout layout) {
        VkShaderModule m = shader(code, bytes);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(device_, m, nullptr);
        return p;
    };

    yuvSetLayout_ = setLayout({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, VK_SHADER_STAGE_COMPUTE_BIT);
    fsrSetLayout_ = setLayout({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, VK_SHADER_STAGE_COMPUTE_BIT);
    blitSetLayout_ = setLayout({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, VK_SHADER_STAGE_FRAGMENT_BIT);
    yuvLayout_ = pipeLayout(yuvSetLayout_, 32, VK_SHADER_STAGE_COMPUTE_BIT);
    fsrLayout_ = pipeLayout(fsrSetLayout_, 64, VK_SHADER_STAGE_COMPUTE_BIT);
    blitLayout_ = pipeLayout(blitSetLayout_, 0, 0);
    yuvPipe_ = compute(kYuvSpv, sizeof kYuvSpv, yuvLayout_);
    easuPipe_ = compute(kEasuSpv, sizeof kEasuSpv, fsrLayout_);
    rcasPipe_ = compute(kRcasSpv, sizeof kRcasSpv, fsrLayout_);
    casPipe_ = compute(kCasSpv, sizeof kCasSpv, fsrLayout_);  // same bindings as FSR 1 (sampler + storage)
    {
        VkDescriptorSetLayoutBinding nb[5] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ci.bindingCount = 5;
        ci.pBindings = nb;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &nisSetLayout_);
        nisLayout_ = pipeLayout(nisSetLayout_, 112, VK_SHADER_STAGE_COMPUTE_BIT);
        nisPipe_ = compute(kNisSpv, sizeof kNisSpv, nisLayout_);
    }
    predSetLayout_ = setLayout({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                               VK_SHADER_STAGE_COMPUTE_BIT);
    predLayout_ = pipeLayout(predSetLayout_, 20, VK_SHADER_STAGE_COMPUTE_BIT);
    predPipe_ = compute(kPredictSpv, sizeof kPredictSpv, predLayout_);

    VkShaderModule vs = shader(kBlitVertSpv, sizeof kBlitVertSpv), fs = shader(kBlitFragSpv, sizeof kBlitFragSpv);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &swapFormat_;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = blitLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &blitPipe_));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return yuvPipe_ && easuPipe_ && rcasPipe_;
}

// ---------------------------------------------------------------------------
uint32_t Renderer::memType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (VkMemoryPropertyFlags f : {want, fallback})
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    return 0;
}

bool Renderer::createImage(Image& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    destroyImage(img);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &img.mem));
    vkBindImageMemory(device_, img.image, img.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &img.view));
    img.w = w;
    img.info = ci;
    img.h = h;
    return true;
}

void Renderer::destroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.mem) vkFreeMemory(device_, img.mem, nullptr);
    img = {};
}

bool Renderer::createBuffer(Buffer& b, VkDeviceSize size) {
    destroyBuffer(b);
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device_, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    // With resizable BAR the GPU reads the frame from its own VRAM; otherwise over PCIe.
    ai.memoryTypeIndex = memType(req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &b.mem));
    vkBindBufferMemory(device_, b.buf, b.mem, 0);
    VK_CHECK(vkMapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    b.size = size;
    return true;
}

void Renderer::destroyBuffer(Buffer& b) {
    if (b.map) vkUnmapMemory(device_, b.mem);
    if (b.buf) vkDestroyBuffer(device_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(device_, b.mem, nullptr);
    b = {};
}

void Renderer::barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void Renderer::pushImageSampler(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                VkImageView sampled, VkSampler sampler, VkImageView storage, VkImageLayout sampledLayout) {
    VkDescriptorImageInfo in{sampler, sampled, sampledLayout};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &in;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &out;
    pushDesc_(cmd, bp, layout, 0, storage ? 2 : 1, w);
}

// NIS needs its 64-phase x 8-tap filter tables as 2x64 RGBA32F textures; uploaded once.
bool Renderer::uploadNisCoefficients() {
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!createImage(coefScale_, 2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, u) || !createImage(coefUsm_, 2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, u)) return false;
    const VkDeviceSize bytes = 2 * 64 * 4 * sizeof(float);
    VkBuffer staging;
    VkDeviceMemory mem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes * 2;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &staging) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, staging, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    vkAllocateMemory(device_, &ai, nullptr, &mem);
    vkBindBufferMemory(device_, staging, mem, 0);
    void* map = nullptr;
    vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &map);
    memcpy(map, nisCoefScale(), bytes);
    memcpy(static_cast<uint8_t*>(map) + bytes, nisCoefUsm(), bytes);
    vkUnmapMemory(device_, mem);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    for (int i = 0; i < 2; i++) {
        Image& img = i == 0 ? coefScale_ : coefUsm_;
        barrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = i * bytes;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {2, 64, 1};
        vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, mem, nullptr);
    return true;
}

Rect Renderer::fitRect(Rect a, uint32_t sw, uint32_t sh, ScaleMode m) {
    if (!sw || !sh || a.w <= 0 || a.h <= 0 || m == ScaleMode::Stretch) return a;
    float s = std::min(a.w / sw, a.h / sh);
    if (m == ScaleMode::Integer && s >= 1.0f) s = std::floor(s);
    Rect r;
    r.w = std::round(sw * s);
    r.h = std::round(sh * s);
    r.x = std::round(a.x + (a.w - r.w) * 0.5f);
    r.y = std::round(a.y + (a.h - r.h) * 0.5f);
    return r;
}

// ---------------------------------------------------------------------------
bool Renderer::render(const FrameInput* in, const RenderParams& p, ImDrawData* dd) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    if (swapchainDirty_ || uint32_t(pw) != swapW || uint32_t(ph) != swapH)
        if (!createSwapchain()) return false;

    lastRenderStartNs = nowNs();
    Frame& f = frames_[cur_];
    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);

    if (f.queried && queries_) {
        uint64_t ts[4];
        if (vkGetQueryPoolResults(device_, queries_, cur_ * 4, 4, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            auto ms = [&](uint64_t a, uint64_t b) { return float(double(b - a) * tsPeriod_ / 1e6); };
            gpu = {ms(ts[0], ts[1]), ms(ts[1], ts[2]), ms(ts[2], ts[3]), ms(ts[0], ts[3]), true};
            if (f.hadInput) gpuWork = gpu;
        }
    }

    uint32_t imgIdx = 0;
    VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imgIdx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { swapchainDirty_ = true; return false; }
    if (ar == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
    vkResetFences(device_, 1, &f.fence);

    // (Re)allocate input resources if the capture format changed.
    if (in) {
        size_t need = (std::max(in->bytes, size_t(in->stride) * in->height * 2) + 3) & ~size_t(3);
        if (in->width != src_.w || in->height != src_.h || need > stagingSize_) {
            vkDeviceWaitIdle(device_);
            createImage(src_, in->width, in->height);
            for (auto& fr : frames_) createBuffer(fr.staging, need);
            stagingSize_ = need;
        }
        memcpy(f.staging.map, in->data, std::min(in->bytes, size_t(f.staging.size)));
        inW = in->width;
        inH = in->height;
        inFourcc_ = in->fourcc;
    }

    Rect dst = fitRect(p.videoArea, inW, inH, p.scale);
    const uint32_t dstW = uint32_t(std::max(1.0f, dst.w)), dstH = uint32_t(std::max(1.0f, dst.h));
    const bool haveInput = inputValid_ || in;
    const bool wantTemporal = p.upscaler == Upscaler::FSR3 || p.upscaler == Upscaler::FSR4;
    // FSR 3/4 output size: the window ("Auto") or a fixed multiple of the capture resolution
    // (Native AA 1.0x, Quality 1.5x, Balanced 1.7x, Performance 2.0x, Ultra Performance 3.0x),
    // resized to the window by the final composite.
    static const float kQualityRatio[] = {0.0f, 1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    const int q = std::clamp(p.fsrQuality, 0, 5);
    uint32_t tW = dstW, tH = dstH;
    if (q > 0 && inW && inH) {
        tW = std::min(7680u, uint32_t(std::lround(inW * kQualityRatio[q])));
        tH = std::min(4320u, uint32_t(std::lround(inH * kQualityRatio[q])));
    }
    const bool canUpscale = haveInput && dstW > inW && dstH > inH;
    const bool temporalOk = haveInput && (q > 0 || canUpscale);
    effectiveUpscaler = Upscaler::Off;
    if (temporalOk && wantTemporal && temporal_.available()) {
        std::string terr;
        if (temporal_.ensure(inW, inH, tW, tH, p.upscaler == Upscaler::FSR4, terr)) {
            effectiveUpscaler = p.upscaler;
            temporalStatus.clear();
        } else {
            temporalStatus = terr;
        }
    }
    // Spatial upscalers: NIS (output = window size) and Anime4K (fixed 2x, resized to the window).
    if (effectiveUpscaler == Upscaler::Off && canUpscale && p.upscaler == Upscaler::NIS && nisAvailable()) {
        if (nis_.w != dstW || nis_.h != dstH) {
            vkDeviceWaitIdle(device_);
            createImage(nis_, dstW, dstH);
            spatialDirty_ = true;
        }
        effectiveUpscaler = Upscaler::NIS;
    }
    if (effectiveUpscaler == Upscaler::Off && canUpscale && p.upscaler == Upscaler::Anime4K && a4k_.available()) {
        std::string aerr;
        if (a4k_.ensure(p.anime4kVariant, inW, inH, aerr)) {
            effectiveUpscaler = Upscaler::Anime4K;
            temporalStatus.clear();
        } else {
            temporalStatus = aerr;
        }
    }
    if (effectiveUpscaler == Upscaler::Off && canUpscale && p.upscaler != Upscaler::Off) effectiveUpscaler = Upscaler::FSR1;
    // FSR 3/4 only re-run on a new capture frame; otherwise the last output is reused.
    if (in) temporalInputFresh_ = true;
    const bool temporalMode = effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4;
    const bool a4kMode = effectiveUpscaler == Upscaler::Anime4K, nisMode = effectiveUpscaler == Upscaler::NIS;
    const bool postMode = temporalMode || a4kMode || nisMode;  // frame gen runs after these upscalers
    outW = temporalMode ? tW : a4kMode ? a4k_.outW() : dstW;
    outH = temporalMode ? tH : a4kMode ? a4k_.outH() : dstH;
    comparing = p.compare && postMode && !p.debugMotion && canUpscale;
    lastDst = dst;
    // CAS: sharpen at native resolution whenever nothing upscales (Upscaling off, or the window isn't bigger).
    const bool casMode = effectiveUpscaler == Upscaler::Off && p.sharpness > 0.0f && haveInput && inW && inH;
    // Frame generation runs at capture resolution, before upscaling (about 4x cheaper than at 4K).
    // The generated frame then gets the same spatial upscaler; FSR 1 stands in for FSR 3/4, whose
    // history must only ever see real frames. Predict mode only computes motion here.
    (void)postMode;
    bool fgThis = p.frameGen && fg_.available() && in && !p.debugMotion && !comparing;
    const bool fgPredict = fgThis && p.fgPredict;
    const bool fgPre = fgThis && !fgPredict;  // a generated frame is made (and upscaled) this frame
    const bool needFsr1 = canUpscale && (effectiveUpscaler == Upscaler::FSR1 || comparing);
    if (needFsr1 && (easu_.w != dstW || easu_.h != dstH)) {
        vkDeviceWaitIdle(device_);
        createImage(easu_, dstW, dstH);
        createImage(rcas_, dstW, dstH);
        upscaleDirty_ = true;
    }
    if (fgPre && canUpscale && (effectiveUpscaler == Upscaler::FSR1 || temporalMode) && (easuG_.w != dstW || easuG_.h != dstH)) {
        vkDeviceWaitIdle(device_);
        createImage(easuG_, dstW, dstH);
        createImage(rcasG_, dstW, dstH);
    }
    if (casMode && (cas_.w != inW || cas_.h != inH)) {
        vkDeviceWaitIdle(device_);
        createImage(cas_, inW, inH);
        spatialDirty_ = true;
    }
    if (casMode && fgPre && (casG_.w != inW || casG_.h != inH)) {
        vkDeviceWaitIdle(device_);
        createImage(casG_, inW, inH);
    }
    if (nisMode && fgPre && (nisG_.w != dstW || nisG_.h != dstH)) {
        vkDeviceWaitIdle(device_);
        createImage(nisG_, dstW, dstH);
    }
    // Predict mode, between real frames: warp the last real frame forward along its motion.
    predictedShown = false;
    const bool doPredict = !in && p.predictFrame && p.frameGen && p.fgPredict && predictReady_ && inputValid_ && !comparing && !p.debugMotion;
    if (doPredict && (pred_.w != dstW || pred_.h != dstH)) {
        vkDeviceWaitIdle(device_);
        createImage(pred_, dstW, dstH);
    }
    if (in) predictReady_ = false;
    if (p.sharpness != lastSharpness_) { lastSharpness_ = p.sharpness; upscaleDirty_ = true; spatialDirty_ = true; }
    if (in) spatialDirty_ = true;

    generatedShown = false;
    bool afterRecorded = false;  // f.cmdAfter holds work to submit after present
    if (fgThis) {
        std::string ferr;
        if (fg_.ensure(in->width, in->height, p.fgFlowScale, ferr)) frameGenStatus.clear();
        else { frameGenStatus = ferr; fgThis = false; }
    }

    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (queries_) {
        vkCmdResetQueryPool(cmd, queries_, cur_ * 4, 4);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, cur_ * 4 + 0);
    }

    const VkPipelineStageFlags2 kReaders = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

    // 1) Raw frame -> RGBA
    if (in) {
        barrier(cmd, src_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, yuvPipe_);
        VkDescriptorBufferInfo bufInfo{f.staging.buf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, src_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bufInfo;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &imgInfo;
        pushDesc_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, yuvLayout_, 0, 2, w);
        uint32_t fmt = in->fourcc == V4L2_PIX_FMT_NV12 ? 0 : in->fourcc == V4L2_PIX_FMT_YUYV ? 1 : in->fourcc == V4L2_PIX_FMT_YUV422P ? 3 : 2;
        uint32_t ySize = in->stride * in->height;
        uint32_t cSize = fmt == 3 ? ySize / 2 : ySize / 4;  // one chroma plane
        uint32_t pc[8] = {in->width, in->height, in->stride, fmt, ySize, ySize + cSize, in->fullRange ? 1u : 0u, in->bt601 ? 1u : 0u};
        vkCmdPushConstants(cmd, yuvLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cmd, (in->width + 15) / 16, (in->height + 15) / 16, 1);
        barrier(cmd, src_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        inputValid_ = true;
        upscaleDirty_ = true;
    }
    if (queries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, cur_ * 4 + 1);

    // 2a) FSR 3.1: optical flow -> motion vectors -> temporal upscale
    showingMotion = false;
    if ((effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4) && inputValid_ && temporalInputFresh_) {
        TemporalParams tp;
        tp.colorImage = src_.image;
        tp.colorInfo = src_.info;
        tp.colorView = src_.view;
        tp.jitter = p.jitter;
        tp.fsr4 = effectiveUpscaler == Upscaler::FSR4;
        tp.frameTimeMs = p.frameTimeMs;
        tp.sharpness = p.sharpness;
        tp.debugMotion = p.debugMotion;
        if (!temporal_.record(cmd, tp)) {
            temporalStatus = temporal_.lastError;
            effectiveUpscaler = Upscaler::FSR1;
            upscaleDirty_ = true;
            if (easu_.w != outW || easu_.h != outH) effectiveUpscaler = Upscaler::Off;  // FSR 1 images not ready this frame
            comparing = false;
        }
        temporalInputFresh_ = false;
    }
    if (effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4) showingMotion = p.debugMotion;

    // 2b) FSR 1: EASU upscale + RCAS sharpen (helper: also used on generated frames)
    auto runFsr1 = [&](VkImageView srcView, VkImageLayout srcLayout, Image& e, Image& r) {
        uint32_t con[16] = {};
        fsrEasuConstants(con, float(inW), float(inH), float(e.w), float(e.h));
        barrier(cmd, e.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, easuPipe_);
        pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsrLayout_, srcView, linear_, e.view, srcLayout);
        vkCmdPushConstants(cmd, fsrLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof con, con);
        vkCmdDispatch(cmd, (e.w + 15) / 16, (e.h + 15) / 16, 1);
        barrier(cmd, e.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        uint32_t rc[16] = {};
        fsrRcasConstants(rc, (1.0f - std::clamp(p.sharpness, 0.0f, 1.0f)) * 2.0f);  // 0 stops = sharpest
        barrier(cmd, r.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rcasPipe_);
        pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsrLayout_, e.view, nearest_, r.view);
        vkCmdPushConstants(cmd, fsrLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rc, rc);
        vkCmdDispatch(cmd, (r.w + 15) / 16, (r.h + 15) / 16, 1);
        barrier(cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    if (needFsr1 && inputValid_ && upscaleDirty_ && easu_.w == dstW) {
        runFsr1(src_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, easu_, rcas_);
        upscaleDirty_ = false;
    }

    // 2b-2) CAS native sharpening, NIS, Anime4K
    auto runCas = [&](VkImageView srcView, VkImageLayout srcLayout, Image& outImg) {
        uint32_t con[16] = {};
        casConstants(con, std::clamp(p.sharpness, 0.0f, 1.0f), float(outImg.w), float(outImg.h));
        barrier(cmd, outImg.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, casPipe_);
        pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsrLayout_, srcView, nearest_, outImg.view, srcLayout);
        vkCmdPushConstants(cmd, fsrLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof con, con);
        vkCmdDispatch(cmd, (outImg.w + 15) / 16, (outImg.h + 15) / 16, 1);
        barrier(cmd, outImg.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    if (casMode && inputValid_ && spatialDirty_ && cas_.w == inW) runCas(src_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cas_);
    auto runNis = [&](VkImageView srcView, VkImageLayout srcLayout, Image& outImg) {
        uint8_t cfg[112];
        nisScalerConfig(cfg, std::clamp(p.sharpness, 0.0f, 1.0f), inW, inH, outImg.w, outImg.h);
        barrier(cmd, outImg.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nisPipe_);
        VkDescriptorImageInfo smp{linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorImageInfo inp{VK_NULL_HANDLE, srcView, srcLayout};
        VkDescriptorImageInfo outp{VK_NULL_HANDLE, outImg.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo cs{VK_NULL_HANDLE, coefScale_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo cu{VK_NULL_HANDLE, coefUsm_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo* infos[5] = {&smp, &inp, &outp, &cs, &cu};
        const VkDescriptorType types[5] = {VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
        VkWriteDescriptorSet nw[5];
        for (int i = 0; i < 5; i++) {
            nw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            nw[i].dstBinding = uint32_t(i);
            nw[i].descriptorCount = 1;
            nw[i].descriptorType = types[i];
            nw[i].pImageInfo = infos[i];
        }
        pushDesc_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nisLayout_, 0, 5, nw);
        vkCmdPushConstants(cmd, nisLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cfg, cfg);
        vkCmdDispatch(cmd, (outImg.w + 31) / 32, (outImg.h + 23) / 24, 1);  // NIS_BLOCK_WIDTH x NIS_BLOCK_HEIGHT
        barrier(cmd, outImg.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    if (nisMode && inputValid_ && spatialDirty_) runNis(src_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nis_);
    if (a4kMode && inputValid_ && in) a4k_.record(cmd, src_.view);
    spatialDirty_ = false;

    // 2c) Frame generation: frame halfway between the previous and this one
    VkImageView genView = VK_NULL_HANDLE;
    VkImageLayout genLayout = VK_IMAGE_LAYOUT_GENERAL;
    bool genNearest = false;
    if (fgThis && inputValid_) {
        FrameGenParams fp;
        fp.color = src_.image;  // capture resolution, before upscaling
        fp.colorInfo = src_.info;
        fp.colorView = src_.view;
        fp.lockStatic = p.fgLock;
        fp.frameTimeMs = p.frameTimeMs;
        fp.predict = fgPredict;
        fp.reset = fgPredict != lastPredict_;  // switching modes: the other path's history is stale
        lastPredict_ = fgPredict;
        const bool tempNow = effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4;
        bool sizeOk = fp.colorInfo.extent.width == fg_.width() && fp.colorInfo.extent.height == fg_.height();
        // Predict mode: the motion is only needed for the predicted frame half a frame later, so it is
        // recorded into a second command buffer submitted after this frame is presented (the real
        // frame doesn't wait for it).
        VkCommandBuffer fgCmd = cmd;
        if (sizeOk && fgPredict) {
            VkCommandBufferBeginInfo abi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            abi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(f.cmdAfter, &abi);
            fgCmd = f.cmdAfter;
        }
        const bool recorded = sizeOk && fg_.record(fgCmd, fp);
        if (fgCmd != cmd) {
            vkEndCommandBuffer(f.cmdAfter);
            afterRecorded = true;
        }
        if (recorded && fgPredict) {
            predictReady_ = true;  // the real frame is shown now; the predicted one follows
        } else if (recorded) {
            generatedShown = true;
            if ((effectiveUpscaler == Upscaler::FSR1 || tempNow) && canUpscale && easuG_.w == dstW && easuG_.h == dstH) {
                runFsr1(fg_.outputView(), VK_IMAGE_LAYOUT_GENERAL, easuG_, rcasG_);  // upscale the generated frame too
                genView = rcasG_.view;
                genLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                genNearest = true;
            } else if (nisMode && nisG_.w == dstW && nisG_.h == dstH) {
                runNis(fg_.outputView(), VK_IMAGE_LAYOUT_GENERAL, nisG_);
                genView = nisG_.view;
                genLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                genNearest = true;
            } else if (a4kMode) {
                a4k_.record(cmd, fg_.outputView(), VK_IMAGE_LAYOUT_GENERAL, 1);
                genView = a4k_.outputView(1);
                genLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                genNearest = a4k_.outW() == dstW && a4k_.outH() == dstH;
            } else if (casMode && casG_.w == inW) {
                runCas(fg_.outputView(), VK_IMAGE_LAYOUT_GENERAL, casG_);  // sharpen it like the real frames
                genView = casG_.view;
                genLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else {
                genView = fg_.outputView();
                genNearest = fg_.width() == dstW && fg_.height() == dstH;
            }
        } else if (!sizeOk) frameGenStatus = "upscaler changed this frame";
        else frameGenStatus = fg_.lastError;
    }
    // 2d) Predict mode: last real frame (as shown, after upscaling) warped forward half a frame
    if (doPredict && pred_.w == dstW && pred_.h == dstH) {
        VkImageLayout rl = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageView rv = src_.view;
        if (effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4) { rv = temporal_.outputView(); rl = VK_IMAGE_LAYOUT_GENERAL; }
        else if (nisMode) rv = nis_.view;
        else if (a4kMode) rv = a4k_.outputView(0);
        else if (effectiveUpscaler == Upscaler::FSR1) rv = rcas_.view;
        else if (casMode && cas_.w == inW) rv = cas_.view;
        barrier(cmd, pred_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, predPipe_);
        VkDescriptorImageInfo ri{linear_, rv, rl};
        VkDescriptorImageInfo mi{linear_, fg_.predictMotionView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo oi{VK_NULL_HANDLE, pred_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo* pinfo[3] = {&ri, &mi, &oi};
        VkWriteDescriptorSet pw[3];
        for (int i = 0; i < 3; i++) {
            pw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            pw[i].dstBinding = uint32_t(i);
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            pw[i].pImageInfo = pinfo[i];
        }
        pushDesc_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, predLayout_, 0, 3, pw);
        struct { uint32_t w, h; float fgW, fgH, amount; } ppc{pred_.w, pred_.h, float(fg_.width()), float(fg_.height()), 0.5f};
        vkCmdPushConstants(cmd, predLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ppc, &ppc);
        vkCmdDispatch(cmd, (pred_.w + 7) / 8, (pred_.h + 7) / 8, 1);
        barrier(cmd, pred_.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        predictedShown = true;
        predictReady_ = false;  // one predicted frame per real frame
    }
    if (queries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, cur_ * 4 + 2);

    // 3) Composite video + UI into the swapchain image
    VkImage simg = swapImages_[imgIdx];
    barrier(cmd, simg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = swapViews_[imgIdx];
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{0.027f, 0.031f, 0.039f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {swapW, swapH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    if (inputValid_ && dst.w >= 1 && dst.h >= 1) {
        VkViewport vp{dst.x, dst.y, dst.w, dst.h, 0.0f, 1.0f};
        int sx = std::max(0, int(dst.x)), sy = std::max(0, int(dst.y));
        int ex = std::min(int(swapW), int(dst.x + dst.w)), ey = std::min(int(swapH), int(dst.y + dst.h));
        if (ex > sx && ey > sy) {
            VkRect2D sc{{sx, sy}, {uint32_t(ex - sx), uint32_t(ey - sy)}};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipe_);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            if (generatedShown && genView)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, genView, genNearest ? nearest_ : linear_, VK_NULL_HANDLE, genLayout);
            else if (showingMotion)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, temporal_.debugView(), linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
            else if (predictedShown)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, pred_.view, nearest_, VK_NULL_HANDLE);
            else if (effectiveUpscaler == Upscaler::FSR3 || effectiveUpscaler == Upscaler::FSR4)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, temporal_.outputView(),
                                 (tW == dstW && tH == dstH) ? nearest_ : linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
            else if (nisMode)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, nis_.view, nearest_, VK_NULL_HANDLE);
            else if (a4kMode)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, a4k_.outputView(),
                                 (a4k_.outW() == dstW && a4k_.outH() == dstH) ? nearest_ : linear_, VK_NULL_HANDLE);
            else if (effectiveUpscaler == Upscaler::FSR1)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, rcas_.view, nearest_, VK_NULL_HANDLE);
            else if (casMode && cas_.w == inW)
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, cas_.view, linear_, VK_NULL_HANDLE);
            else
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, src_.view, linear_, VK_NULL_HANDLE);
            if (comparing) {
                // Right half already bound (FSR 3); draw it, then the left half with FSR 1.
                int mid = int(dst.x + dst.w / 2);
                VkRect2D right{{std::max(sx, mid), sy}, {uint32_t(std::max(0, ex - std::max(sx, mid))), uint32_t(ey - sy)}};
                VkRect2D left{{sx, sy}, {uint32_t(std::max(0, std::min(ex, mid) - sx)), uint32_t(ey - sy)}};
                vkCmdSetScissor(cmd, 0, 1, &right);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                pushImageSampler(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, rcas_.view, nearest_, VK_NULL_HANDLE);
                vkCmdSetScissor(cmd, 0, 1, &left);
            }
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }
    if (dd) ImGui_ImplVulkan_RenderDrawData(dd, cmd);
    vkCmdEndRendering(cmd);
    bool shot = !screenshotPath_.empty();
    if (shot) {
        VkDeviceSize need = VkDeviceSize(swapW) * swapH * 4;
        if (readback_.size < need) {
            destroyBuffer(readback_);
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = need;
            ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(device_, &ci, nullptr, &readback_.buf);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device_, readback_.buf, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(device_, &ai, nullptr, &readback_.mem);
            vkBindBufferMemory(device_, readback_.buf, readback_.mem, 0);
            vkMapMemory(device_, readback_.mem, 0, VK_WHOLE_SIZE, 0, &readback_.map);
            readback_.size = need;
        }
        barrier(cmd, simg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {swapW, swapH, 1};
        vkCmdCopyImageToBuffer(cmd, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buf, 1, &region);
        barrier(cmd, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        barrier(cmd, simg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    if (queries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, cur_ * 4 + 3);
    vkEndCommandBuffer(cmd);

    // Compute work doesn't wait for the swapchain image; only the composite does.
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[imgIdx];
    // With work after present, the fence goes on that later submission; a fence signal covers
    // everything earlier in submission order, so it still guards this one too.
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, afterRecorded ? VK_NULL_HANDLE : f.fence));
    f.queried = queries_ != VK_NULL_HANDLE;
    f.hadInput = in != nullptr;
    lastSubmitNs = nowNs();

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imgIdx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imgIdx;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
    lastPresentReturnNs = nowNs();
    if (afterRecorded) {
        VkSubmitInfo ai{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        ai.commandBufferCount = 1;
        ai.pCommandBuffers = &f.cmdAfter;
        VK_CHECK(vkQueueSubmit(queue_, 1, &ai, f.fence));
    }
    if (shot) {
        vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        writeScreenshot();
        screenshotPath_.clear();
    }
    cur_ = (cur_ + 1) % kFrames;
    return true;
}

// Writes the last composited frame (video + UI) as a binary PPM.
bool Renderer::writeScreenshot() {
    const std::string& path = screenshotPath_;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".bmp") == 0) {
        bool bgr = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB;
        SDL_Surface* s = SDL_CreateSurfaceFrom(int(swapW), int(swapH), bgr ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32, readback_.map, int(swapW * 4));
        bool ok = s && SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
        if (ok) savedScreenshot = path;
        else fprintf(stderr, "Screenshot failed: %s\n", SDL_GetError());
        return ok;
    }
    FILE* fp = fopen(screenshotPath_.c_str(), "wb");
    if (!fp) return false;
    fprintf(fp, "P6\n%u %u\n255\n", swapW, swapH);
    const uint8_t* src = static_cast<const uint8_t*>(readback_.map);
    bool bgr = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<uint8_t> row(size_t(swapW) * 3);
    for (uint32_t y = 0; y < swapH; y++) {
        const uint8_t* s = src + size_t(y) * swapW * 4;
        for (uint32_t x = 0; x < swapW; x++) {
            row[x * 3 + 0] = s[x * 4 + (bgr ? 2 : 0)];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + (bgr ? 0 : 2)];
        }
        fwrite(row.data(), 1, row.size(), fp);
    }
    fclose(fp);
    fprintf(stderr, "Saved screenshot %s (%ux%u)\n", screenshotPath_.c_str(), swapW, swapH);
    return true;
}
