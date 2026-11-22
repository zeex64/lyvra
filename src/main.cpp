// Lyvra: a low-latency capture card viewer for Linux.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "ui.h"

static Uint32 gFrameEvent = 0;
static std::atomic<bool> gFrameEventPending{false};

// ---- App actions ------------------------------------------------------------
void App::refreshDevices() {
    devices = enumerateDevices();
    audioInputs = AudioPassthrough::inputs();
    audioOutputs = AudioPassthrough::outputs();
    for (int i = 0; i < int(devices.size()); i++)
        if (devices[i].path == (capture.isOpen() ? capture.path : settings.devicePath)) pickerSelection = i;
}

const DeviceInfo* App::currentDevice() const {
    for (auto& d : devices) if (capture.isOpen() && d.path == capture.path) return &d;
    return nullptr;
}

bool App::signalOk() const {
    uint64_t last = capture.lastFrameNs.load();
    return capture.isOpen() && last && nowNs() - last < 500'000'000ull;
}

void App::showToast(const std::string& msg) {
    toast = msg;
    toastUntilNs = nowNs() + 3'000'000'000ull;
}

bool App::openDevice(const std::string& path, const VideoMode* requested) {
    const DeviceInfo* dev = nullptr;
    for (auto& d : devices) if (d.path == path) dev = &d;
    if (!dev) { showToast("Device not found: " + path); return false; }

    VideoMode mode;
    if (requested) mode = *requested;
    else if (settings.width && settings.devicePath == path) {
        mode = {settings.fourcc, settings.width, settings.height, settings.fpsNum, settings.fpsDen ? settings.fpsDen : 1};
    } else mode = pickBestMode(dev->modes);

    capture.close();
    renderer.clearInput();
    std::string err;
    if (!capture.open(path, mode, err)) {
        showToast(err);
        capture.failed = true;
        capture.failReason = err;
        return false;
    }
    activeMode = {capture.fourcc, capture.width, capture.height, mode.fpsNum, mode.fpsDen};
    settings.devicePath = path;
    if (requested) {
        settings.fourcc = mode.fourcc;
        settings.width = mode.width;
        settings.height = mode.height;
        settings.fpsNum = mode.fpsNum;
        settings.fpsDen = mode.fpsDen;
    }
    settings.save();
    stats.reset();
    restartAudio();
    showToast(capture.name + " · " + activeMode.label());
    return true;
}

void App::closeDevice() {
    capture.close();
    audio.stop();
    renderer.clearInput();
}

void App::restartAudio() {
    audio.stop();
    audioError.clear();
    if (!settings.audioEnabled || !capture.isOpen()) return;
    if (!audio.start(settings.audioInput, capture.name, settings.audioOutput, audioError)) {
        fprintf(stderr, "audio: %s\n", audioError.c_str());
        return;
    }
    audio.setVolume(settings.volume, settings.muted);
    audio.setMaxLatencyMs(settings.audioMaxMs);
}

void App::toggleFullscreen() {
    SDL_SetWindowFullscreen(window, !fullscreen);
}

