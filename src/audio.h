#pragma once
#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

struct AudioDevice { SDL_AudioDeviceID id; std::string name; };

// Capture-card audio -> speakers with a hard cap on queued audio.
class AudioPassthrough {
public:
    ~AudioPassthrough() { stop(); }
    static std::vector<AudioDevice> inputs();
    static std::vector<AudioDevice> outputs();

    // inputName: exact device name, or empty to match `cardHint` (e.g. "DX20R").
    bool start(const std::string& inputName, const std::string& cardHint, const std::string& outputName, std::string& err);
    void stop();
    bool active() const { return rec_ != nullptr; }

    void setVolume(float v, bool muted);
    void setMaxLatencyMs(int ms) { maxMs_ = ms; }

    float peak(int ch) const { return peak_[ch].load(); }  // 0..1, decays in decay()
    void decay(float dt);
    float latencyMs() const;

    std::string inputName, outputName;
    std::atomic<uint64_t> droppedChunks{0};

private:
    static void SDLCALL onRecord(void* user, SDL_AudioStream* stream, int additional, int total);

    SDL_AudioStream* rec_ = nullptr;
    SDL_AudioStream* play_ = nullptr;
    SDL_AudioSpec spec_{SDL_AUDIO_S16, 2, 48000};
    std::atomic<int> maxMs_{30};
    std::atomic<float> peak_[2]{{0.f}, {0.f}};
    int deviceFrames_ = 0;
    std::vector<uint8_t> buf_;
};
