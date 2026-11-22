#include "settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

std::string Settings::path() {
    std::string base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME")) base = std::string(h) + "/.config";
    else base = ".";
    return base + "/" APP_ID "/settings.ini";
}

void Settings::load() {
    std::ifstream f(path());
    if (!f) {
        // First run under the new name: pick up settings saved as "peregrine" (the old working name).
        std::string legacy = path();
        legacy.replace(legacy.rfind("/" APP_ID "/"), std::string("/" APP_ID "/").size(), "/peregrine/");
        f.open(legacy);
    }
    if (!f) return;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto s = [&](const char* k, std::string& v) { if (kv.count(k)) v = kv[k]; };
    auto u = [&](const char* k, uint32_t& v) { if (kv.count(k)) v = (uint32_t)std::strtoul(kv[k].c_str(), nullptr, 10); };
    auto i = [&](const char* k, int& v) { if (kv.count(k)) v = std::atoi(kv[k].c_str()); };
    auto fl = [&](const char* k, float& v) { if (kv.count(k)) v = std::strtof(kv[k].c_str(), nullptr); };
    auto b = [&](const char* k, bool& v) { if (kv.count(k)) v = kv[k] == "1"; };
    auto e = [&](const char* k, auto& v) { if (kv.count(k)) v = static_cast<std::remove_reference_t<decltype(v)>>(std::atoi(kv[k].c_str())); };

    s("device", devicePath); u("fourcc", fourcc); u("width", width); u("height", height);
    u("fps_num", fpsNum); u("fps_den", fpsDen); e("matrix", matrix); e("range", range);
    e("upscaler", upscaler); fl("sharpness", sharpness); i("fsr_quality", fsrQuality); i("anime4k_variant", anime4kVariant); b("frame_gen", frameGen); b("fg_predict", fgPredict); i("fg_flow_scale", fgFlowScale);
    e("present", present); e("scale", scale); b("start_fullscreen", startFullscreen);
    b("bars_in_fullscreen", barsInFullscreen);
    b("audio", audioEnabled); s("audio_input", audioInput); s("audio_output", audioOutput);
    fl("volume", volume); b("muted", muted); i("audio_max_ms", audioMaxMs);
    b("hud", showHud); i("hud_corner", hudCorner);
}

void Settings::save() const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path()).parent_path(), ec);
    std::ofstream f(path());
    if (!f) return;
    f << "# " APP_NAME " settings\n"
      << "device=" << devicePath << "\nfourcc=" << fourcc << "\nwidth=" << width << "\nheight=" << height
      << "\nfps_num=" << fpsNum << "\nfps_den=" << fpsDen << "\nmatrix=" << (int)matrix << "\nrange=" << (int)range
      << "\nupscaler=" << (int)upscaler << "\nsharpness=" << sharpness << "\nfsr_quality=" << fsrQuality << "\nanime4k_variant=" << anime4kVariant << "\nframe_gen=" << frameGen << "\nfg_predict=" << fgPredict << "\nfg_flow_scale=" << fgFlowScale
      << "\npresent=" << (int)present << "\nscale=" << (int)scale << "\nstart_fullscreen=" << startFullscreen
      << "\nbars_in_fullscreen=" << barsInFullscreen
      << "\naudio=" << audioEnabled << "\naudio_input=" << audioInput << "\naudio_output=" << audioOutput
      << "\nvolume=" << volume << "\nmuted=" << muted << "\naudio_max_ms=" << audioMaxMs
      << "\nhud=" << showHud << "\nhud_corner=" << hudCorner << "\n";
}
