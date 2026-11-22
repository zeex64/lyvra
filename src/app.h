#pragma once
#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

#include "audio.h"
#include "capture.h"
#include "renderer.h"
#include "settings.h"

struct ImFont;

struct Stats {
    static constexpr int N = 240;
    float latency[N] = {}, frameMs[N] = {};
    int head = 0, count = 0;

    // Smoothed stage breakdown (ms)
    float transfer = 0, decode = 0, wait = 0, cpu = 0, gpuConvert = 0, gpuUpscale = 0, gpuComposite = 0;
    float latAvg = 0, latMin = 0, latP99 = 0;
    double shownFps = 0;
    uint64_t shownFrames = 0, lastShownNs = 0;
    uint64_t fpsWindowStart = 0, fpsWindowFrames = 0;
    double genFps = 0;
    uint64_t genWindowFrames = 0, generatedFrames = 0;

    void push(float lat, float ft) {
        latency[head] = lat;
        frameMs[head] = ft;
        head = (head + 1) % N;
        count = std::min(count + 1, N);
        float tmp[N];
        int n = std::min(count, 120);
        float sum = 0, mn = 1e9f;
        for (int i = 0; i < n; i++) {
            tmp[i] = at(latency, i);
            sum += tmp[i];
            mn = std::min(mn, tmp[i]);
        }
        latAvg = sum / n;
        latMin = mn;
        std::sort(tmp, tmp + n);
        latP99 = tmp[std::min(n - 1, int(n * 0.99f))];
    }
    // i = 0 is newest
    float at(const float* a, int i) const { return a[(head - 1 - i + N * 2) % N]; }
    void reset() { *this = Stats{}; }
};

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    ImFont* monoSemibold = nullptr;
};

struct App {
    SDL_Window* window = nullptr;
    Settings settings;
    Capture capture;
    DeviceMonitor deviceMonitor;
    Renderer renderer;
    AudioPassthrough audio;
    std::vector<DeviceInfo> devices;
    std::vector<AudioDevice> audioInputs, audioOutputs;
    VideoMode activeMode;
    Stats stats;
    Fonts fonts;

    // UI state
    bool showSettings = false, showStats = false, showPicker = false;
    int settingsTab = 1;
    bool debugMotion = false;  // show optical-flow motion field (FSR 3 only)
    bool screenshotRequested = false;
    bool compareView = false;  // FSR 1 | FSR 3 split view
    float displayHz = 0;       // refresh rate of the display the window is on
    int pickerSelection = 0, pickerMode = 0, pickerPreset = 0;
    bool fullscreen = false;
    bool quit = false;
    uint64_t lastInputNs = 0;
    std::string toast;
    uint64_t toastUntilNs = 0;
    std::string audioError;

    // Hotplug / reconnect. `lost` is set when the device vanished while viewing; the app then waits
    // for it to come back (matched by USB bus, then name, then path) instead of opening the picker.
    bool lost = false;
    std::string lostName, lostBus, lostPath, lostReason;
    VideoMode lostMode;
    bool lostModeExplicit = false;
    uint64_t nextDeviceCheckNs = 0;   // when to re-enumerate next (hotplug event or periodic retry while lost)
    uint64_t captureStartedNs = 0;    // last (re)start of the stream
    uint64_t nextStreamRestartNs = 0; // signal recovery: stream off/on while no frames arrive
    uint64_t streamRestarts = 0;
    bool testPattern = false;

    Rect videoAreaPx;      // where the video may be drawn, in pixels
    float pxPerPoint = 1;  // framebuffer scale

    // Actions (main.cpp)
    void refreshDevices();
    bool openDevice(const std::string& path, const VideoMode* mode);
    void closeDevice();
    void restartAudio();
    void toggleFullscreen();
    void showToast(const std::string& msg);
    const DeviceInfo* currentDevice() const;
    bool signalOk() const;
    float noSignalSec() const;   // seconds since the last frame (0 while frames arrive or nothing is open)
    void onCaptureLost();        // capture thread reported an error: remember the device, wait for it
    void onDevicesChanged();     // re-enumerate; reconnect a lost device or auto-start the remembered one
    void restartCapture();       // stream off/on on the current device (no-signal recovery)
    void scheduleDeviceCheck(uint64_t delayNs);
};
