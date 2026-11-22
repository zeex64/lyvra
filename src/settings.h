#pragma once
#include <cstdint>
#include <string>

// App name. To rename, change it here plus CMakeLists.txt (project/target) and data/*.desktop.
#define APP_NAME "Lyvra"
#define APP_ID "lyvra"

enum class Upscaler : int { Off = 0, FSR1 = 1, FSR3 = 2, FSR4 = 3, NIS = 4, Anime4K = 5 };  // values are persisted
enum class PresentMode : int { Mailbox = 0, Immediate = 1, Fifo = 2 };
enum class ScaleMode : int { Fit = 0, Integer = 1, Stretch = 2 };
enum class ColorMatrix : int { Auto = 0, BT709 = 1, BT601 = 2 };
enum class ColorRange : int { Auto = 0, Limited = 1, Full = 2 };

struct Settings {
    // Source (0 = auto-pick)
    std::string devicePath;
    uint32_t fourcc = 0, width = 0, height = 0, fpsNum = 0, fpsDen = 0;
    ColorMatrix matrix = ColorMatrix::Auto;
    ColorRange range = ColorRange::Auto;

    // Upscaling / frame generation
    Upscaler upscaler = Upscaler::FSR1;
    float sharpness = 0.25f;  // 0..1, maps to RCAS stops
    int fsrQuality = 0;       // FSR 3/4 output size: 0 Auto ... 5 Ultra Performance
    int anime4kVariant = 1;   // 0 S (fast), 1 M, 2 L (best)
    bool frameGen = false;
    bool fgPredict = false;   // low-latency frame gen: predict the next frame instead of waiting for it
    int fgFlowScale = 0;      // optical flow resolution in % (0 = auto)

    // Display
    PresentMode present = PresentMode::Mailbox;
    ScaleMode scale = ScaleMode::Fit;
    bool startFullscreen = false;
    bool barsInFullscreen = false;

    // Audio
    bool audioEnabled = true;
    std::string audioInput;   // empty = match capture card
    std::string audioOutput;  // empty = system default
    float volume = 1.0f;
    bool muted = false;
    int audioMaxMs = 30;

    // Overlay
    bool showHud = true;
    int hudCorner = 0;  // 0 TL, 1 TR, 2 BL, 3 BR

    void load();
    void save() const;
    static std::string path();
};
