// NVIDIA Image Scaling: CPU-side config and coefficient tables (NIS_Config.h, MIT).
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "NIS_Config.h"

// NISConfig is padded to 256 bytes (D3D constant-buffer alignment); its fields are the 112 bytes nis.comp declares.
static_assert(offsetof(NISConfig, reserved1) + sizeof(float) == 112, "NISConfig fields must match nis.comp");

void nisScalerConfig(void* out112, float sharpness, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    NISConfig c{};
    NVScalerUpdateConfig(c, sharpness, 0, 0, inW, inH, inW, inH, 0, 0, outW, outH, outW, outH, NISHDRMode::None);
    std::memcpy(out112, &c, 112);
}

// 64 phases x 8 taps each, stored as a 2x64 RGBA32F texture (x = tap/4, y = phase).
const float* nisCoefScale() { return &coef_scale[0][0]; }
const float* nisCoefUsm() { return &coef_usm[0][0]; }
