#include "audio/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace rl::audio {

Audio::~Audio() {
    for (SDL_AudioStream* s : active_) SDL_DestroyAudioStream(s);
    if (device_) SDL_CloseAudioDevice(device_);
}

bool Audio::init() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device_) {
        std::fprintf(stderr, "[audio] no playback device: %s\n", SDL_GetError());
        return false;
    }
    ok_ = true;
    return true;
}

void Audio::addSound(const std::string& name, int sampleRate, std::vector<float> monoSamples) {
    sounds_[name] = Sound{sampleRate, std::move(monoSamples)};
}

void Audio::play(const std::string& name, float gain, float pitch) {
    if (!ok_) return;
    auto it = sounds_.find(name);
    if (it == sounds_.end()) return;
    const Sound& snd = it->second;
    if (snd.samples.empty()) return;
    // Cap simultaneous voices so a burst of events cannot pile up streams.
    if (active_.size() > 24) {
        SDL_DestroyAudioStream(active_.front());
        active_.erase(active_.begin());
    }
    SDL_AudioSpec src{SDL_AUDIO_F32, 1, snd.rate};
    SDL_AudioSpec dst{SDL_AUDIO_F32, 2, 48000};
    SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
    if (!stream) return;
    SDL_SetAudioStreamGain(stream, std::clamp(gain * master_, 0.f, 4.f));
    SDL_SetAudioStreamFrequencyRatio(stream, std::clamp(pitch, 0.25f, 4.f));
    SDL_PutAudioStreamData(stream, snd.samples.data(), static_cast<int>(snd.samples.size() * sizeof(float)));
    SDL_FlushAudioStream(stream);
    if (!SDL_BindAudioStream(device_, stream)) {
        SDL_DestroyAudioStream(stream);
        return;
    }
    active_.push_back(stream);
}

void Audio::update() {
    if (!ok_) return;
    for (size_t i = 0; i < active_.size();) {
        if (SDL_GetAudioStreamAvailable(active_[i]) <= 0 && SDL_GetAudioStreamQueued(active_[i]) <= 0) {
            SDL_DestroyAudioStream(active_[i]);
            active_.erase(active_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

}  // namespace rl::audio
