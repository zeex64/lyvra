# Lyvra

A low-latency capture card viewer for Linux, with FSR 1 / 3 / 4 upscaling, frame generation and a live latency/performance dashboard.

Licensed under GPL-3.0. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

![Viewer](docs/viewer.png)

## What it does

- **Low-latency capture.** V4L2 memory-mapped buffers on a dedicated thread. The render loop wakes on each new frame and always shows the newest one. Older undelivered frames are recycled, never queued.
- **No CPU colour conversion.** The raw NV12 / YUYV / I420 frame is copied once into GPU-visible memory (VRAM with resizable BAR), and a compute shader converts it to RGB (BT.709/BT.601, limited/full range).
- **FSR 1 upscaling.** AMD FidelityFX EASU + RCAS (MIT) as compute shaders, adjustable sharpness. It turns itself off when the output isn't larger than the input.
- **FSR 3.1 upscaling (experimental).** AMD FidelityFX SDK 1.1.4 (MIT), built natively on Linux. A capture feed has no engine motion vectors, so FidelityFX optical flow estimates them from the video. A small shader then expands them to per-pixel vectors, ignoring ±1 px noise. Two capture-specific fixes:
  - **Synthetic jitter.** The frame is resampled at a Halton sub-pixel offset each frame. Without it FSR 3 converges to blocky, stair-stepped edges.
  - **Reactive mask.** The previous frame is warped by each vector and compared with the current frame. Where they disagree (repetitive textures, particles, transparency, cuts), FSR is told to trust the current frame instead of history.
  - There's no real camera jitter, so expect steadier edges and less shimmer rather than new detail. Split view (C) shows FSR 1 and FSR 3 side by side on the same frame, and the motion debug view marks unreliable vectors with a checker.
- **FSR 4 upscaling (experimental).** AMD's ML upscaler (FSR 4.1) through [fsr4-vk](third_party/fsr4-vk), an independent Vulkan implementation (GPL-3.0). It uses the FP8 model on RDNA 4 and INT8 elsewhere, loaded at runtime via the FFX API. It's fed the same optical-flow motion, synthetic jitter and reactive mask as FSR 3. Handles bad motion vectors much better than FSR 3. In side-by-side gameplay tests it was clearly better than FSR 1 on dense foliage (defined blades instead of a painterly smear) and on par elsewhere, with no ghosting seen. About 3 ms GPU at 1080p to 4K, about 11 ms latency at 1080p60. The first switch to FSR 4 takes about half a second while the model loads.
- **FSR 3/4 quality row.** Chooses FSR's output size as a multiple of the capture resolution; the final composite then resizes it to the window. Options: Auto (window size), Native AA 1.0× (FSR as anti-aliasing, cheapest), Quality 1.5×, Balanced 1.7×, Performance 2.0×, Ultra Performance 3.0×. Measured with FSR 4 at 1080p: Native AA 1.8 ms / 9.9 ms latency, Quality 2.5 ms / 10.6 ms, Performance (4K) 3.2 ms / 11.4 ms.
- **Frame generation (2×, experimental).** FidelityFX frame interpolation (MIT) on the final frame at display resolution. It uses its own optical flow and motion vectors, so it works with Off, FSR 1 or FSR 3. Each new capture frame first shows the generated in-between frame; the real frame follows half a capture interval later, so pacing stays even. With Off or FSR 1 it generates at capture resolution before upscaling, and FSR 1 then upscales the generated frame as well. With FSR 3/4 it runs after the temporal upscaler, with optical flow at half resolution for outputs wider than 2560. Measured at 1080p60 to 4K: 120 frames/s shown, about +9 ms latency (16.7 vs 7.9 ms). GPU for upscale + frame gen: FSR 1 2.4 ms (was 5.2), FSR 4 Native AA 3.0 ms, FSR 4 at 4K 6.2 ms (was 8.2). The display needs at least 2× the capture rate. Press G to toggle. Two capture-specific protections:
  - **Fast-motion guard.** When more than 10% of the frame isn't explained by the motion vectors, or more than 25% of it moves faster than 3% of the screen width per frame, the real frame is shown instead (as AMD's AFMF does). Fast flicks drop to native smoothness instead of smearing.
  - **Static-pixel lock.** Pixels unchanged since the previous frame (game HUD text, crosshairs) keep their real value, since there's no HUD-less copy to work from. The anti-aliased edges of HUD text over very fast-changing content can still streak slightly.
- **Present modes.** Mailbox (default), Immediate, or VSync, with 2 frames in flight and the smallest swapchain each mode allows.
- **Audio passthrough.** Finds the capture card's audio input automatically and keeps the output queue within a hard limit (30 ms by default).
- **Stats.** A latency overlay and a Performance panel with a per-stage latency breakdown (USB transfer, queue, CPU, GPU convert / upscale / composite), frame-time graph, fps, dropped frames and signal path.
- **Device picker.** Capture cards are listed before webcams, the best mode is picked automatically, and your choice is remembered.

