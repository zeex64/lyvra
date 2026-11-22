#include "audio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

static std::vector<AudioDevice> list(bool recording) {
    std::vector<AudioDevice> out;
    int n = 0;
    SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&n) : SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids && i < n; i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        out.push_back({ids[i], name ? name : "Unknown"});
    }
    SDL_free(ids);
    return out;
}

std::vector<AudioDevice> AudioPassthrough::inputs() { return list(true); }
std::vector<AudioDevice> AudioPassthrough::outputs() { return list(false); }

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool AudioPassthrough::start(const std::string& inName, const std::string& cardHint, const std::string& outName, std::string& err) {
    stop();
    SDL_AudioDeviceID inId = 0;
    auto ins = inputs();
    if (!inName.empty()) {
        for (auto& d : ins) if (d.name == inName) { inId = d.id; inputName = d.name; }
    }
    if (!inId && !cardHint.empty()) {
        // Match on the first word of the capture card name ("DX20R Video Device" -> "dx20r").
        std::string token = lower(cardHint.substr(0, cardHint.find(' ')));
        for (auto& d : ins) if (lower(d.name).find(token) != std::string::npos) { inId = d.id; inputName = d.name; break; }
    }
    if (!inId) { err = "No matching audio input for this capture device"; inputName.clear(); return false; }

    SDL_AudioDeviceID outId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    outputName = "System default";
    if (!outName.empty())
        for (auto& d : outputs()) if (d.name == outName) { outId = d.id; outputName = d.name; }

    play_ = SDL_OpenAudioDeviceStream(outId, &spec_, nullptr, nullptr);
    if (!play_) { err = std::string("Audio output: ") + SDL_GetError(); return false; }
    rec_ = SDL_OpenAudioDeviceStream(inId, &spec_, &AudioPassthrough::onRecord, this);
    if (!rec_) { err = std::string("Audio input: ") + SDL_GetError(); SDL_DestroyAudioStream(play_); play_ = nullptr; return false; }

    SDL_AudioSpec devSpec;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(play_), &devSpec, &deviceFrames_)) deviceFrames_ = 0;
    buf_.resize(1 << 16);
    droppedChunks = 0;
    SDL_ResumeAudioStreamDevice(play_);
    SDL_ResumeAudioStreamDevice(rec_);
    return true;
}

void AudioPassthrough::stop() {
    if (rec_) SDL_DestroyAudioStream(rec_);
    if (play_) SDL_DestroyAudioStream(play_);
    rec_ = play_ = nullptr;
    peak_[0] = peak_[1] = 0.f;
}

void AudioPassthrough::setVolume(float v, bool muted) {
    if (play_) SDL_SetAudioStreamGain(play_, muted ? 0.0f : v);
}

void SDLCALL AudioPassthrough::onRecord(void* user, SDL_AudioStream* stream, int, int) {
    auto* self = static_cast<AudioPassthrough*>(user);
    int avail = SDL_GetAudioStreamAvailable(stream);
    while (avail > 0) {
        int n = SDL_GetAudioStreamData(stream, self->buf_.data(), std::min<int>(avail, int(self->buf_.size())));
        if (n <= 0) break;
        avail -= n;

        const int16_t* s = reinterpret_cast<const int16_t*>(self->buf_.data());
        int frames = n / 4;
        float pk[2] = {0, 0};
        for (int i = 0; i < frames; i++) {
            pk[0] = std::max(pk[0], std::abs(s[i * 2]) / 32768.0f);
            pk[1] = std::max(pk[1], std::abs(s[i * 2 + 1]) / 32768.0f);
        }
        for (int c = 0; c < 2; c++)
            if (pk[c] > self->peak_[c].load()) self->peak_[c] = pk[c];

        // Keep latency bounded: if the output queue is over budget, drop this chunk.
        int maxBytes = self->spec_.freq * 4 * self->maxMs_ / 1000;
        if (SDL_GetAudioStreamQueued(self->play_) > maxBytes) { self->droppedChunks++; continue; }
        SDL_PutAudioStreamData(self->play_, self->buf_.data(), n);
    }
}

void AudioPassthrough::decay(float dt) {
    for (auto& p : peak_) p = std::max(0.0f, p.load() - dt * 1.5f);
}

float AudioPassthrough::latencyMs() const {
    if (!play_) return 0;
    float queued = float(SDL_GetAudioStreamQueued(play_)) / (spec_.freq * 4) * 1000.0f;
    float device = deviceFrames_ > 0 ? float(deviceFrames_) / spec_.freq * 1000.0f * 2.0f : 0.0f;  // in + out buffers
    return queued + device;
}
