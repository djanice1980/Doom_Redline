#pragma once
// Music player: MUS tracks sequenced at 140 Hz into a synthesizer backend
// (OPL3 emulation with GENMIDI, or FluidSynth with a soundfont when one is
// available), rendered on SDL's audio thread through an SDL_AudioStream.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/genmidi.h"
#include "audio/mus.h"

struct SDL_AudioStream;

namespace rl::audio {

class SynthBackend {
public:
    virtual ~SynthBackend() = default;
    virtual void noteOn(int channel, int note, int velocity) = 0;
    virtual void noteOff(int channel, int note) = 0;
    virtual void programChange(int channel, int program) = 0;
    virtual void controller(int channel, int cc, int value) = 0;
    virtual void pitchBend(int channel, int value14) = 0;
    virtual void allNotesOff() = 0;
    virtual void render(float* out, int frames) = 0;   // additive, interleaved stereo
    virtual const char* name() const = 0;
};

class Music {
public:
    Music() = default;
    ~Music();
    Music(const Music&) = delete;
    Music& operator=(const Music&) = delete;

    // deviceId: the SDL audio device already opened by Audio. Returns false if
    // no backend could be created (music stays silent).
    bool init(uint32_t deviceId, uint32_t sampleRate, const GenMidiBank& bank, const std::string& soundfontPath);
    void addTrack(const std::string& name, std::vector<uint8_t> musData);
    bool hasTrack(const std::string& name) const { return tracks_.count(name) != 0; }
    std::vector<std::string> trackNames() const;

    // Starts a track (fading in). `loop` repeats it; otherwise the track ends
    // and `next` (if given and known) starts afterwards.
    void play(const std::string& name, bool loop = true, const std::string& next = "");
    void stop(float fadeSeconds = 0.6f);
    const std::string& current() const { return currentName_; }
    void setVolume(float v);                 // 0..1 master music level
    float volume() const { return volume_; }
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    const char* backendName() const;

private:
    static void streamCallback(void* userdata, SDL_AudioStream* stream, int additional, int total);
    void renderInto(float* out, int frames);
    void dispatch(const MusEvent& ev);

    std::unique_ptr<SynthBackend> backend_;
    std::map<std::string, MusTrack> tracks_;
    SDL_AudioStream* stream_ = nullptr;
    uint32_t sampleRate_ = 48000;
    std::mutex mutex_;

    // Sequencer state (guarded by mutex_, touched by the audio thread).
    const MusTrack* track_ = nullptr;
    std::string currentName_, nextName_;
    bool loop_ = true;
    size_t eventIndex_ = 0;
    double samplesUntilNext_ = 0.0;
    float fade_ = 0.f;          // current fade multiplier 0..1
    float fadeTarget_ = 1.f;
    float fadeRate_ = 2.f;      // per second
    bool stopWhenFaded_ = false;
    float volume_ = 0.45f;
    bool enabled_ = true;
    std::vector<float> scratch_;
};

}  // namespace rl::audio