Measured on a DX20R (1080p120 NV12) with an RX 9070 XT: about 7.7 ms from capture timestamp to frame ready, 0 dropped frames, about 0.04 ms GPU without upscaling and about 0.5 ms with FSR 1 to 4K. Roughly 7.2 ms of that is the USB transfer of one frame itself. FSR 3.1 to 4K (including optical flow) costs about 0.6–2.3 ms GPU, about 9–10 ms total at 1080p60.

## Build

Needs: CMake ≥ 3.20, a C++20 compiler, SDL3, the Vulkan loader, `glslc` (shaderc) and `glslangValidator`. Dear ImGui, FSR 1, the FidelityFX SDK and the Vulkan headers are vendored in `third_party/`. The first build compiles about 250 FidelityFX shader variants (around 7 s on 32 threads).

**FSR 4 (optional).** Build fsr4-vk from `third_party/fsr4-vk` by following its README. You need AMD's `amd_fidelityfx_upscaler_dx12.dll` from FidelityFX SDK 2.3.0; its setup verifies the hash and extracts the model, and the result is `build/linux/amd_fidelityfx_upscaler_vk.so`. Then either configure Lyvra with `-DLYVRA_FSR4_VK_LIB=/path/to/amd_fidelityfx_upscaler_vk.so` (copied next to the binary and installed to `lib/lyvra/`), or put the file in `~/.local/share/lyvra/`, or set `LYVRA_FSR4_LIB`. Without it, FSR 4 is greyed out.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/lyvra
```

Your user must be in the `video` group to open `/dev/video*`.

### Options

| Flag | |
|---|---|
| `--device /dev/videoN` | Open this device |
| `--fullscreen` | Start fullscreen (this run only) |
| `--selftest N` | Run N seconds, printing live stats every second |
| `--screenshot file.ppm` | With `--selftest`, save the final composited frame |
| `--show settings[:tab] / stats / picker` | Open a panel at start |
| `--upscaler off / fsr1 / fsr3 / fsr4` | Override the upscaler for this run |
| `--quality auto / native / quality / balanced / performance / ultra` | Override the FSR 3/4 quality for this run |
| `--debug-motion` | Show the optical-flow motion field (FSR 3) |
| `--test-pattern` | Synthetic frames: left half moves right 4 px/frame, right half static |
| `--test-pattern=smooth` | Anti-aliased stripes and rings repeating every 960 px; top half moves 1.37 px/frame |
| `--compare` | Split view: FSR 1 left, FSR 3 right |
| `--no-jitter` | Disable FSR 3 synthetic jitter (for A/B) |
| `--framegen` | Turn frame generation on for this run |
| `--test-pattern=fast` | Blocks moving 150 px/frame (exercises the frame-gen guard) |
| `--no-fglock` | Disable the frame-gen static-pixel lock and guard (for A/B) |
| `LYVRA_VALIDATION=1` | Enable Vulkan validation layers |

### Shortcuts

F11/F fullscreen · Tab overlay · S settings · P performance · D device · M mute · U cycle Off / FSR 1 / FSR 3 / FSR 4 · C split view · G frame generation · F12 screenshot (a generated frame when frame gen is on) · Esc close · Ctrl+Q quit

Settings live in `~/.config/lyvra/settings.ini`.

## Layout

```
src/capture.*    V4L2 enumeration, mode selection, capture thread
src/renderer.*   Vulkan 1.3: YUV→RGB compute, FSR 1, composite, timestamps
src/temporal.*   FSR 3.1: FidelityFX optical flow -> motion vectors -> FSR 3.1 upscaler
src/framegen.*   Frame generation: optical flow -> motion vectors -> FI prepare -> FI dispatch
src/audio.*      SDL3 audio passthrough with bounded latency
src/ui.*         Dear ImGui UI (top/bottom bars, overlay, settings, performance, picker)
src/main.cpp     Event loop, device lifecycle, stats
shaders/         yuv.comp, easu.comp, rcas.comp, motion.comp, blit.vert/frag
cmake/           fidelityfx.cmake (Linux build of the SDK subset + shader generation), compat shims
tools/ffx_sc/    FidelityFX shader compiler ported to Linux (GLSL path)
tools/ffx_smoke/ Creates and dispatches the FidelityFX contexts headlessly
```

## Roadmap

- **Display-side latency** via `VK_KHR_present_wait`, so the number includes compositor and scanout.
- MJPEG input (libturbojpeg) for cheaper cards, HDR passthrough, and recording.

## Renaming

The name is set in `src/settings.h` (`APP_NAME`, `APP_ID`), in `CMakeLists.txt` (project and target) and in `data/lyvra.desktop`.
