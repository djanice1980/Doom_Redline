#include "audio/music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

namespace {
void writeWav(const std::string& path, const std::vector<float>& mix, uint32_t rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t n = static_cast<uint32_t>(mix.size());
    uint32_t dataBytes = n * 2;
    auto p32 = [&](uint32_t v) { std::fputc(v & 255, f); std::fputc((v >> 8) & 255, f); std::fputc((v >> 16) & 255, f); std::fputc((v >> 24) & 255, f); };
    auto p16 = [&](uint16_t v) { std::fputc(v & 255, f); std::fputc((v >> 8) & 255, f); };
    std::fwrite("RIFF", 1, 4, f); p32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); p32(16); p16(1); p16(2); p32(rate); p32(rate * 4); p16(4); p16(16);
    std::fwrite("data", 1, 4, f); p32(dataBytes);
    for (float v : mix) p16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f))));
    std::fclose(f);
    std::fprintf(stderr, "[music] dumped %u frames to %s\n", n / 2, path.c_str());
}
}  // namespace

Music::~Music() { shutdown(); }

void Music::shutdown() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);   // unbinds and stops the callback
        stream_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ogg_.reset();
        track_ = nullptr;
    }
    if (!dumpPath_.empty() && !dumpDone_ && !dump_.empty()) { dumpDone_ = true; writeWav(dumpPath_, dump_, sampleRate_); }
}

void Music::clearTracks() {
    std::lock_guard<std::mutex> lock(mutex_);
    track_ = nullptr;
    currentName_.clear();
    nextName_.clear();
    tracks_.clear();
    oggTracks_.clear();
}

bool Music::init(uint32_t deviceId, uint32_t sampleRate, const GenMidiBank& bank, const std::string& soundfontPath) {
    sampleRate_ = sampleRate;
    backend_.reset();   // a re-init (new WAD) gets the new GENMIDI bank
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
    if (const char* d = std::getenv("REDLINE_MUSIC_DUMP")) { dumpPath_ = d; dumpFrames_ = static_cast<size_t>(sampleRate) * 15; dump_.reserve(dumpFrames_ * 2); }
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

void Music::addOggTrack(const std::string& name, const std::string& path, uint64_t offset, uint64_t size, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    oggTracks_[name] = OggInfo{path, offset, size, gain};
}

bool Music::oggAvailable() const {
#ifdef REDLINE_HAVE_VORBIS
    return true;
#else
    return false;
#endif
}

std::vector<std::string> Music::trackNames() const {
    std::vector<std::string> out;
    for (auto& [n, t] : tracks_) out.push_back(n);
    for (auto& [n, t] : oggTracks_) out.push_back(n);
    return out;
}

void Music::play(const std::string& name, bool loop, const std::string& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto oit = oggTracks_.find(name);
    auto it = tracks_.find(name);
    if (oit == oggTracks_.end() && it == tracks_.end()) return;
    if (backend_) backend_->allNotesOff();
    ogg_.reset();
    track_ = nullptr;
    if (oit != oggTracks_.end()) {
        ogg_ = OggStream::open(oit->second.path, oit->second.offset, oit->second.size, sampleRate_);
        if (!ogg_) { std::fprintf(stderr, "[music] cannot open %s\n", name.c_str()); if (it == tracks_.end()) return; }
        else oggGain_ = oit->second.gain;
    }
    if (!ogg_) track_ = &it->second;
    if (ogg_) std::fprintf(stderr, "[music] playing %s (ogg %u Hz, %.0f s)%s\n", name.c_str(), ogg_->sourceRate(), ogg_->lengthSeconds(), loop ? ", loop" : "");
    else std::fprintf(stderr, "[music] playing %s (mus, %.0f s)%s\n", name.c_str(), track_->totalTicks / 140.0, loop ? ", loop" : "");
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
    if (!track_ && !ogg_) return;
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
    if (!enabled_) return;
    if (ogg_) {
        // Streamed track: decode, loop or hand over at the end.
        int done = 0;
        while (done < frames) {
            int got = ogg_->read(out + static_cast<size_t>(done) * 2, frames - done, oggGain_);
            done += got;
            if (done < frames) {
                if (loop_) { ogg_->rewind(); if (ogg_->atEnd()) break; }
                else if (!nextName_.empty() && (oggTracks_.count(nextName_) || tracks_.count(nextName_))) {
                    std::string n = nextName_;
                    nextName_.clear();
                    auto oit = oggTracks_.find(n);
                    ogg_.reset();
                    if (oit != oggTracks_.end()) { ogg_ = OggStream::open(oit->second.path, oit->second.offset, oit->second.size, sampleRate_); oggGain_ = oit->second.gain; }
                    else { track_ = &tracks_[n]; eventIndex_ = 0; samplesUntilNext_ = 0.0; }
                    currentName_ = n;
                    loop_ = true;
                    if (!ogg_) break;   // continue below with the MUS sequencer
                } else { ogg_.reset(); currentName_.clear(); break; }
            }
        }
        if (ogg_ || !track_) { goto fade; }
    }
    if (!backend_ || !track_) return;
    {
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
                } else if (!nextName_.empty() && oggTracks_.count(nextName_)) {
                    auto oit = oggTracks_.find(nextName_);
                    ogg_ = OggStream::open(oit->second.path, oit->second.offset, oit->second.size, sampleRate_);
                    oggGain_ = oit->second.gain;
                    currentName_ = nextName_;
                    nextName_.clear();
                    loop_ = true;
                    track_ = nullptr;
                    break;
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
    }
fade:
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
        if (backend_) backend_->allNotesOff();
        track_ = nullptr;
        ogg_.reset();
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
    if (!self->dumpPath_.empty() && !self->dumpDone_) {
        self->dump_.insert(self->dump_.end(), self->scratch_.begin(), self->scratch_.end());
        if (self->dump_.size() >= self->dumpFrames_ * 2) {
            self->dumpDone_ = true;
            writeWav(self->dumpPath_, self->dump_, self->sampleRate_);
        }
    }
}

}  // namespace rl::audio