// ---- Main -------------------------------------------------------------------
int main(int argc, char** argv) {
    // Small audio device buffers; SDL picks the PipeWire/Pulse/ALSA backend.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");
    SDL_SetHint(SDL_HINT_APP_ID, APP_ID);
    SDL_SetHint(SDL_HINT_APP_NAME, APP_NAME);
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    gFrameEvent = SDL_RegisterEvents(1);

    static App app;  // static: lives past main's locals, joined/destroyed in order below
    app.settings.load();
    int selftestSec = 0;
    const Upscaler savedUpscaler = app.settings.upscaler;  // CLI overrides below are for this run only
    const bool savedFrameGen = app.settings.frameGen, savedPredict = app.settings.fgPredict;
    const int savedFlow = app.settings.fgFlowScale;
    bool cliPredict = false, cliFlow = false;
    bool cliUpscaler = false, cliFrameGen = false;
    const int savedQuality = app.settings.fsrQuality;
    bool cliQuality = false;
    const float savedSharpness = app.settings.sharpness;
    const int savedA4k = app.settings.anime4kVariant;
    bool cliSharpness = false, cliA4k = false;
    bool cliFullscreen = false;
    bool noJitter = false, noFgLock = false;
    int testPattern = 0;  // synthetic frames with known motion: 1 = blocks, 2 = smooth
    std::string screenshotPath, showPanel, shotOnGenerated;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) app.settings.devicePath = argv[++i];
        else if (!strcmp(argv[i], "--fullscreen")) cliFullscreen = true;
        else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftestSec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshotPath = argv[++i];  // taken at the end of --selftest
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) showPanel = argv[++i];  // settings[:tab] | stats | picker
        else if (!strcmp(argv[i], "--upscaler") && i + 1 < argc) {  // off | fsr1 | fsr3 (this run only)
            std::string u = argv[++i];
            app.settings.upscaler = u == "fsr4" ? Upscaler::FSR4 : u == "fsr3" ? Upscaler::FSR3 : u == "fsr1" ? Upscaler::FSR1
                                  : u == "nis" ? Upscaler::NIS : u == "anime4k" ? Upscaler::Anime4K : Upscaler::Off;
            cliUpscaler = true;
        } else if (!strcmp(argv[i], "--debug-motion")) app.debugMotion = true;
        else if (!strcmp(argv[i], "--framegen")) { app.settings.frameGen = true; cliFrameGen = true; }
        else if (!strcmp(argv[i], "--no-framegen")) { app.settings.frameGen = false; cliFrameGen = true; }
        else if (!strcmp(argv[i], "--fg-predict")) { app.settings.fgPredict = true; cliPredict = true; }
        else if (!strcmp(argv[i], "--no-fg-predict")) { app.settings.fgPredict = false; cliPredict = true; }
        else if (!strcmp(argv[i], "--flow-scale") && i + 1 < argc) {  // auto | 100 | 75 | 50 | 25 (this run only)
            std::string v = argv[++i];
            app.settings.fgFlowScale = v == "auto" ? 0 : std::clamp(atoi(v.c_str()), 10, 100);
            cliFlow = true;
        }
        else if (!strcmp(argv[i], "--compare")) app.compareView = true;
        else if (!strcmp(argv[i], "--no-jitter")) noJitter = true;
        else if (!strcmp(argv[i], "--no-fglock")) noFgLock = true;
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) {  // auto|native|quality|balanced|performance|ultra (this run only)
            std::string q = argv[++i];
            const char* names[] = {"auto", "native", "quality", "balanced", "performance", "ultra"};
            for (int k = 0; k < 6; k++) if (q == names[k]) app.settings.fsrQuality = k;
            cliQuality = true;
        }
        else if (!strcmp(argv[i], "--sharpness") && i + 1 < argc) { app.settings.sharpness = std::clamp(float(atof(argv[++i])), 0.0f, 1.0f); cliSharpness = true; }
        else if (!strcmp(argv[i], "--anime4k") && i + 1 < argc) {  // s|m|l
            char v = argv[++i][0];
            app.settings.anime4kVariant = v == 's' || v == 'S' ? 0 : v == 'l' || v == 'L' ? 2 : 1;
            cliA4k = true;
        }
        else if (!strcmp(argv[i], "--test-pattern")) testPattern = 1;
        else if (!strcmp(argv[i], "--test-pattern=smooth")) testPattern = 2;
        else if (!strcmp(argv[i], "--test-pattern=fast")) testPattern = 3;
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--device /dev/videoN] [--fullscreen] [--selftest SECONDS]\n", argv[0]);
            return 0;
        }
    }

    app.window = SDL_CreateWindow(APP_NAME, 1440, 900, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!app.window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(app.window, 720, 480);
    {
        extern const unsigned char app_icon_png[];
        extern const size_t app_icon_png_size;
        if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(app_icon_png, app_icon_png_size), true)) {
            SDL_SetWindowIcon(app.window, icon);  // desktops on Wayland use lyvra.desktop's icon via the app id
            SDL_DestroySurface(icon);
        }
    }

    std::string err;
    if (!app.renderer.init(app.window, app.settings.present, err)) {
        fprintf(stderr, "Renderer init failed: %s\n", err.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, err.c_str(), nullptr);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::loadFonts(app);
    ui::applyTheme();
    ImGui_ImplSDL3_InitForVulkan(app.window);
    app.renderer.initImGui();

    app.capture.setWakeCallback([] {
        if (!gFrameEventPending.exchange(true)) {
            SDL_Event e{};
            e.type = gFrameEvent;
            SDL_PushEvent(&e);
        }
    });

    app.refreshDevices();
    bool opened = false;
    if (!app.settings.devicePath.empty() && !testPattern)
        for (auto& d : app.devices)
            if (d.path == app.settings.devicePath) opened = app.openDevice(d.path, nullptr);
    if (!opened && !testPattern) app.showPicker = true;
    std::vector<uint8_t> testFrame(1920 * 1080 * 3 / 2);
    uint64_t testFrameIdx = 0, lastTestNs = 0;
    bool realPending = false;
    uint64_t realDueNs = 0;
    bool predPending = false;  // predict mode: a predicted frame is due at predDueNs unless a real one comes first
    uint64_t predDueNs = 0, lastAcquiredNs = 0;
    CaptureFrame pendingCf{};

    if (showPanel.rfind("settings", 0) == 0) {
        app.showSettings = true;
        if (showPanel.size() > 9) app.settingsTab = atoi(showPanel.c_str() + 9);
    } else if (showPanel == "stats") app.showStats = true;
    else if (showPanel == "picker") app.showPicker = true;

    SDL_ShowWindow(app.window);
    if (app.settings.startFullscreen || cliFullscreen) SDL_SetWindowFullscreen(app.window, true);

    uint64_t lastRender = 0, lastStatsTick = nowNs();
    const uint64_t startNs = nowNs();
    uint64_t lastReportNs = startNs;
    app.lastInputNs = nowNs();

    while (!app.quit) {
        // Sleep until a capture frame or input arrives. Idle UI still refreshes ~4x/s for stats.
        bool animating = testPattern != 0 || nowNs() - app.lastInputNs < 600'000'000ull;
        int waitMs = animating ? 8 : 250;
        if (realPending || predPending) {  // a real (or predicted) frame is due shortly
            uint64_t t = nowNs(), due = realPending ? realDueNs : predDueNs;
            waitMs = due > t + 2'000'000 ? int((due - t) / 1'000'000) - 1 : 0;
        }
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, waitMs);
        bool uiEvent = false;
        while (got) {
            if (e.type == gFrameEvent) {
                gFrameEventPending = false;
            } else {
                ImGui_ImplSDL3_ProcessEvent(&e);
                uiEvent = true;
                switch (e.type) {
                    case SDL_EVENT_QUIT: app.quit = true; break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: app.renderer.onResize(); break;
                    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: app.fullscreen = true; break;
                    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: app.fullscreen = false; break;
                    case SDL_EVENT_AUDIO_DEVICE_ADDED:
                    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
                        app.audioInputs = AudioPassthrough::inputs();
                        app.audioOutputs = AudioPassthrough::outputs();
                        break;
                    default: break;
                }
            }
            got = SDL_PollEvent(&e);
        }
        uint64_t now = nowNs();
        if (uiEvent) app.lastInputNs = now;

        // Device failure (unplugged, etc.)
        if (app.capture.failed && app.capture.isOpen()) {
            app.showToast(app.capture.failReason);
            app.closeDevice();
            app.refreshDevices();
            app.showPicker = true;
        }

        // Apply live settings
        app.renderer.setPresentMode(app.settings.present);
        app.audio.setVolume(app.settings.volume, app.settings.muted);
        app.audio.setMaxLatencyMs(app.settings.audioMaxMs);
        app.audio.decay(float(now - lastStatsTick) / 1e9f);

        // Frame generation pacing: nothing else is shown until the held real frame's time.
        if (realPending) {
            uint64_t t = nowNs();
            if (t < realDueNs) {
                if (realDueNs - t > 1'500'000) { lastStatsTick = now; continue; }
                SDL_DelayPrecise(realDueNs - t);
            }
        }
        // Predict mode: a new real frame is shown as soon as it arrives; otherwise the predicted
        // frame goes out half a capture interval after the last real one.
        bool predictNow = false;
        if (predPending) {
            // (The test pattern takes ~17 ms of CPU to draw, so its next frame simply waits for the predicted one.)
            bool newReal = !testPattern && app.capture.lastFrameNs.load() != lastAcquiredNs;
            if (newReal || !app.settings.frameGen || !app.settings.fgPredict) {
                predPending = false;
            } else {
                uint64_t t = nowNs();
                if (t < predDueNs) {
                    if (predDueNs - t > 1'500'000) { lastStatsTick = now; continue; }
                    SDL_DelayPrecise(predDueNs - t);
                }
                predictNow = true;
            }
        }
        bool frameReady = testPattern != 0 || (app.capture.isOpen() && app.capture.lastFrameNs.load() > 0);
        if (!frameReady && !uiEvent && !animating && now - lastRender < 200'000'000ull) { lastStatsTick = now; continue; }

        // Build UI
        if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(app.window))) app.displayHz = dm->refresh_rate;
        int ww, wh, pw, ph;
        SDL_GetWindowSize(app.window, &ww, &wh);
        SDL_GetWindowSizeInPixels(app.window, &pw, &ph);
        app.pxPerPoint = ww > 0 ? float(pw) / ww : 1.0f;
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ui::draw(app);
        ImGui::Render();

        // Grab the newest frame as late as possible, right before upload.
        CaptureFrame cf;
        bool have = !realPending && app.capture.isOpen() && app.capture.acquireLatest(cf);
        if (have) lastAcquiredNs = cf.dequeueNs;
        FrameInput in;
        if (have) {
            in.data = cf.data;
            in.bytes = cf.bytes;
            in.width = app.capture.width;
            in.height = app.capture.height;
            in.fourcc = cf.fourcc;  // MJPEG arrives already decoded to planar YUV
            in.stride = cf.stride;
            in.bt601 = app.settings.matrix == ColorMatrix::BT601 || (app.settings.matrix == ColorMatrix::Auto && in.height < 720);
            in.fullRange = app.settings.range == ColorRange::Full;
        }
        if (testPattern && !realPending && !predictNow && nowNs() - lastTestNs >= 16'666'666ull) {
            // Mode 1 (--test-pattern): left half hashed blocks scrolling right 4 px/frame, right half static.
            // Mode 2 (--test-pattern=smooth): anti-aliased stripes + rings repeating every 960 px, so the
            // left and right halves are identical (for split view). Top half moves 1.37 px/frame.
            lastTestNs = nowNs();
            uint64_t fi = testFrameIdx++;
            if (testPattern == 2) {
                double off = fi * 1.37;
                for (int y = 0; y < 1080; y++)
                    for (int x = 0; x < 1920; x++) {
                        double px = (x % 960) - (y < 540 ? off : 0.0), py = y % 540;
                        double stripes = std::sin((px * 0.866 + py * 0.5) * 0.35);
                        double r = std::hypot(std::fmod(px + 96000.0, 240.0) - 120.0, py - 270.0);
                        double ring = std::clamp(std::fabs(std::fmod(r, 24.0) - 12.0) - 4.0, -1.0, 1.0);
                        double v = py < 270 ? stripes : ring;
                        testFrame[y * 1920 + x] = uint8_t(128 + 90 * v);
                    }
            } else {
                int off = int(fi * (testPattern == 3 ? 150 : 4));
                for (int y = 0; y < 1080; y++)
                    for (int x = 0; x < 1920; x++) {
                        int sx = x < 960 ? x - off : x;
                        uint32_t hsh = uint32_t((sx >> 3) * 73856093) ^ uint32_t((y >> 3) * 19349663);
                        hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
                        testFrame[y * 1920 + x] = uint8_t(40 + (hsh >> 24) % 180);
                        // Static "HUD": white glyph-like bars at a fixed place over the moving half
                        if (y >= 500 && y < 560 && x >= 120 && x < 840 && ((x / 12) % 3 != 2) && ((y / 10) % 3 != 1)) testFrame[y * 1920 + x] = 245;
                    }
            }
            memset(testFrame.data() + 1920 * 1080, 128, 1920 * 1080 / 2);
            in = {};
            in.data = testFrame.data();
            in.bytes = testFrame.size();
            in.width = 1920;
            in.height = 1080;
            in.fourcc = V4L2_PIX_FMT_NV12;
            in.stride = 1920;
            have = true;
            cf = {};
        }
        RenderParams rp{app.videoAreaPx, app.settings.scale, app.settings.upscaler, app.settings.sharpness,
                        app.capture.fps > 0 && !testPattern ? float(1000.0 / app.capture.fps) : 16.67f, app.debugMotion, app.compareView, !noJitter,
                        app.settings.frameGen, !noFgLock, app.settings.fsrQuality, app.settings.anime4kVariant};
        rp.fgPredict = app.settings.fgPredict;
        rp.predictFrame = predictNow && !have;
        rp.fgFlowScale = app.settings.fgFlowScale;
        if (app.screenshotRequested && !(app.settings.frameGen && app.renderer.frameGenAvailable())) {
            app.screenshotRequested = false;
            const char* home = getenv("HOME");
            std::string dir = std::string(home ? home : ".") + "/Pictures/" APP_NAME;
            std::filesystem::create_directories(dir);
            time_t t = time(nullptr);
            char name[64];
            strftime(name, sizeof name, "/" APP_ID "-%Y%m%d-%H%M%S.bmp", localtime(&t));
            app.renderer.requestScreenshot(dir + name);
        }
        if (app.screenshotRequested && app.settings.frameGen && app.renderer.frameGenAvailable() && shotOnGenerated.empty()) {
            app.screenshotRequested = false;  // with frame gen on, F12 grabs the next generated frame
            const char* home = getenv("HOME");
            std::string dir = std::string(home ? home : ".") + "/Pictures/" APP_NAME;
            std::filesystem::create_directories(dir);
            time_t t = time(nullptr);
            char name[64];
            strftime(name, sizeof name, "/" APP_ID "-%Y%m%d-%H%M%S-generated.bmp", localtime(&t));
            shotOnGenerated = dir + name;
        }
        if (!shotOnGenerated.empty() && (app.settings.fgPredict ? rp.predictFrame : have)) {  // grab a generated / predicted frame
            app.renderer.requestScreenshot(shotOnGenerated);
            shotOnGenerated.clear();
        }
        bool ok = app.renderer.render(have ? &in : nullptr, rp, ImGui::GetDrawData());
        if (!app.renderer.savedScreenshot.empty()) {
            app.showToast("Screenshot saved: " + app.renderer.savedScreenshot);
            app.renderer.savedScreenshot.clear();
        }
        if (have && cf.index >= 0) app.capture.release(cf);
        lastRender = nowNs();

        // With frame generation, a new frame first shows the generated in-between frame; the real
        // one follows half a capture interval later and is what latency is measured on.
        bool realShown = false;
        CaptureFrame shownCf = cf;
        if (predictNow) {  // predict mode: the predicted frame went out (or couldn't be made)
            predPending = false;
            if (ok && app.renderer.predictedShown) {
                app.stats.generatedFrames++;
                app.stats.genWindowFrames++;
            }
        }
        if (ok && have && app.renderer.predictReady()) {
            double fps = app.capture.measuredFps.load() > 1 ? app.capture.measuredFps.load() : (app.capture.fps > 0 ? app.capture.fps : 60.0);
            if (testPattern) fps = 60.0;
            predPending = true;
            predDueNs = app.renderer.lastSubmitNs + uint64_t(0.5e9 / fps);
        }
        if (ok && app.renderer.generatedShown) {
            double fps = app.capture.measuredFps.load() > 1 ? app.capture.measuredFps.load() : (app.capture.fps > 0 ? app.capture.fps : 60.0);
            if (testPattern) fps = 60.0;
            realPending = true;
            realDueNs = app.renderer.lastSubmitNs + uint64_t(0.5e9 / fps);
            pendingCf = cf;
            app.stats.generatedFrames++;
            app.stats.genWindowFrames++;
            auto& r = app.renderer;  // this slot's previous use was also a generated frame
            if (r.gpu.valid) {
                auto ema = [](float& v, float x) { v = v == 0 ? x : v * 0.9f + x * 0.1f; };
                ema(app.stats.gpuConvert, r.gpu.convert);
                ema(app.stats.gpuUpscale, r.gpu.upscale);
            }
        } else if (realPending) {
            realPending = false;
            realShown = ok;
            shownCf = pendingCf;
        } else {
            realShown = have && ok;
        }
        if (realShown && !testPattern) {
            CaptureFrame& cf = shownCf;
            auto& r = app.renderer;
            auto& s = app.stats;
            float gpuMs = r.gpu.valid ? r.gpu.total : 0.f;
            float lat = float(double(r.lastSubmitNs - cf.timestampNs) / 1e6) + gpuMs;
            float ft = s.lastShownNs ? float(double(r.lastSubmitNs - s.lastShownNs) / 1e6) : 0.f;
            s.lastShownNs = r.lastSubmitNs;
            if (ft > 0) s.push(lat, ft);
            auto ema = [](float& v, float x) { v = v == 0 ? x : v * 0.9f + x * 0.1f; };
            ema(s.transfer, float(double(cf.dequeueNs - cf.timestampNs) / 1e6));
            ema(s.decode, float(double(cf.decodeNs) / 1e6));
            ema(s.wait, float(double(r.lastRenderStartNs - cf.dequeueNs - cf.decodeNs) / 1e6));
            ema(s.cpu, float(double(r.lastSubmitNs - r.lastRenderStartNs) / 1e6));
            if (r.gpu.valid) {
                if (!app.settings.frameGen || app.settings.fgPredict) {
                    ema(s.gpuConvert, r.gpu.convert);
                    ema(s.gpuUpscale, r.gpu.upscale);
                }
                ema(s.gpuComposite, r.gpu.present);
            }
            s.shownFrames++;
            s.fpsWindowFrames++;
            if (!s.fpsWindowStart) s.fpsWindowStart = r.lastSubmitNs;
            double span = double(r.lastSubmitNs - s.fpsWindowStart) / 1e9;
            if (span >= 0.5) {
                s.shownFps = s.fpsWindowFrames / span;
                s.genFps = s.genWindowFrames / span;
                s.fpsWindowStart = r.lastSubmitNs;
                s.fpsWindowFrames = 0;
                s.genWindowFrames = 0;
            }
        }
        if (!app.signalOk()) app.stats.shownFps = app.stats.genFps = 0;
        if (!app.settings.frameGen) app.stats.genFps = 0;
        lastStatsTick = now;

        if (selftestSec > 0 && now - lastReportNs >= 1'000'000'000ull) {
            lastReportNs = now;
            auto& r = app.renderer;
            auto& s = app.stats;
            printf("[selftest] cap %.1f fps | shown %.1f fps | latency avg %.2f p99 %.2f min %.2f ms | xfer %.2f dec %.2f wait %.2f cpu %.2f | "
                   "gpu conv %.3f up %.3f comp %.3f | %ux%u %s -> %ux%u %s | dropped %llu+%llu | audio %s %.1f ms | gen %.1f fps\n",
                   app.capture.measuredFps.load(), s.shownFps, s.latAvg, s.latP99, s.latMin, s.transfer, s.decode, s.wait, s.cpu,
                   s.gpuConvert, s.gpuUpscale, s.gpuComposite, app.capture.width, app.capture.height, fourccName(app.capture.fourcc),
                   r.outW, r.outH, r.effectiveUpscaler == Upscaler::FSR4 ? "FSR4" : r.effectiveUpscaler == Upscaler::FSR3 ? "FSR3" : r.effectiveUpscaler == Upscaler::FSR1 ? "FSR1"
                   : r.effectiveUpscaler == Upscaler::NIS ? "NIS" : r.effectiveUpscaler == Upscaler::Anime4K ? "Anime4K" : "native",
                   (unsigned long long)app.capture.framesSkipped.load(), (unsigned long long)app.capture.framesLostByDriver.load(),
                   app.audio.active() ? app.audio.inputName.c_str() : ("off: " + app.audioError).c_str(), app.audio.latencyMs(), s.genFps);
            if (!r.frameGenStatus.empty()) printf("[selftest] frame gen status: %s\n", r.frameGenStatus.c_str());
            if (testPattern && r.gpuWork.valid) printf("[selftest] pattern gpu: convert %.3f upscale %.3f composite %.3f ms (%s, %ux%u)\n", r.gpuWork.convert, r.gpuWork.upscale, r.gpuWork.present,
                                                   r.effectiveUpscaler == Upscaler::Off ? "native" : "upscaled", r.outW, r.outH);
            if (app.settings.frameGen) printf("[selftest] frame gen: %s, flow %ux%u, %llu generated frames\n", app.settings.fgPredict ? "predict" : "interpolate",
                                              r.frameGenFlowW(), r.frameGenFlowH(), (unsigned long long)app.stats.generatedFrames);
            if (app.settings.frameGen) printf("[selftest] frame gen: %llu skipped for fast motion, last unexplained %.1f%%\n", (unsigned long long)r.frameGenSkipped(), r.frameGenFailedFraction() * 100);
            if (!r.temporalStatus.empty()) printf("[selftest] temporal upscaler status: %s\n", r.temporalStatus.c_str());
            fflush(stdout);
            if (now - startNs + 1'000'000'000ull >= uint64_t(selftestSec) * 1'000'000'000ull && !screenshotPath.empty()) {
                if (app.settings.frameGen) shotOnGenerated = screenshotPath;
                else app.renderer.requestScreenshot(screenshotPath);
                screenshotPath.clear();
            }
            if (now - startNs >= uint64_t(selftestSec) * 1'000'000'000ull) app.quit = true;
        }
    }

    if (cliUpscaler) app.settings.upscaler = savedUpscaler;
    if (cliFrameGen) app.settings.frameGen = savedFrameGen;
    if (cliPredict) app.settings.fgPredict = savedPredict;
    if (cliFlow) app.settings.fgFlowScale = savedFlow;
    if (cliQuality) app.settings.fsrQuality = savedQuality;
    if (cliSharpness) app.settings.sharpness = savedSharpness;
    if (cliA4k) app.settings.anime4kVariant = savedA4k;
    app.settings.save();
    app.capture.close();
    app.audio.stop();
    app.renderer.shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
