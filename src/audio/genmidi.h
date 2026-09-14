#pragma once
// Doom's GENMIDI lump: the OPL2 instrument bank DMX used (128 melodic
// programs + 47 percussion voices for notes 35-81), each with one or two
// two-operator voices.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rl::audio {

struct GenMidiOperator {
    uint8_t tremVibSusKsrMult;   // register 0x20: AM | VIB | EG-type | KSR | MULT
    uint8_t attackDecay;         // register 0x60
    uint8_t sustainRelease;      // register 0x80
    uint8_t waveform;            // register 0xE0
    uint8_t keyScale;            // register 0x40 bits 6-7 (KSL)
    uint8_t outputLevel;         // register 0x40 bits 0-5 (TL)
};

struct GenMidiVoice {
    GenMidiOperator modulator;
    uint8_t feedbackConnection;  // register 0xC0 bits 0-3
    GenMidiOperator carrier;
    int16_t baseNoteOffset;      // added to the MIDI note
};

struct GenMidiInstrument {
    enum Flags : uint16_t { FixedPitch = 1, DelayedSecond = 2, DoubleVoice = 4 };
    uint16_t flags = 0;
    uint8_t fineTune = 128;      // second-voice detune, 128 = none
    uint8_t fixedNote = 0;       // used when FixedPitch (percussion)
    GenMidiVoice voice[2];
    std::string name;
    bool doubleVoice() const { return (flags & DoubleVoice) != 0; }
    bool fixedPitch() const { return (flags & FixedPitch) != 0; }
};

struct GenMidiBank {
    std::vector<GenMidiInstrument> instruments;   // 175: 0-127 melodic, 128-174 percussion for notes 35-81
    const GenMidiInstrument* melodic(int program) const { return program >= 0 && program < 128 && instruments.size() >= 128 ? &instruments[static_cast<size_t>(program)] : nullptr; }
    const GenMidiInstrument* percussion(int note) const {
        int i = note - 35;
        return i >= 0 && i < 47 && instruments.size() >= 175 ? &instruments[static_cast<size_t>(128 + i)] : nullptr;
    }
};

// Parses "#OPL_II#" + 175 x 36-byte instruments (+ optional 175 x 32-byte names).
std::optional<GenMidiBank> parseGenMidi(std::span<const uint8_t> data);

// A small built-in bank for when no WAD is available: a handful of plausible
// OPL patches mapped over the GM programs and drums.
GenMidiBank builtinGenMidi();

}  // namespace rl::audio
