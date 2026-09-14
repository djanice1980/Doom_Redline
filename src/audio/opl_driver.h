#pragma once
// MIDI-style voice driver for the OPL3 emulator using a GENMIDI bank: the
// part of Doom's DMX that turned note-on/off, program, volume, pan and pitch
// bend into chip register writes. 18 two-operator voices in OPL3 mode.
#include <cstdint>

#include "audio/genmidi.h"
#include "audio/opl3.h"

namespace rl::audio {

class OplDriver {
public:
    explicit OplDriver(uint32_t sampleRate);
    void setBank(const GenMidiBank* bank) { bank_ = bank; }
    void reset();

    void noteOn(int channel, int note, int velocity);
    void noteOff(int channel, int note);
    void programChange(int channel, int program);
    void controller(int channel, int cc, int value);
    void pitchBend(int channel, int value14);      // 0..16383, 8192 = centre, range +-2 semitones
    void allNotesOff();

    // Additive stereo render.
    void render(float* out, int frames) { opl_.generate(out, frames); }

private:
    struct Voice {
        bool active = false;
        int channel = 0;
        int note = 0;                 // sounding note (fixed for percussion)
        int triggerNote = 0;          // MIDI note that started it
        int chip = 0;                 // chip channel 0-17
        uint64_t stamp = 0;           // allocation order for stealing
        const GenMidiVoice* data = nullptr;
        bool secondary = false;       // second voice of a double instrument
        int noteOffset = 0;           // base note offset + fixed note handling
        float detune = 0.f;           // semitones
        bool held = false;            // key released while sustain pedal down
        int velocity = 100;
    };
    struct Channel {
        int program = 0;
        int volume = 100;
        int expression = 127;
        int pan = 64;
        int bend = 8192;
        bool sustain = false;
    };

    int allocVoice();
    void programVoice(Voice& v);
    void updateFrequency(const Voice& v);
    void updateVolume(const Voice& v);
    void keyOff(Voice& v);
    uint16_t opReg(int chip, int op) const;   // register offset for operator (0 modulator, 1 carrier) of a chip channel
    uint16_t chReg(int chip) const;           // channel register offset (0xA0/0xB0/0xC0 + this)

    opl::Opl3 opl_;
    const GenMidiBank* bank_ = nullptr;
    Voice voices_[18];
    Channel channels_[16];
    uint64_t stamp_ = 0;
    uint8_t keyOnByte_[18] = {};
};

}  // namespace rl::audio
