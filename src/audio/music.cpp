#include "audio/music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/opl_driver.h"
#ifdef REDLINE_HAVE_FLUIDSYNTH
#include <fluidsynth.h>
#endif

namespace rl::audio {

namespace {
constexpr double kMusTicksPerSecond = 140.0;

class OplBackend final : public SynthBackend {
public:
    OplBackend(uint32_t rate, const GenMidiBank& bank) : driver_(rate), bank_(bank) { driver_.setBank(&bank_); }
    void noteOn(int c, int n, int v) override { driver_.noteOn(c, n, v); }
    void noteOff(int c, int n) override { driver_.noteOff(c, n); }
    void programChange(int c, int p) override { driver_.programChange(c, p); }
    void controller(int c, int cc, int v) override { driver_.controller(c, cc, v); }
    void pitchBend(int c, int v) override { driver_.pitchBend(c, v); }
    void allNotesOff() override { driver_.allNotesOff(); }
    void render(float* out, int frames) override { driver_.render(out, frames); }
    const char* name() const override { return "OPL3 (GENMIDI)"; }

private:
    OplDriver driver_;
    GenMidiBank bank_;
};

#ifdef REDLINE_HAVE_FLUIDSYNTH
class FluidBackend final : public SynthBackend {
public:
    static std::unique_ptr<FluidBackend> create(uint32_t rate, const std::string& soundfont) {
        auto b = std::unique_ptr<FluidBackend>(new FluidBackend());
        b->settings_ = new_fluid_settings();
        fluid_settings_setnum(b->settings_, "synth.sample-rate", static_cast<double>(rate));
        fluid_settings_setnum(b->settings_, "synth.gain", 0.6);
        fluid_settings_setint(b->settings_, "synth.reverb.active", 1);
        fluid_settings_setint(b->settings_, "synth.chorus.active", 0);
        b->synth_ = new_fluid_synth(b->settings_);
        if (!b->synth_ || fluid_synth_sfload(b->synth_, soundfont.c_str(), 1) == FLUID_FAILED) return nullptr;
        return b;
    }
    ~FluidBackend() override {
        if (synth_) delete_fluid_synth(synth_);
        if (settings_) delete_fluid_settings(settings_);
    }
    void noteOn(int c, int n, int v) override { fluid_synth_noteon(synth_, c, n, v); }
    void noteOff(int c, int n) override { fluid_synth_noteoff(synth_, c, n); }
    void programChange(int c, int p) override { fluid_synth_program_change(synth_, c, p); }
    void controller(int c, int cc, int v) override { fluid_synth_cc(synth_, c, cc, v); }
    void pitchBend(int c, int v) override { fluid_synth_pitch_bend(synth_, c, v); }
    void allNotesOff() override { fluid_synth_all_notes_off(synth_, -1); }
    void render(float* out, int frames) override {
        tmp_.assign(static_cast<size_t>(frames) * 2, 0.f);
        fluid_synth_write_float(synth_, frames, tmp_.data(), 0, 2, tmp_.data(), 1, 2);
        for (size_t i = 0; i < tmp_.size(); ++i) out[i] += tmp_[i];
    }
    const char* name() const override { return "FluidSynth"; }

private:
    FluidBackend() = default;
    fluid_settings_t* settings_ = nullptr;
    fluid_synth_t* synth_ = nullptr;
    std::vector<float> tmp_;
};
#endif
}  // namespace

Music::~Music() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

bool Music::init(uint32_t deviceId, uint32_t sampleRate, const GenMidiBank& bank, const std::string& soundfontPath) {
    sampleRate_ = sampleRate;
#ifdef REDLINE_HAVE_FLUIDSYNTH
    if (!soundfontPath.empty()) {
        if (auto f = FluidBackend::create(sampleRate, soundfontPath)) {
            backend_ = std::move(f);
            std::fprintf(stderr, "[music] FluidSynth with %s\n", soundfontPath.c_str());
        } else {
            std::fprintf(stderr, "[music] could not load soundfont %s; using OPL3\n", soundfontPath.c_str());
        }
    }
#else
    (void)soundfontPath;
#endif
    if (!backend_) backend_ = std::make_unique<OplBackend>(sampleRate, bank);
    if (!deviceId) return false;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(sampleRate)};
    stream_ = SDL_CreateAudioStream(&spec, &spec);
    if (!stream_) return false;
    SDL_SetAudioStreamGetCallback(stream_, &Music::streamCallback, this);
    if (!SDL_BindAudioStream(deviceId, stream_)) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

const char* Music::backendName() const { return backend_ ? backend_->name() : "none"; }

void Music::addTrack(const std::string& name, std::vector<uint8_t> musData) {
    MusTrack t = parseMus(musData);
    if (!t.valid()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_[name] = std::move(t);
}

std::vector<std::string> Music::trackNames() const {
    std::vector<std::string> out;
    for (auto& [n, t] : tracks_) out.push_back(n);
    return out;
}

void Music::play(const std::string& name, bool loop, const std::string& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tracks_.find(name);
    if (it == tracks_.end()) return;
    if (backend_) backend_->allNotesOff();
    track_ = &it->second;
    currentName_ = name;
    nextName_ = next;
    loop_ = loop;
    eventIndex_ = 0;
    samplesUntilNext_ = 0.0;
    fade_ = 0.f;
    fadeTarget_ = 1.f;
    fadeRate_ = 1.5f;
    stopWhenFaded_ = false;
}

void Music::stop(float fadeSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!track_) return;
    fadeTarget_ = 0.f;
    fadeRate_ = fadeSeconds > 0.f ? 1.f / fadeSeconds : 1000.f;
    stopWhenFaded_ = true;
}

void Music::setVolume(float v) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::clamp(v, 0.f, 1.f);
}

