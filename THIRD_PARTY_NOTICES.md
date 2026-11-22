# Third-party notices

Lyvra is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)).
It includes or loads the following third-party components, which keep their own licences:

| Component | Where | Licence |
|---|---|---|
| fsr4-vk (FSR 4.1 Vulkan implementation) | `third_party/fsr4-vk/`, loaded at runtime as `amd_fidelityfx_upscaler_vk.so` | GPL-3.0-only (its own code); AMD FSR 4 assets as described in `third_party/fsr4-vk/THIRD_PARTY_NOTICES.md` |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1 upscaler, optical flow, frame interpolation, Vulkan backend) | `third_party/fidelityfx/` | MIT |
| AMD FidelityFX FSR 1 | `third_party/fsr1/` | MIT |
| AMD FidelityFX API headers (SDK 2.x) | `third_party/ffx-api/` | MIT (FFX_API_ENTRY patched for non-Windows) |
| Dear ImGui 1.92.5 | `third_party/imgui/` | MIT |
| Vulkan-Headers 1.4.363 | `third_party/vulkan-headers/` | Apache-2.0 / MIT |
| Inter, JetBrains Mono fonts | `assets/fonts/` | SIL Open Font License 1.1 |
| SDL3 | system library | zlib |

The FSR 4 model weights and shaders used by fsr4-vk come from AMD's
`amd_fidelityfx_upscaler_dx12.dll` (FidelityFX SDK 2.3.0). That file is listed under the
MIT terms in AMD's SDK licence at commit `60f4ea8` (and on the current main branch). The
generated assets aren't stored in this repository; fsr4-vk's setup step extracts them
from the DLL at build time.
