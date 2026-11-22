# AMD FidelityFX SDK 1.1.4 (trimmed)

Subset of https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK at tag `v1.1.4`
(commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55), MIT licensed (see `sdk/LICENSE.txt`).

Only what Lyvra builds is kept: `sdk/include`, `sdk/src/shared`, `sdk/src/backends/shared`,
`sdk/src/backends/vk/ffx_vk.cpp`, the fsr3upscaler / opticalflow / frameinterpolation components
and their GLSL shaders, and the shader compiler's MD5, SPIRV-Reflect and tiny-process-library.
Files are unmodified; Linux fixes live in `cmake/` and `tools/ffx_sc/`.
