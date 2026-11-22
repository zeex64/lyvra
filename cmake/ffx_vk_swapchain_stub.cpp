// Stub for the FidelityFX VK FrameInterpolationSwapchain (excluded from ffx_sdk).
//
// ffx_vk.cpp stores ffxSetFrameGenerationConfigToSwapchainVK in
// FfxInterface::fpSwapChainConfigureFrameGeneration. That callback is only invoked
// by the FSR3 wrapper component (ffx_fsr3.cpp), which is not built here; lyvra
// does its own presentation. The other FrameInterpolationSwapchain entry points
// declared in ffx_vk.h (ffxReplaceSwapchainForFrameinterpolationVK, ...) are
// intentionally left undefined so any accidental use fails at link time.
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* config)
{
    (void)config;
    return FFX_ERROR_INVALID_ARGUMENT;
}