void Music::setEnabled(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = on;
    if (!on && backend_) backend_->allNotesOff();
}

void Music::dispatch(const MusEvent& ev) {
    switch (ev.type) {
    case MusEvent::Type::NoteOff: backend_->noteOff(ev.channel, ev.a); break;
    case MusEvent::Type::NoteOn: backend_->noteOn(ev.channel, ev.a, ev.b); break;
    case MusEvent::Type::PitchBend: backend_->pitchBend(ev.channel, std::clamp(ev.a * 64, 0, 16383)); break;
    case MusEvent::Type::System:
        if (ev.a == 10 || ev.a == 11) backend_->controller(ev.channel, 123, 0);
        else if (ev.a == 14) backend_->controller(ev.channel, 121, 0);
        break;
    case MusEvent::Type::Controller:
        switch (ev.a) {
        case 0: backend_->programChange(ev.channel, ev.b); break;
        case 1: backend_->controller(ev.channel, 0, ev.b); break;
        case 2: backend_->controller(ev.channel, 1, ev.b); break;
        case 3: backend_->controller(ev.channel, 7, ev.b); break;
        case 4: backend_->controller(ev.channel, 10, ev.b); break;
        case 5: backend_->controller(ev.channel, 11, ev.b); break;
        case 6: backend_->controller(ev.channel, 91, ev.b); break;
        case 7: backend_->controller(ev.channel, 93, ev.b); break;
        case 8: backend_->controller(ev.channel, 64, ev.b); break;
        case 9: backend_->controller(ev.channel, 67, ev.b); break;
        default: break;
        }
        break;
    case MusEvent::Type::ScoreEnd:
        break;
    }
}

void Music::renderInto(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(frames) * 2);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!backend_ || !track_ || !enabled_) return;
    const double samplesPerTick = static_cast<double>(sampleRate_) / kMusTicksPerSecond;
    int done = 0;
    while (done < frames) {
        // Fire every event that is due now.
        while (samplesUntilNext_ <= 0.0 && track_) {
            const MusEvent& ev = track_->events[eventIndex_];
            if (ev.type == MusEvent::Type::ScoreEnd) {
                backend_->allNotesOff();
                if (loop_) {
                    eventIndex_ = 0;
                } else if (!nextName_.empty() && tracks_.count(nextName_)) {
                    track_ = &tracks_[nextName_];
                    currentName_ = nextName_;
                    nextName_.clear();
                    loop_ = true;
                    eventIndex_ = 0;
                } else {
                    track_ = nullptr;
                    currentName_.clear();
                    break;
                }
                continue;
            }
            dispatch(ev);
            samplesUntilNext_ += ev.delayTicks * samplesPerTick;
            ++eventIndex_;
            if (eventIndex_ >= track_->events.size()) eventIndex_ = track_->events.size() - 1;
        }
        int chunk = track_ ? std::max(1, std::min(frames - done, static_cast<int>(std::ceil(samplesUntilNext_)))) : frames - done;
        backend_->render(out + static_cast<size_t>(done) * 2, chunk);
        samplesUntilNext_ -= chunk;
        done += chunk;
        if (!track_) break;
    }
    // Fade and master volume, ramped per sample so switches are click-free.
    float step = fadeRate_ / static_cast<float>(sampleRate_);
    for (int i = 0; i < frames; ++i) {
        if (fade_ < fadeTarget_) fade_ = std::min(fadeTarget_, fade_ + step);
        else if (fade_ > fadeTarget_) fade_ = std::max(fadeTarget_, fade_ - step);
        float g = fade_ * volume_;
        out[static_cast<size_t>(i) * 2] *= g;
        out[static_cast<size_t>(i) * 2 + 1] *= g;
    }
    if (stopWhenFaded_ && fade_ <= 0.f) {
        backend_->allNotesOff();
        track_ = nullptr;
        currentName_.clear();
        stopWhenFaded_ = false;
    }
}

void Music::streamCallback(void* userdata, SDL_AudioStream* stream, int additional, int /*total*/) {
    auto* self = static_cast<Music*>(userdata);
    int frames = additional / static_cast<int>(sizeof(float) * 2);
    if (frames <= 0) return;
    frames = std::min(frames, 4096);
    self->scratch_.resize(static_cast<size_t>(frames) * 2);
    self->renderInto(self->scratch_.data(), frames);
    SDL_PutAudioStreamData(stream, self->scratch_.data(), frames * static_cast<int>(sizeof(float) * 2));
}

}  // namespace rl::audio
