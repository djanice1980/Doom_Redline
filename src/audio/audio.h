#pragma once
// Tiny sound-effect mixer on top of SDL3 audio streams: every play() binds a
// fresh stream (SDL resamples and mixes), update() reaps finished ones.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_AudioStream;

namespace rl::audio {

class Audio {
public:
    Audio() = default;
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool init();   // false = no audio device; play() becomes a no-op
    void addSound(const std::string& name, int sampleRate, std::vector<float> monoSamples);
    bool has(const std::string& name) const { return sounds_.count(name) != 0; }
    void play(const std::string& name, float gain = 1.f, float pitch = 1.f);
    void update();
    void setMasterGain(float g) { master_ = g; }

private:
    struct Sound { int rate; std::vector<float> samples; };
    std::unordered_map<std::string, Sound> sounds_;
    std::vector<SDL_AudioStream*> active_;
    uint32_t device_ = 0;
    float master_ = 0.8f;
    bool ok_ = false;
};

}  // namespace rl::audio
